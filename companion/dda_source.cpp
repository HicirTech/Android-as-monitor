#include "dda_source.h"

#include "clock.h"
#include "win_error.h"

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace aum {

static constexpr UINT kAcquireTimeoutMs = 50;

static ComPtr<IDXGIOutput1> FindOutput(IDXGIAdapter1* adapter, const std::wstring& gdiDeviceName, DXGI_OUTPUT_DESC& desc) {
    ComPtr<IDXGIOutput> output;
    for (UINT index = 0; adapter->EnumOutputs(index, &output) != DXGI_ERROR_NOT_FOUND; ++index) {
        output->GetDesc(&desc);
        if (gdiDeviceName == desc.DeviceName) {
            ComPtr<IDXGIOutput1> output1;
            ThrowIfFailed(output.As(&output1), "IDXGIOutput1 query");
            return output1;
        }
        output.Reset();
    }
    return nullptr;
}

std::optional<OutputLocation> DdaSource::Locate(const std::wstring& gdiDeviceName) {
    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_OUTPUT_DESC desc{};
        if (FindOutput(adapter.Get(), gdiDeviceName, desc)) {
            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter->GetDesc1(&adapterDesc);
            return OutputLocation{adapterDesc.AdapterLuid,
                                  static_cast<std::uint32_t>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left),
                                  static_cast<std::uint32_t>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top)};
        }
        adapter.Reset();
    }
    return std::nullopt;
}

DdaSource::DdaSource(GpuDevice& gpu, const std::wstring& gdiDeviceName) : gpu_(gpu) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ThrowIfFailed(gpu_.device.As(&dxgiDevice), "IDXGIDevice query");
    ComPtr<IDXGIAdapter> adapter;
    ThrowIfFailed(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIAdapter1> adapter1;
    ThrowIfFailed(adapter.As(&adapter1), "IDXGIAdapter1 query");

    DXGI_OUTPUT_DESC desc{};
    const ComPtr<IDXGIOutput1> output = FindOutput(adapter1.Get(), gdiDeviceName, desc);
    if (!output) {
        throw std::runtime_error("display output is not on the capture GPU: " + Narrow(gdiDeviceName));
    }
    ThrowIfFailed(output->DuplicateOutput(gpu_.device.Get(), &duplication_), "DuplicateOutput");
}

bool DdaSource::Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) {
    for (;;) {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            return false;
        }
        if (holdingFrame_) {
            duplication_->ReleaseFrame();
            holdingFrame_ = false;
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = duplication_->AcquireNextFrame(kAcquireTimeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        ThrowIfFailed(hr, "AcquireNextFrame");
        holdingFrame_ = true;
        if (info.LastPresentTime.QuadPart == 0) {
            continue;
        }
        ComPtr<ID3D11Texture2D> texture;
        ThrowIfFailed(resource.As(&texture), "texture query");
        gpu_.context->CopyResource(target, texture.Get());
        frame.capturedNs = QpcToNs(info.LastPresentTime.QuadPart);
        frame.isRepeat = false;
        return true;
    }
}

}  // namespace aum
