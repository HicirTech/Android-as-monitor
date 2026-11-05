#pragma once

#include "color_converter.h"
#include "frame_source.h"
#include "latency_stats.h"
#include "video_encoder.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace aum {

// Capture to encoded bitstream. The newest frame always wins: a frame is dropped before encoding when the encoder or
// the link is still busy, so latency stays bounded instead of growing a queue.
class VideoPipeline {
public:
    using PacketSink = std::function<bool(const EncodedFrame&)>;

    VideoPipeline(GpuDevice& gpu, FrameSource& source, const EncoderSettings& settings, LatencyStats& stats, PacketSink sink);

    const std::wstring& EncoderName() const { return encoder_->Name(); }
    void RequestKeyframe() { keyframeRequested_ = true; }

    // Runs until `stopEvent` is signalled.
    void Run(HANDLE stopEvent);

private:
    static constexpr std::size_t kInputTextureCount = 6;

    GpuDevice& gpu_;
    FrameSource& source_;
    LatencyStats& stats_;
    PacketSink sink_;
    ColorConverter converter_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> captureTexture_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kInputTextureCount> inputTextures_;
    std::atomic<bool> keyframeRequested_{true};
    std::uint64_t nextFrameId_ = 0;
    // Last, so it is destroyed first: the encoder holds the input textures while it runs and unregisters them when it stops.
    std::unique_ptr<VideoEncoder> encoder_;
};

}  // namespace aum
