#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/predictivedrop.h"
#include <cassert>
#include <cstdio>

int main() {
    for (int scenario = 0; scenario < 7; ++scenario) {
        VrrPredictiveDrop policy;
        unsigned drops = 0, dropsAfterGap = 0;
        bool previousDrop = false;
        uint64_t lastFlip = 0, lastId = 0;
        for (uint64_t n = 0; n < 3000; ++n) {
            const uint64_t source = 1000000 + n * 8621 +
                (scenario == 3 && n > 1500 ? (n - 1500) * 8000 : 0) +
                (scenario == 6 ? ((n + 20) / 100) * 8621 : 0);
            const uint64_t delay = ((scenario == 1 || scenario == 2) && n % 100 == 80) ||
                (scenario == 2 && n % 100 == 81) ||
                (scenario == 5 && (n % 100 == 80 || n % 100 == 84)) ||
                (scenario == 6 && n % 100 == 81) ? 4500 : 0;
            const uint64_t now = source + 5000 + delay;
            bool dropped = policy.shouldDrop({now, now, source, 8621, 8621, 8333,
                                               now, source + 5000, false});
            assert(!dropped || !previousDrop);
            previousDrop = dropped;
            if (dropped) { ++drops; if (scenario == 6 && n % 100 == 81) ++dropsAfterGap; continue; }
            policy.presented(n + 1, now, 8333);
            // Feedback about the preceding frame arrives at this submission.
            if (scenario != 4 && lastId) policy.displayed(lastId, lastFlip, now, 8333);
            lastFlip = std::max(now + 250, lastFlip + 8333);
            lastId = n + 1;
        }
        assert(drops <= 150);
        // Two separate late arrivals four frames apart must both be eligible.
        // The former 20-period cooldown rejects the second one.
        if (scenario == 5) assert(drops >= 60);
        // One double-length interval must not erase the learned window.
        if (scenario == 6) assert(dropsAfterGap == 30);
        if (scenario == 0 || scenario == 3 || scenario == 4) assert(drops == 0);
        else assert(drops > 0);
        policy.reset();
        assert(!policy.shouldDrop({10000000,10000000,9990000,8621,8621,8333,10000000,9995000,false}));
    }
    puts("Predictive drop: steady, isolated/paired delays, slowdown, missing feedback, closely repeated lateness, gap retention, reset passed");
}
