#include "swap_chain_processor.h"

#include "event_log.h"
#include "frame_tap.h"

using Microsoft::WRL::ComPtr;

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent,
                                       FrameTapChannel& channel)
    : swapChain_(swapChain), renderAdapter_(renderAdapter), newFrameEvent_(newFrameEvent), channel_(channel) {
    terminateEvent_.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

SwapChainProcessor::~SwapChainProcessor() {
    SetEvent(terminateEvent_.Get());
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
    }
}

bool SwapChainProcessor::Start() {
    ComPtr<IDXGIFactory5> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogEvent(L"swap chain: CreateDXGIFactory2 failed 0x%08lX", hr);
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByLuid(renderAdapter_, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
        LogEvent(L"swap chain: EnumAdapterByLuid failed 0x%08lX", hr);
        return false;
    }
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(hr)) {
        LogEvent(L"swap chain: D3D11CreateDevice failed 0x%08lX", hr);
        return false;
    }
    thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
    return thread_ != nullptr;
}

DWORD CALLBACK SwapChainProcessor::ThreadMain(LPVOID argument) {
    static_cast<SwapChainProcessor*>(argument)->Run();
    return 0;
}

void SwapChainProcessor::Run() {
    DWORD taskIndex = 0;
    const HANDLE task = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);

    ProcessFrames();

    // Deleting the swap chain tells the OS to offer a new one if the monitor is still in use.
    WdfObjectDelete(static_cast<WDFOBJECT>(swapChain_));
    swapChain_ = nullptr;

    if (task) {
        AvRevertMmThreadCharacteristics(task);
    }
}

void SwapChainProcessor::ProcessFrames() {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) {
        LogEvent(L"swap chain: IDXGIDevice query failed 0x%08lX", hr);
        return;
    }
    IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
    setDevice.pDevice = dxgiDevice.Get();
    hr = IddCxSwapChainSetDevice(swapChain_, &setDevice);
    if (FAILED(hr)) {
        LogEvent(L"swap chain: IddCxSwapChainSetDevice failed 0x%08lX", hr);
        return;
    }
    LogEvent(L"swap chain: started on adapter %08lX-%08lX", renderAdapter_.HighPart, renderAdapter_.LowPart);

    FrameTap tap(channel_, device_.Get(), context_.Get(), renderAdapter_);
    const HANDLE waitHandles[] = {newFrameEvent_, terminateEvent_.Get()};
    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(swapChain_, &buffer);
        if (hr == E_PENDING) {
            const DWORD result = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 16);
            if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT) {
                continue;
            }
            break;
        }
        if (FAILED(hr)) {
            LogEvent(L"swap chain: acquire ended 0x%08lX", hr);
            break;
        }

        LARGE_INTEGER acquired;
        QueryPerformanceCounter(&acquired);

        ComPtr<IDXGIResource> resource;
        resource.Attach(buffer.MetaData.pSurface);
        ComPtr<ID3D11Texture2D> surface;
        if (SUCCEEDED(resource.As(&surface))) {
            tap.Publish(surface.Get(), buffer.MetaData, static_cast<UINT64>(acquired.QuadPart));
        }
        surface.Reset();
        resource.Reset();

        hr = IddCxSwapChainFinishedProcessingFrame(swapChain_);
        if (FAILED(hr)) {
            LogEvent(L"swap chain: finish frame ended 0x%08lX", hr);
            break;
        }
    }
    LogEvent(L"swap chain: stopped");
}
