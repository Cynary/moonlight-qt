#include "directwayland.h"
#include "protocols/gamescope-swapchain-client-protocol.h"
#include "protocols/linux-dmabuf-unstable-v1-client-protocol.h"
#include "protocols/wlr-layer-shell-client-protocol.h"
#include "streaming/session.h"
#include <SDL_syswm.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
extern "C" {
#include <drm_fourcc.h>
#include <libavutil/hwcontext_vaapi.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
}

// A failed live import must not create an endless decoder-reset loop. The next
// renderer selection falls back to Vulkan; restarting Moonlight permits retry.
static std::atomic<bool> directFailed { false };
struct DirectWaylandRenderer::State {
    IFFmpegRenderer* backend;
    int mode, width = 0, height = 0;
    bool ready = false, testOnly = false, failed = false, ownsDisplay = false;
    uint32_t xWindow = 0, xServer = 0;
    std::vector<VASurfaceID> freeRgb;
    std::atomic<bool> suspended { false };
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;
    wl_event_queue* queue = nullptr;
    wl_registry* registry = nullptr;
    zwp_linux_dmabuf_v1* dma = nullptr;
    gamescope_swapchain_factory_v2* factory = nullptr;
    gamescope_swapchain* swapchain = nullptr;
    wl_compositor* compositor = nullptr;
    zwlr_layer_shell_v1* layerShell = nullptr;
    wl_surface* overlaySurface = nullptr;
    zwlr_layer_surface_v1* overlayLayer = nullptr;
    wl_shm* shm = nullptr;
    std::set<std::pair<uint32_t, uint64_t>> formats;
    AVFrame* decoded = nullptr;
    uint64_t id = 0;
    struct Sample {
        uint64_t id, time;
    };
    std::deque<Sample> samples;
    std::deque<Sample> submissions;
    static uint64_t monotonicUs()
    {
        timespec t { };
        clock_gettime(CLOCK_MONOTONIC, &t);
        return uint64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
    }
    VADisplay va = nullptr;
    AVBufferRef* device = nullptr;
    VAConfigID vppConfig = VA_INVALID_ID;
    VAContextID vppContext = VA_INVALID_ID;
    struct Buffer {
        State* owner;
        wl_buffer* proxy = nullptr;
        AVFrame* frame = nullptr;
        VASurfaceID rgb = VA_INVALID_ID;
    };
    struct Import {
        wl_buffer* buffer = nullptr;
        bool done = false;
    };
    static void imported(void* data, zwp_linux_buffer_params_v1*, wl_buffer* buffer)
    {
        auto* i = static_cast<Import*>(data);
        i->buffer = buffer;
        i->done = true;
    }
    static void importFailed(void* data, zwp_linux_buffer_params_v1*)
    {
        static_cast<Import*>(data)->done = true;
    }
    std::set<Buffer*> buffers;
    Buffer* pending = nullptr;
    uint64_t bufferSaturationSince = 0;
    struct OverlayState {
        SDL_Surface* cached = nullptr;
        std::atomic<bool> dirty { true };
    } overlays[Overlay::OverlayMax];
    struct ShmBuffer {
        State* owner;
        wl_buffer* proxy;
        void* memory;
        size_t size;
    };
    std::set<ShmBuffer*> overlayBuffers;

