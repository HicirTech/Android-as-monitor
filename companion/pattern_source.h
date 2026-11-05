#pragma once

#include "frame_source.h"

#include "../shared/edid_timing.h"

#include <vector>

namespace aum {

inline constexpr std::uint32_t kPatternBandRows = 64;

enum class PatternKind {
    // One-pixel line bands prove the phone shows pixels 1:1; a moving bar forces every frame to differ.
    PixelBands,
    // Fine detail scrolling across the whole frame: the worst case a desktop can ask of the encoder.
    ScrollingDetail,
};

// A synthetic frame at the panel's exact size and rate, so the pipeline can be measured without the driver.
class PatternSource final : public FrameSource {
public:
    PatternSource(GpuDevice& gpu, std::uint32_t width, std::uint32_t height, Rational refresh, PatternKind kind);
    ~PatternSource() override;

    bool Next(ID3D11Texture2D* target, CapturedFrame& frame, HANDLE stopEvent) override;

private:
    void WaitForNextTick(HANDLE stopEvent);
    void CreatePixelBandsTexture();
    void CreateScrollingDetailTexture();
    void DrawPixelBands(ID3D11Texture2D* target);
    void DrawScrollingDetail(ID3D11Texture2D* target);

    GpuDevice& gpu_;
    std::uint32_t width_;
    std::uint32_t height_;
    PatternKind kind_;
    std::int64_t frameIntervalNs_;
    std::int64_t nextTickNs_ = 0;
    std::uint64_t frameIndex_ = 0;
    HANDLE timer_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staticFrame_;
};

}  // namespace aum
