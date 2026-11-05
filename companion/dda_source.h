#pragma once

#include "frame_source.h"

#include <optional>
#include <string>

namespace aum {

struct OutputLocation {
    LUID adapter;
    std::uint32_t width;
    std::uint32_t height;
};

// Desktop Duplication, used to compare against the driver tap and to measure the pipeline on a physical monitor
// before the driver is installed. The pointer is not composited into these frames.
class DdaSource final : public FrameSource {
public:
    static std::optional<OutputLocation> Locate(const std::wstring& gdiDeviceName);

    DdaSource(GpuDevice& gpu, const std::wstring& gdiDeviceName);

    bool Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) override;

private:
    GpuDevice& gpu_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    bool holdingFrame_ = false;
};

}  // namespace aum
