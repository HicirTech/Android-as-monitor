#pragma once

#include "frame_source.h"
#include "panel_rate_picker.h"

#include "../shared/edid_timing.h"

#include <memory>
#include <utility>

namespace aum {

// A source Windows composes faster than the panel runs, passing on one frame per panel period.
class PanelRateSource final : public FrameSource {
public:
    PanelRateSource(std::unique_ptr<FrameSource> inner, Rational panelRefresh)
        : inner_(std::move(inner)), picker_(static_cast<std::int64_t>(1'000'000'000ull * panelRefresh.denominator / panelRefresh.numerator)) {}

    bool Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) override {
        while (inner_->Next(target, frame, stopEvent)) {
            if (picker_.Pick(frame.capturedNs)) {
                return true;
            }
        }
        return false;
    }

private:
    std::unique_ptr<FrameSource> inner_;
    PanelRatePicker picker_;
};

}  // namespace aum
