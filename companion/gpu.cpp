#include "gpu.h"

#include "win_error.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace aum {

static constexpr UINT kNvidiaVendorId = 0x10DE;

static bool SameLuid(LUID a, LUID b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static bool ContainsIgnoringCase(std::wstring text, std::wstring needle) {
    const auto lower = [](std::wstring& value) {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    };
    lower(text);
    lower(needle);
    return text.find(needle) != std::wstring::npos;
}

std::vector<AdapterInfo> ListHardwareAdapters() {
    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    std::vector<AdapterInfo> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            adapters.push_back({desc.AdapterLuid, desc.Description, desc.VendorId});
        }
        adapter.Reset();
    }
    return adapters;
}

std::optional<LUID> FindAdapter(const std::wstring& nameSubstring) {
    for (const AdapterInfo& adapter : ListHardwareAdapters()) {
        if (ContainsIgnoringCase(adapter.description, nameSubstring)) {
            return adapter.luid;
        }
    }
    return std::nullopt;
}

std::wstring DescribeAdapter(LUID luid) {
    for (const AdapterInfo& adapter : ListHardwareAdapters()) {
        if (SameLuid(adapter.luid, luid)) {
            return adapter.description;
        }
    }
    return L"(unknown adapter)";
}

bool IsNvidiaAdapter(LUID luid) {
    for (const AdapterInfo& adapter : ListHardwareAdapters()) {
        if (SameLuid(adapter.luid, luid)) {
            return adapter.vendorId == kNvidiaVendorId;
        }
    }
    return false;
}

LUID PreferredEncodingAdapter() {
    const auto adapters = ListHardwareAdapters();
    if (adapters.empty()) {
        throw std::runtime_error("no hardware GPU found");
    }
    for (const AdapterInfo& adapter : adapters) {
        if (adapter.vendorId == kNvidiaVendorId) {
            return adapter.luid;
        }
    }
    return adapters.front().luid;
}

GpuDevice CreateGpuDevice(LUID luid) {
    ComPtr<IDXGIFactory6> factory;
    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    ThrowIfFailed(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid");

    GpuDevice gpu;
    gpu.luid = luid;
    static constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    ThrowIfFailed(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                    D3D11_SDK_VERSION, &gpu.device, nullptr, &gpu.context),
                  "D3D11CreateDevice");

    // The hardware encoder uses this device from its own threads.
    ComPtr<ID3D10Multithread> multithread;
    ThrowIfFailed(gpu.device.As(&multithread), "ID3D10Multithread query");
    multithread->SetMultithreadProtected(TRUE);
    return gpu;
}

}  // namespace aum
