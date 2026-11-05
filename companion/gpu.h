#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace aum {

struct AdapterInfo {
    LUID luid;
    std::wstring description;
    UINT vendorId;
};

std::vector<AdapterInfo> ListHardwareAdapters();
std::optional<LUID> FindAdapter(const std::wstring& nameSubstring);
std::wstring DescribeAdapter(LUID luid);
bool IsNvidiaAdapter(LUID luid);

struct GpuDevice {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    LUID luid;
};

GpuDevice CreateGpuDevice(LUID luid);

// The GPU to render and encode the monitor on: NVIDIA when there is one, because its encoder keeps full rate under any
// 3D load, while AMD's drops to around half when its 3D engine is busy.
LUID PreferredEncodingAdapter();

}  // namespace aum
