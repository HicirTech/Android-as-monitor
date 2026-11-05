#include "motion_window.h"

#include "win_error.h"

#include <cstdio>
#include <exception>

using Microsoft::WRL::ComPtr;

namespace aum {

static constexpr wchar_t kWindowClass[] = L"AumMotionWindow";
static constexpr LONG kBarWidth = 64;
static constexpr LONG kBarStep = 8;

MotionWindow::MotionWindow(LUID adapter, const RECT& desktopRect) : adapter_(adapter), rect_(desktopRect) {
    thread_ = std::thread([this] {
        try {
            Run();
        } catch (const std::exception& error) {
            std::printf("motion window: %s\n", error.what());
        }
    });
}

MotionWindow::~MotionWindow() {
    stopping_ = true;
    thread_.join();
}

void MotionWindow::Run() {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassW(&windowClass);

    const LONG width = rect_.right - rect_.left;
    const LONG height = rect_.bottom - rect_.top;
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"Android USB Monitor test", WS_POPUP | WS_VISIBLE,
                                        rect_.left, rect_.top, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) {
        throw std::runtime_error("cannot create the motion window");
    }

    GpuDevice gpu = CreateGpuDevice(adapter_);
    ComPtr<IDXGIDevice> dxgiDevice;
    ThrowIfFailed(gpu.device.As(&dxgiDevice), "IDXGIDevice query");
    ComPtr<IDXGIAdapter> adapter;
    ThrowIfFailed(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(gpu.device.Get(), window, &desc, nullptr, nullptr, &swapChain), "CreateSwapChainForHwnd");

    ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "GetBuffer");
    ComPtr<ID3D11RenderTargetView> view;
    ThrowIfFailed(gpu.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &view), "CreateRenderTargetView");
    ComPtr<ID3D11DeviceContext1> context;
    ThrowIfFailed(gpu.context.As(&context), "ID3D11DeviceContext1 query");

    static constexpr float kBackground[4] = {0.05f, 0.05f, 0.05f, 1.0f};
    static constexpr float kBar[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    LONG barLeft = 0;
    while (!stopping_) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&message);
        }
        context->ClearRenderTargetView(view.Get(), kBackground);
        const D3D11_RECT bar{barLeft, 0, barLeft + kBarWidth, height};
        context->ClearView(view.Get(), kBar, &bar, 1);
        barLeft = (barLeft + kBarStep) % (width - kBarWidth);
        ThrowIfFailed(swapChain->Present(1, 0), "Present");
    }
    DestroyWindow(window);
    UnregisterClassW(kWindowClass, windowClass.hInstance);
}

}  // namespace aum
