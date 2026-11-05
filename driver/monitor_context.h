#pragma once

#include "driver_includes.h"
#include "frame_tap_channel.h"
#include "swap_chain_processor.h"

#include "../shared/edid_timing.h"

#include <memory>

class MonitorContext {
public:
    MonitorContext(const aum::DetailedTiming& timing, FrameTapChannel& channel) : timing_(timing), channel_(channel) {}

    const aum::DetailedTiming& Timing() const { return timing_; }

    void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain() { processor_.reset(); }

private:
    aum::DetailedTiming timing_;
    FrameTapChannel& channel_;
    std::unique_ptr<SwapChainProcessor> processor_;
};
