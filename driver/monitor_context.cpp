#include "monitor_context.h"

#include "event_log.h"

void MonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    processor_.reset();

    auto processor = std::make_unique<SwapChainProcessor>(swapChain, renderAdapter, newFrameEvent, channel_);
    if (processor->Start()) {
        processor_ = std::move(processor);
        return;
    }
    // Deleting the swap chain asks the OS to generate a new one and try again.
    LogEvent(L"monitor: swap chain start failed");
    WdfObjectDelete(swapChain);
}
