#include "video_pipeline.h"

#include "clock.h"
#include "win_error.h"

namespace aum {

VideoPipeline::VideoPipeline(GpuDevice& gpu, FrameSource& source, const EncoderSettings& settings, LatencyStats& stats, PacketSink sink)
    : gpu_(gpu), source_(source), stats_(stats), sink_(std::move(sink)), converter_(gpu, settings.width, settings.height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = settings.width;
    desc.Height = settings.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ThrowIfFailed(gpu_.device->CreateTexture2D(&desc, nullptr, &captureTexture_), "capture CreateTexture2D");

    desc.Format = DXGI_FORMAT_NV12;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;
    for (auto& texture : inputTextures_) {
        ThrowIfFailed(gpu_.device->CreateTexture2D(&desc, nullptr, &texture), "encoder input CreateTexture2D");
    }

    encoder_ = CreateVideoEncoder(gpu_, settings, [this](const EncodedFrame& output) {
        stats_.OnEncoded(output.frameId, NowNs(), output.size);
        if (sink_(output)) {
            stats_.OnSent(output.frameId, NowNs());
        }
    });
}

void VideoPipeline::Run(HANDLE stopEvent) {
    // A frame waits on this thread for the CPU before it can be encoded, so it must not lose out to whatever else the PC is doing.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        CapturedFrame frame;
        if (!source_.Next(captureTexture_.Get(), frame, stopEvent)) {
            return;
        }
        const std::int64_t pickedUpNs = NowNs();
        stats_.OnCaptured();

        if (!encoder_->CanAccept()) {
            stats_.OnDroppedBeforeEncode();
            continue;
        }

        const std::uint64_t frameId = nextFrameId_++;
        ID3D11Texture2D* input = inputTextures_[frameId % kInputTextureCount].Get();
        converter_.Convert(captureTexture_.Get(), input);
        // Queued GPU work otherwise waits for a later flush, which would count against the encoder and add latency.
        gpu_.context->Flush();
        const bool forceKeyframe = keyframeRequested_.exchange(false);
        stats_.OnSubmitted(frameId, frame.capturedNs, pickedUpNs, NowNs());
        if (encoder_->Submit(input, frameId, forceKeyframe)) {
            stats_.OnDroppedBeforeEncode();
        }
    }
}

}  // namespace aum
