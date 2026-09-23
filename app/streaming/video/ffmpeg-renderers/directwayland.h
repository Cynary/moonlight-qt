#pragma once
#include "ivrrframepresenter.h"
#include "renderer.h"
#include <memory>

// Gamescope-only native-resolution HDR frontend. Decoder ownership remains in
// FFmpeg; each submitted buffer holds a frame reference until compositor release.
class DirectWaylandRenderer final : public IFFmpegRenderer, public IVrrFramePresenter {
public:
    DirectWaylandRenderer(IFFmpegRenderer* backend, int mode);
    ~DirectWaylandRenderer() override;
    bool initialize(PDECODER_PARAMETERS params) override;
    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override { return true; }
    bool testRenderFrame(AVFrame*) override;
    void renderFrame(AVFrame*) override;
    uint64_t waitForDecode(AVFrame*) override;
    VrrPrepareResult prepareFrame(AVFrame*, uint64_t) override;
    VrrPresentFeedback presentAdaptive(const VrrPresentRequest&) override;
    VrrPresentFeedback cancelFrame() override;
    void setSuspended(bool) override;
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;
    void notifyOverlayUpdated(Overlay::OverlayType) override;
    IVrrFramePresenter* getVrrFramePresenter() override { return this; }
    VrrFallbackReason checkSupport() const override;
    bool canLatchAdaptivePresent() const override { return true; }
    bool restoreFixedPresentation(VrrFallbackReason) override { return true; }
    int getRendererAttributes() override
    {
        return RENDERER_ATTRIBUTE_HDR_SUPPORT | RENDERER_ATTRIBUTE_FULLSCREEN_ONLY;
    }
    int getDecoderColorspace() override { return COLORSPACE_REC_2020; }
    int getDecoderColorRange() override { return COLOR_RANGE_LIMITED; }
    int getDecoderCapabilities() override;
    QString getCalibrationIdentity() override;

private:
    struct State;
    std::unique_ptr<State> d;
};