    void fail(const char* reason)
    {
        if (failed)
            return;
        failed = true;
        directFailed = true;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Direct Wayland: %s; switching to Vulkan", reason);
        if (!testOnly) {
            SDL_Event event { };
            event.type = SDL_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
        }
    }
    void pump(int timeoutMs = 0)
    {
        if (!display || !queue)
            return;
        if (wl_display_prepare_read_queue(display, queue) == 0) {
            wl_display_flush(display);
            pollfd fd { wl_display_get_fd(display), POLLIN, 0 };
            if (::poll(&fd, 1, timeoutMs) > 0 && (fd.revents & POLLIN))
                wl_display_read_events(display);
            else
                wl_display_cancel_read(display);
        }
        if (wl_display_dispatch_queue_pending(display, queue) < 0)
            fail("Wayland connection failed");
    }
    void drop(Buffer* b)
    {
        if (!b)
            return;
        if (b->proxy)
            wl_buffer_destroy(b->proxy);
        if (b->rgb != VA_INVALID_ID)
            freeRgb.push_back(b->rgb);
        av_frame_free(&b->frame);
        buffers.erase(b);
        delete b;
    }
    static void released(void* data, wl_buffer*)
    {
        auto* b = static_cast<Buffer*>(data);
        b->owner->drop(b);
    }
    static void shmReleased(void* data, wl_buffer*)
    {
        auto* b = static_cast<ShmBuffer*>(data);
        wl_buffer_destroy(b->proxy);
        munmap(b->memory, b->size);
        b->owner->overlayBuffers.erase(b);
        delete b;
    }
    static void format(void* data, zwp_linux_dmabuf_v1*, uint32_t f)
    {
        static_cast<State*>(data)->formats.insert({ f, DRM_FORMAT_MOD_INVALID });
    }
    static void modifier(void* data, zwp_linux_dmabuf_v1*, uint32_t f, uint32_t hi, uint32_t lo)
    {
        static_cast<State*>(data)->formats.insert({ f, (uint64_t(hi) << 32) | lo });
    }
    static void global(void* data, wl_registry* r, uint32_t name, const char* interface, uint32_t version)
    {
        auto* s = static_cast<State*>(data);
        if (!strcmp(interface, "zwp_linux_dmabuf_v1")) {
            s->dma = static_cast<zwp_linux_dmabuf_v1*>(
                wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u)));
            static const zwp_linux_dmabuf_v1_listener listener { format, modifier };
            zwp_linux_dmabuf_v1_add_listener(s->dma, &listener, s);
        } else if (!strcmp(interface, "gamescope_swapchain_factory_v2"))
            s->factory = static_cast<gamescope_swapchain_factory_v2*>(
                wl_registry_bind(r, name, &gamescope_swapchain_factory_v2_interface, 1));
        else if (!strcmp(interface, "wl_compositor"))
            s->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(r, name, &wl_compositor_interface, std::min(version, 4u)));
        else if (!strcmp(interface, "zwlr_layer_shell_v1"))
            s->layerShell = static_cast<zwlr_layer_shell_v1*>(
                wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1));
        else if (!strcmp(interface, "wl_shm"))
            s->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
    }
    static void removed(void*, wl_registry*, uint32_t) { }
    static void past(void* data, gamescope_swapchain*, uint32_t id, uint32_t, uint32_t, uint32_t hi,
        uint32_t lo, uint32_t, uint32_t, uint32_t, uint32_t)
    {
        auto* s = static_cast<State*>(data);
        if (s->samples.size() == 32)
            s->samples.pop_front();
        s->samples.push_back({ id, ((uint64_t(hi) << 32) | lo) / 1000 });
    }
    static void cycle(void*, gamescope_swapchain*, uint32_t, uint32_t) { }
    static void retired(void* data, gamescope_swapchain*)
    {
        static_cast<State*>(data)->fail("swapchain retired");
    }
    bool convertRgb(AVFrame* frame, Buffer* b)
    {
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        auto* ctx = reinterpret_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
        if (!va) {
            va = ctx->display;
            device = av_buffer_ref(frames->device_ref);
        }
        if (va != ctx->display || !device)
            return false;
        if (vppContext == VA_INVALID_ID) {
            if (vaCreateConfig(va, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &vppConfig)
                != VA_STATUS_SUCCESS)
                return false;
            if (vaCreateContext(va, vppConfig, width, height, VA_PROGRESSIVE, nullptr, 0, &vppContext)
                != VA_STATUS_SUCCESS)
                return false;
        }
        VASurfaceAttrib attr[2] { };
        attr[0].type = VASurfaceAttribPixelFormat;
        attr[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attr[0].value.type = VAGenericValueTypeInteger;
        attr[0].value.value.i = VA_FOURCC_A2B10G10R10;
        attr[1].type = VASurfaceAttribUsageHint;
        attr[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
        attr[1].value.type = VAGenericValueTypeInteger;
        attr[1].value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE;
        if (!freeRgb.empty()) {
            b->rgb = freeRgb.back();
            freeRgb.pop_back();
        } else if (vaCreateSurfaces(va, VA_RT_FORMAT_RGB32_10, width, height, &b->rgb, 1, attr, 2)
            != VA_STATUS_SUCCESS)
            return false;
        VAProcPipelineParameterBuffer p { };
        p.surface = VASurfaceID(uintptr_t(frame->data[3]));
        p.surface_color_standard = VAProcColorStandardBT2020;
        p.output_color_standard = VAProcColorStandardBT2020;
        p.input_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
        p.output_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        VABufferID param = VA_INVALID_ID;
        if (vaCreateBuffer(va, vppContext, VAProcPipelineParameterBufferType, sizeof(p), 1, &p, &param)
            != VA_STATUS_SUCCESS)
            return false;
        bool ok = vaBeginPicture(va, vppContext, b->rgb) == VA_STATUS_SUCCESS;
        if (ok) {
            const auto status = vaRenderPicture(va, vppContext, &param, 1);
            const auto end = vaEndPicture(va, vppContext);
            ok = status == VA_STATUS_SUCCESS && end == VA_STATUS_SUCCESS;
        }
        vaDestroyBuffer(va, param);
        return ok && vaSyncSurface(va, b->rgb) == VA_STATUS_SUCCESS;
    }
    Buffer* makeBuffer(AVFrame* frame)
    {
        if (!frame || frame->format != AV_PIX_FMT_VAAPI || !frame->hw_frames_ctx)
            return nullptr;
        if ((frame->color_trc != AVCOL_TRC_UNSPECIFIED && frame->color_trc != AVCOL_TRC_SMPTE2084)
            || frame->color_range == AVCOL_RANGE_JPEG
            || (frame->colorspace != AVCOL_SPC_UNSPECIFIED && frame->colorspace != AVCOL_SPC_BT2020_NCL))
            return nullptr;
        auto* b = new Buffer { this };
        buffers.insert(b);
        b->frame = av_frame_clone(frame);
        if (!b->frame) {
            drop(b);
            return nullptr;
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        auto* ctx = reinterpret_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
        if (mode == 2 && !convertRgb(frame, b)) {
            drop(b);
            return nullptr;
        }
        VADRMPRIMESurfaceDescriptor desc { };
        VASurfaceID target = mode == 2 ? b->rgb : VASurfaceID(uintptr_t(frame->data[3]));
        if (vaExportSurfaceHandle(ctx->display, target, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc)
            != VA_STATUS_SUCCESS) {
            drop(b);
            return nullptr;
        }
        uint32_t format = desc.num_layers == 1 ? desc.layers[0].drm_format : 0;
        if (format == DRM_FORMAT_Y410)
            format = DRM_FORMAT_XVYU2101010;
        bool valid = desc.num_layers == 1 && desc.num_objects > 0 && desc.num_objects <= 4
            && desc.layers[0].num_planes > 0 && desc.layers[0].num_planes <= 4;
        valid = valid && (mode == 2 ? format == DRM_FORMAT_ABGR2101010 : format == DRM_FORMAT_XVYU2101010);
        for (uint32_t i = 0; valid && i < desc.layers[0].num_planes; i++) {
            auto o = desc.layers[0].object_index[i];
            valid = o < desc.num_objects && formats.count({ format, desc.objects[o].drm_format_modifier });
        }
        if (valid) {
            auto* p = zwp_linux_dmabuf_v1_create_params(dma);
            for (uint32_t i = 0; i < desc.layers[0].num_planes; i++) {
                auto o = desc.layers[0].object_index[i];
                auto mod = desc.objects[o].drm_format_modifier;
                zwp_linux_buffer_params_v1_add(p, desc.objects[o].fd, i, desc.layers[0].offset[i],
                    desc.layers[0].pitch[i], mod >> 32, uint32_t(mod));
            }
            Import importedBuffer;
            static const zwp_linux_buffer_params_v1_listener importListener { imported, importFailed };
            zwp_linux_buffer_params_v1_add_listener(p, &importListener, &importedBuffer);
            zwp_linux_buffer_params_v1_create(p, frame->width, frame->height, format, 0);
            uint64_t deadline = LiGetMicroseconds() + 50000;
            while (!importedBuffer.done && !failed && !suspended && LiGetMicroseconds() < deadline)
                pump(5);
            b->proxy = importedBuffer.buffer;
            zwp_linux_buffer_params_v1_destroy(p);
            valid = b->proxy != nullptr;
            if (valid) {
                static const wl_buffer_listener listener { released };
                wl_buffer_add_listener(b->proxy, &listener, b);
            }
        }
        for (uint32_t i = 0; i < desc.num_objects; i++)
            close(desc.objects[i].fd);
        if (!valid) {
            drop(b);
            return nullptr;
        }
        return b;
    }
    static void layerConfigure(void*, zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t, uint32_t)
    {
        zwlr_layer_surface_v1_ack_configure(layer, serial);
    }
    static void layerClosed(void* data, zwlr_layer_surface_v1*)
    {
        static_cast<State*>(data)->fail("statistics overlay closed by compositor");
    }
    void destroyOverlay()
    {
        if (overlayLayer)
            zwlr_layer_surface_v1_destroy(overlayLayer);
        if (overlaySurface)
            wl_surface_destroy(overlaySurface);
        overlayLayer = nullptr;
        overlaySurface = nullptr;
    }
    void updateOverlays()
    {
        auto& manager = Session::get()->getOverlayManager();
        bool changed = false, visible = false;
        for (int i = 0; i < Overlay::OverlayMax; i++) {
            auto& o = overlays[i];
            auto type = static_cast<Overlay::OverlayType>(i);
            if (o.dirty.exchange(false)) {
                changed = true;
                auto* src = manager.getUpdatedOverlaySurface(type);
                if (src) {
                    SDL_FreeSurface(o.cached);
                    o.cached = src;
                }
            }
            visible |= manager.isOverlayEnabled(type) && o.cached;
        }
        if (!changed)
            return;
        if (!visible) {
            destroyOverlay();
            return;
        }
        auto* rgba = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!rgba)
            return;
        SDL_FillRect(rgba, nullptr, 0);
        int paintedRight = 0, paintedTop = height, paintedBottom = 0;
        for (int i = 0; i < Overlay::OverlayMax; i++) {
            auto* src = overlays[i].cached;
            if (!src || !manager.isOverlayEnabled(static_cast<Overlay::OverlayType>(i)))
                continue;
            SDL_Rect dst { 0, i == Overlay::OverlayDebug ? 0 : std::max(0, height - src->h), src->w, src->h };
            SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
            SDL_BlitSurface(src, nullptr, rgba, &dst);
            paintedRight = std::max(paintedRight, std::min(width, dst.x + dst.w));
            paintedTop = std::min(paintedTop, dst.y);
            paintedBottom = std::max(paintedBottom, std::min(height, dst.y + dst.h));
        }
        if (!overlaySurface) {
            overlaySurface = wl_compositor_create_surface(compositor);
            auto* input = wl_compositor_create_region(compositor);
            wl_surface_set_input_region(overlaySurface, input);
            wl_region_destroy(input);
            overlayLayer = zwlr_layer_shell_v1_get_layer_surface(layerShell, overlaySurface, nullptr,
                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "moonlight-statistics");
            static const zwlr_layer_surface_v1_listener listener { layerConfigure, layerClosed };
            zwlr_layer_surface_v1_add_listener(overlayLayer, &listener, this);
            zwlr_layer_surface_v1_set_size(overlayLayer, width, height);
            zwlr_layer_surface_v1_set_anchor(
                overlayLayer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
            zwlr_layer_surface_v1_set_exclusive_zone(overlayLayer, -1);
            zwlr_layer_surface_v1_set_keyboard_interactivity(overlayLayer, 0);
            wl_surface_commit(overlaySurface);
            if (wl_display_roundtrip_queue(display, queue) < 0) {
                SDL_FreeSurface(rgba);
                fail("statistics overlay configuration failed");
                return;
            }
        }
        if (overlayBuffers.size() >= 16) {
            SDL_FreeSurface(rgba);
            fail("overlay buffers not released");
            return;
        }
        size_t size = size_t(rgba->pitch) * rgba->h;
        char name[] = "/tmp/moonlight-overlay-XXXXXX";
        int fd = mkstemp(name);
        unlink(name);
        if (fd < 0 || ftruncate(fd, size) < 0) {
            if (fd >= 0)
                close(fd);
            SDL_FreeSurface(rgba);
            return;
        }
        void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) {
            close(fd);
            SDL_FreeSurface(rgba);
            return;
        }
        memcpy(memory, rgba->pixels, size);
        // Wayland SHM ARGB is premultiplied. SDL text surfaces are straight alpha.
        // The untouched canvas is already transparent black. Convert only the
        // painted bounds, avoiding a CPU pass over every pixel of a 4K frame.
        for (int y = paintedTop; y < paintedBottom; y++)
            for (int x = 0; x < paintedRight; x++) {
                auto* p = reinterpret_cast<uint32_t*>(static_cast<char*>(memory) + y * rgba->pitch) + x;
                uint32_t v = *p, a = v >> 24;
                *p = (a << 24) | ((((v >> 16) & 255) * a / 255) << 16) | ((((v >> 8) & 255) * a / 255) << 8)
                    | ((v & 255) * a / 255);
            }
        auto* pool = wl_shm_create_pool(shm, fd, size);
        close(fd);
        auto* buffer
            = wl_shm_pool_create_buffer(pool, 0, rgba->w, rgba->h, rgba->pitch, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        auto* b = new ShmBuffer { this, buffer, memory, size };
        overlayBuffers.insert(b);
        static const wl_buffer_listener listener { shmReleased };
        wl_buffer_add_listener(buffer, &listener, b);
        wl_surface_attach(overlaySurface, buffer, 0, 0);
        wl_surface_damage(overlaySurface, 0, 0, rgba->w, rgba->h);
        wl_surface_commit(overlaySurface);
        SDL_FreeSurface(rgba);
    }
    ~State()
    {
        av_frame_free(&decoded);
        if (surface && ready) {
            wl_surface_attach(surface, nullptr, 0, 0);
            wl_surface_commit(surface);
            wl_display_flush(display);
        }
        destroyOverlay();
        for (auto& o : overlays)
            SDL_FreeSurface(o.cached);
        if (swapchain)
            gamescope_swapchain_destroy(swapchain);
        // Destroying client proxies does not prove the compositor released its
        // imported storage, but DMA-BUF/SHM objects hold kernel references. No
        // decoder remains running when renderer destruction frees these frames.
        while (!buffers.empty())
            drop(*buffers.begin());
        while (!overlayBuffers.empty())
            shmReleased(*overlayBuffers.begin(), nullptr);
        for (auto surface : freeRgb)
            vaDestroySurfaces(va, &surface, 1);
        if (vppContext != VA_INVALID_ID)
            vaDestroyContext(va, vppContext);
        if (vppConfig != VA_INVALID_ID)
            vaDestroyConfig(va, vppConfig);
        av_buffer_unref(&device);
        if (factory)
            gamescope_swapchain_factory_v2_destroy(factory);
        if (dma)
            zwp_linux_dmabuf_v1_destroy(dma);
        if (shm)
            wl_shm_destroy(shm);
        if (layerShell)
            zwlr_layer_shell_v1_destroy(layerShell);
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
        if (ownsDisplay && surface)
            wl_surface_destroy(surface);
        if (queue)
            wl_event_queue_destroy(queue);
        if (ownsDisplay && display)
            wl_display_disconnect(display);
    }
};

DirectWaylandRenderer::DirectWaylandRenderer(IFFmpegRenderer* backend, int mode)
    : IFFmpegRenderer(RendererType::DirectWayland)
    , d(new State)
{
    d->backend = backend;
    d->mode = mode == 2 ? 2 : 1;
}
DirectWaylandRenderer::~DirectWaylandRenderer() = default;
int DirectWaylandRenderer::getDecoderCapabilities() { return d->backend->getDecoderCapabilities(); }
QString DirectWaylandRenderer::getCalibrationIdentity()
{
    return QStringLiteral("Gamescope-direct-%1-v1").arg(d->mode == 2 ? "rgb" : "yuv");
}
bool DirectWaylandRenderer::initialize(PDECODER_PARAMETERS p)
{
    if (directFailed || p->videoFormat != VIDEO_FORMAT_H265_REXT10_444
        || d->backend->getRendererType() != RendererType::VAAPI)
        return false;
    SDL_SysWMinfo info { };
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(p->window, &info))
        return false;
    if (info.subsystem == SDL_SYSWM_WAYLAND) {
        d->display = info.info.wl.display;
        d->surface = info.info.wl.surface;
    }
#ifdef SDL_VIDEO_DRIVER_X11
    else if (info.subsystem == SDL_SYSWM_X11) {
        Display* xd = info.info.x11.display;
        Atom atom = XInternAtom(xd, "GAMESCOPE_XWAYLAND_SERVER_ID", True), actual = 0;
        int bits = 0;
        unsigned long count = 0, left = 0;
        unsigned char* value = nullptr;
        if (!atom
            || XGetWindowProperty(xd, DefaultRootWindow(xd), atom, 0, 1, False, AnyPropertyType, &actual,
                   &bits, &count, &left, &value)
                != Success)
            return false;
        if (bits != 32 || count != 1) {
            if (value)
                XFree(value);
            return false;
        }
        d->xServer = *reinterpret_cast<unsigned long*>(value);
        XFree(value);
        d->xWindow = info.info.x11.window;
        const char* name = getenv("GAMESCOPE_WAYLAND_DISPLAY");
        d->display = wl_display_connect(name ? name : "gamescope-0");
        d->ownsDisplay = true;
    }
#endif
    else
        return false;
    if (!d->display)
        return false;
    int w = 0, h = 0;
    SDL_GetWindowSize(p->window, &w, &h);
    if (!p->testOnly && (w != p->width || h != p->height))
        return false;
    d->width = p->width;
    d->height = p->height;
    d->testOnly = p->testOnly;
    d->queue = wl_display_create_queue(d->display);
    if (!d->queue)
        return false;
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(d->display));
    if (!wrapper)
        return false;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), d->queue);
    d->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    static const wl_registry_listener listener { State::global, State::removed };
    wl_registry_add_listener(d->registry, &listener, d.get());
    if (wl_display_roundtrip_queue(d->display, d->queue) < 0
        || wl_display_roundtrip_queue(d->display, d->queue) < 0)
        return false;
    if (!d->factory || !d->dma || !d->shm || !d->compositor || !d->layerShell)
        return false;
    uint32_t format = d->mode == 2 ? DRM_FORMAT_ABGR2101010 : DRM_FORMAT_XVYU2101010;
    if (std::none_of(d->formats.begin(), d->formats.end(), [format](auto f) { return f.first == format; }))
        return false;
    if (d->ownsDisplay)
        d->surface = wl_compositor_create_surface(d->compositor);
    if (!p->testOnly) {
        d->swapchain = gamescope_swapchain_factory_v2_create_swapchain(d->factory, d->surface);
        static const gamescope_swapchain_listener sl { State::past, State::cycle, State::retired };
        gamescope_swapchain_add_listener(d->swapchain, &sl, d.get());
        if (d->xWindow)
            gamescope_swapchain_override_window_content(d->swapchain, d->xServer, d->xWindow);
        gamescope_swapchain_swapchain_feedback(
            d->swapchain, 3, 64, 1000104008, 1, 1, 1, "Moonlight direct video");
        SS_HDR_METADATA hdr { };
        if (LiGetHdrMetadata(&hdr))
            gamescope_swapchain_set_hdr_metadata(d->swapchain, hdr.displayPrimaries[0].x,
                hdr.displayPrimaries[0].y, hdr.displayPrimaries[1].x, hdr.displayPrimaries[1].y,
                hdr.displayPrimaries[2].x, hdr.displayPrimaries[2].y, hdr.whitePoint.x, hdr.whitePoint.y,
                hdr.maxDisplayLuminance, hdr.minDisplayLuminance, hdr.maxContentLightLevel,
                hdr.maxFrameAverageLightLevel);
        gamescope_swapchain_set_present_mode(d->swapchain, 2); // FIFO: Gamescope owns synchronized VRR flips.
    }
    d->ready = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Direct Wayland: %s HDR presentation initialized",
        d->mode == 2 ? "RGB" : "YUV");
    return true;
}
bool DirectWaylandRenderer::testRenderFrame(AVFrame* f)
{
    waitForDecode(f);
    auto* b = d->makeBuffer(f);
    if (!b)
        return false;
    d->drop(b);
    av_frame_free(&d->decoded);
    return true;
}
uint64_t DirectWaylandRenderer::waitForDecode(AVFrame* f)
{
    if (!f || f->format != AV_PIX_FMT_VAAPI || !f->hw_frames_ctx)
        return 0;
    if (d->decoded && d->decoded->data[3] == f->data[3]
        && d->decoded->hw_frames_ctx->data == f->hw_frames_ctx->data)
        return 0;
    av_frame_free(&d->decoded);
    uint64_t start = LiGetMicroseconds();
    auto* frames = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    auto* ctx = reinterpret_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
    if (vaSyncSurface(ctx->display, VASurfaceID(uintptr_t(f->data[3]))) == VA_STATUS_SUCCESS)
        d->decoded = av_frame_clone(f);
    else
        d->fail("decoder synchronization failed");
    return LiGetMicroseconds() - start;
}
VrrFallbackReason DirectWaylandRenderer::checkSupport() const
{
    return d->ready && !d->failed ? VrrFallbackReason::NoFallback : VrrFallbackReason::UnsupportedRenderer;
}
VrrPrepareResult DirectWaylandRenderer::prepareFrame(AVFrame* f, uint64_t)
{
    VrrPrepareResult result;
    if (d->suspended || d->failed || d->pending)
        return result;
    d->pump();
    result.decodeSyncUs = waitForDecode(f);
    uint64_t start = LiGetMicroseconds();
    if (!d->decoded) {
        d->fail("decoded surface is not ready");
        return result;
    }
    if (!f || f->width != d->width || f->height != d->height) {
        d->fail("decoded frame dimensions changed");
        return result;
    }
    if (d->buffers.size() >= 8) {
        // A screenshot or composition transition can temporarily retain all
        // buffers. Drop this incoming frame instead of growing the queue or
        // permanently disabling direct presentation after one congested frame.
        const uint64_t now = LiGetMicroseconds();
        if (!d->bufferSaturationSince) {
            d->bufferSaturationSince = now;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Direct Wayland: video buffer pool busy");
        }
        else if (now - d->bufferSaturationSince >= 250000)
            d->fail("compositor retained the full video buffer pool for 250 ms");
        av_frame_free(&d->decoded);
        return result;
    }
    if (d->bufferSaturationSince) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Direct Wayland: video buffer pool recovered after %llu us",
            static_cast<unsigned long long>(LiGetMicroseconds() - d->bufferSaturationSince));
        d->bufferSaturationSince = 0;
    }
    d->pending = d->makeBuffer(f);
    av_frame_free(&d->decoded);
    if (!d->pending) {
        d->fail("buffer import or RGB conversion unavailable");
        return result;
    }
    d->updateOverlays();
    result.renderUs = LiGetMicroseconds() - start;
    result.timingValid = true;
    result.prepared = true;
    result.sourceFrameReusable = true; // Our buffer owns an independent AVFrame reference.
    return result;
}
VrrPresentFeedback DirectWaylandRenderer::presentAdaptive(const VrrPresentRequest&)
{
    VrrPresentFeedback result;
    if (!d->pending || d->suspended || d->failed)
        return cancelFrame();
    d->pump();
    uint64_t id = ++d->id;
    gamescope_swapchain_set_present_time(d->swapchain, uint32_t(id), 0, 0);
    if (d->submissions.size() == 32)
        d->submissions.pop_front();
    d->submissions.push_back({ id, State::monotonicUs() });
    result.submissionTimeUs = LiGetMicroseconds();
    wl_surface_attach(d->surface, d->pending->proxy, 0, 0);
    wl_surface_damage(d->surface, 0, 0, d->width, d->height);
    wl_surface_commit(d->surface);
    d->pending = nullptr;
    wl_display_flush(d->display);
    result.nativeBackendValid = true;
    result.nativeBackend = VrrNativePresentationBackend::Composition;
    result.presented = true;
    result.submissionTimeValid = true;
    result.submissionIdValid = true;
    result.submissionId = id;
    if (!d->samples.empty()) {
        auto sample = d->samples.front();
        d->samples.pop_front();
        // Protocol timestamps use CLOCK_MONOTONIC; Limelight may use a different origin.
        const uint64_t before = State::monotonicUs();
        const uint64_t now = LiGetMicroseconds();
        const uint64_t after = State::monotonicUs();
        const uint64_t mono = before + (after - before) / 2;
        auto submission = std::find_if(d->submissions.begin(), d->submissions.end(),
            [&](auto record) { return record.id == sample.id; });
        if (submission != d->submissions.end() && sample.time >= submission->time && after - before <= 100
            && sample.time <= mono && mono - sample.time < 100000 && now >= mono - sample.time) {
            result.latchSampleValid = true;
            result.latchTimeKind = Vrr13::PresentationTimeKind::DisplayEvent;
            result.latchSubmissionId = sample.id;
            result.latchTimeUs = now - (mono - sample.time);
            result.presentationUncertaintyUs = (after - before + 1) / 2 + 1;
        }
        if (submission != d->submissions.end())
            d->submissions.erase(submission);
    }
    return result;
}
VrrPresentFeedback DirectWaylandRenderer::cancelFrame()
{
    d->drop(d->pending);
    d->pending = nullptr;
    av_frame_free(&d->decoded);
    VrrPresentFeedback f;
    f.cancelled = true;
    return f;
}
void DirectWaylandRenderer::renderFrame(AVFrame* f)
{
    if (prepareFrame(f, 0).prepared)
        presentAdaptive({ });
}
void DirectWaylandRenderer::setSuspended(bool value) { d->suspended = value; }
bool DirectWaylandRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO p)
{
    if (p->stateChangeFlags & WINDOW_STATE_CHANGE_SUSPENDED)
        d->suspended = true;
    if (p->stateChangeFlags & WINDOW_STATE_CHANGE_RESTORED)
        d->suspended = false;
    return !(p->stateChangeFlags & (WINDOW_STATE_CHANGE_SIZE | WINDOW_STATE_CHANGE_DISPLAY));
}
void DirectWaylandRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    d->overlays[type].dirty = true;
}
