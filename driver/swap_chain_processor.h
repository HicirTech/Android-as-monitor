#pragma once

#include "driver_includes.h"
#include "frame_tap_channel.h"

class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent, FrameTapChannel& channel);
    ~SwapChainProcessor();

    SwapChainProcessor(const SwapChainProcessor&) = delete;
    SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

    bool Start();

private:
    static DWORD CALLBACK ThreadMain(LPVOID argument);
    void Run();
    void ProcessFrames();

    IDDCX_SWAPCHAIN swapChain_;
    LUID renderAdapter_;
    HANDLE newFrameEvent_;
    FrameTapChannel& channel_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::Wrappers::Event terminateEvent_;
    HANDLE thread_ = nullptr;
};
