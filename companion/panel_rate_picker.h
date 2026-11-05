#pragma once

#include <cstdint>

namespace aum {

// Chooses one frame per panel period from a source that runs faster than the panel. It keeps to a fixed grid of those periods, so
// a tick the source loses costs a little timing and no frame.
class PanelRatePicker {
public:
    explicit PanelRatePicker(std::int64_t panelPeriodNs) : periodNs_(panelPeriodNs), earlyNs_(panelPeriodNs / 4) {}

    // True when a frame captured at capturedNs is the one to send for the current period.
    bool Pick(std::int64_t capturedNs) {
        if (capturedNs < nextNs_ - earlyNs_) {
            return false;
        }
        // Only after a real stall does the grid start over, so an idle desktop is never followed by a burst to catch up.
        nextNs_ = capturedNs > nextNs_ + periodNs_ ? capturedNs + periodNs_ : nextNs_ + periodNs_;
        return true;
    }

private:
    std::int64_t periodNs_;
    std::int64_t earlyNs_;
    std::int64_t nextNs_ = 0;
};

}  // namespace aum
