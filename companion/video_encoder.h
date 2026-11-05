#pragma once

#include "gpu.h"

#include "../shared/edid_timing.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aum {

enum class VideoCodec { Hevc, H264 };

inline const char* MimeType(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? "video/hevc" : "video/avc";
}

struct EncoderSettings {
    VideoCodec codec = VideoCodec::Hevc;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Rational refresh{};
    std::uint32_t bitrateBps = 0;
};

struct EncodedFrame {
    std::uint64_t frameId;
    bool isKeyframe;
    const std::uint8_t* data;
    std::uint32_t size;
};

using EncodedFrameHandler = std::function<void(const EncodedFrame&)>;

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual const std::wstring& Name() const = 0;

    // False when the encoder is still busy: the caller drops the frame so latency never builds up.
    virtual bool CanAccept() const = 0;
    // True when this frame took the place of one that was still waiting to be encoded.
    virtual bool Submit(ID3D11Texture2D* nv12, std::uint64_t frameId, bool forceKeyframe) = 0;
};

std::unique_ptr<VideoEncoder> CreateVideoEncoder(GpuDevice& gpu, const EncoderSettings& settings, EncodedFrameHandler onFrame);

}  // namespace aum
