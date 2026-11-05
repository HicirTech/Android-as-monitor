#include "video_encoder.h"

#include "mf_encoder.h"
#include "nvenc_encoder.h"

namespace aum {

std::unique_ptr<VideoEncoder> CreateVideoEncoder(GpuDevice& gpu, const EncoderSettings& settings, EncodedFrameHandler onFrame) {
    if (IsNvidiaAdapter(gpu.luid)) {
        return std::make_unique<NvencEncoder>(gpu, settings, std::move(onFrame));
    }
    return std::make_unique<MfEncoder>(gpu, settings, std::move(onFrame));
}

}  // namespace aum
