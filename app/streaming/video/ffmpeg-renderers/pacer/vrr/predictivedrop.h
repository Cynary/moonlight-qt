#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <vector>

// Worker-owned, opt-in admission policy. It never skips decoding or changes
// presentation mode. Only confirmed display events anchor the pending FIFO.
class VrrPredictiveDrop {
public:
    struct Input {
        uint64_t now, ready, source, period, interval, displayPeriod, target, originalTarget;
        bool discontinuity;
    };
    void reset() { *this = VrrPredictiveDrop {}; }
    void presented(uint64_t id, uint64_t when, uint64_t period) {
        if (pending.size() >= 16) { reset(); return; }
        uint64_t after = pending.empty() ? lastFlip : pending.back().expected;
        pending.push_back({id, when, std::max(when, after ? after + period : when)});
        protectedNext = false;
    }
    void displayed(uint64_t id, uint64_t when, uint64_t observed, uint64_t period) {
        if (!when || when > observed || (lastFlip && (id <= lastId || when < lastFlip))) return;
        auto it = std::find_if(pending.begin(), pending.end(), [id](const Pending& p) { return p.id == id; });
        if (it == pending.end()) return;
        pending.erase(pending.begin(), std::next(it));
        lastFlip = when; lastId = id; ++confirmed;
        uint64_t after = when;
        for (auto& p : pending) { p.expected = std::max(p.submitted, after + period); after = p.expected; }
    }
    bool shouldDrop(const Input& i) {
        // A longer source interval is an ordinary observation, not a new
        // clock epoch. Preserve both the rolling fit and confirmed FIFO state.
        if (i.discontinuity || (lastSource && i.source && i.source < lastSource)) {
            reset();
        }
        if (!i.period || !i.source || !i.displayPeriod || i.source == lastSource) return false;
        lastSource = i.source;
        // The fit uses earlier frames only; this late sample cannot move its
        // own expected readiness later and hide its lateness.
        auto sorted = std::vector<int64_t>(offsets.begin(), offsets.end());
        std::sort(sorted.begin(), sorted.end());
        const int64_t offset = int64_t(i.ready) - int64_t(i.source);
        offsets.push_back(offset);
        if (offsets.size() > 120) offsets.pop_front();
        if (protectedNext || sorted.size() < 32 || confirmed < 8 || !lastFlip ||
            i.now < lastFlip || i.now - lastFlip > i.displayPeriod * 6 ||
            i.period < i.displayPeriod * 9 / 10 || i.period > i.displayPeriod * 2) return false;
        long double sum = 0;
        for (const auto value : sorted) sum += value;
        const int64_t typical = static_cast<int64_t>(sum / sorted.size());
        const uint64_t uncertainty = uint64_t(std::max<int64_t>(1000,
            sorted[sorted.size() * 9 / 10] - typical));
        if (uncertainty > i.period / 2) return false;
        const int64_t nextReadySigned = int64_t(i.source) + typical + int64_t(i.period);
        if (nextReadySigned <= 0) return false;
        const uint64_t nextReady = std::max(uint64_t(nextReadySigned), i.originalTarget + i.period);
        // Don't predict through an already missed successor. Its arrival is
        // no longer trustworthy; existing queue-overload handling owns that.
        if (nextReady <= i.now) return false;
        const uint64_t occupiedUntil = pending.empty() ? lastFlip + i.displayPeriod : pending.back().expected + i.displayPeriod;
        const uint64_t currentFlip = std::max({i.now + 250, i.target, occupiedUntil});
        const bool late = offset > typical + int64_t(uncertainty);
        const bool queuedBehindDisplay = currentFlip > std::max(i.now + 250, i.target) + i.displayPeriod / 2;
        if (!late && !queuedBehindDisplay) return false;
        const uint64_t nextWithout = std::max(nextReady + 250, occupiedUntil);
        const uint64_t nextWith = std::max(nextWithout, currentFlip + i.displayPeriod);
        if (nextWith - nextWithout <= std::max<uint64_t>(2000, uncertainty)) return false;
        protectedNext = true;
        return true;
    }
private:
    struct Pending { uint64_t id, submitted, expected; };
    std::deque<Pending> pending;
    std::deque<int64_t> offsets;
    uint64_t lastFlip = 0, lastId = 0, confirmed = 0, lastSource = 0;
    bool protectedNext = false;
};
