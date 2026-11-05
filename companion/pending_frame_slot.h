#pragma once

#include <d3d11.h>

#include <cstdint>
#include <mutex>
#include <optional>

namespace aum {

struct PendingFrame {
    ID3D11Texture2D* texture;
    std::uint64_t frameId;
    bool forceKeyframe;
};

// The one frame waiting for the encoder. The newest always wins: an occasional slow frame costs its successor a moment's
// wait rather than a visible skip, and no backlog can form behind a stalled link.
class PendingFrameSlot {
public:
    // True when this frame took the place of one that was still waiting.
    bool Put(PendingFrame frame) {
        const std::lock_guard lock(mutex_);
        const bool replaced = pending_.has_value();
        if (replaced) {
            // A keyframe request must survive: the phone drops everything until it gets the keyframe it asked for.
            frame.forceKeyframe = frame.forceKeyframe || pending_->forceKeyframe;
        }
        pending_ = frame;
        return replaced;
    }

    std::optional<PendingFrame> Take() {
        const std::lock_guard lock(mutex_);
        std::optional<PendingFrame> frame;
        frame.swap(pending_);
        return frame;
    }

private:
    std::mutex mutex_;
    std::optional<PendingFrame> pending_;
};

}  // namespace aum
