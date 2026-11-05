#pragma once

#include "video_encoder.h"

#include <mfidl.h>
#include <mftransform.h>
#include <mfapi.h>
#include <strmif.h>
#include <codecapi.h>

#include <atomic>
#include <thread>

namespace aum {

// A hardware encoder through Media Foundation: no vendor SDK, works wherever the GPU driver registers an encoder.
class MfEncoder final : public VideoEncoder {
public:
    MfEncoder(GpuDevice& gpu, const EncoderSettings& settings, EncodedFrameHandler onFrame);
    ~MfEncoder() override;

    MfEncoder(const MfEncoder&) = delete;
    MfEncoder& operator=(const MfEncoder&) = delete;

    const std::wstring& Name() const override { return name_; }
    bool CanAccept() const override { return pendingInputRequests_.load() > 0; }
    bool Submit(ID3D11Texture2D* nv12, std::uint64_t frameId, bool forceKeyframe) override;

private:
    void CreateTransform(LUID adapter);
    void ConfigureCodecApi();
    void ConfigureMediaTypes();
    void EventLoop();
    void DrainOutput();
    void ForwardSample(IMFSample* sample);

    GpuDevice& gpu_;
    EncoderSettings settings_;
    EncodedFrameHandler onFrame_;
    std::wstring name_;
    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFActivate> activate_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    DWORD inputStream_ = 0;
    DWORD outputStream_ = 0;
    bool outputProvidesSamples_ = false;
    std::uint32_t outputBufferBytes_ = 0;
    std::atomic<int> pendingInputRequests_{0};
    std::atomic<bool> stopping_{false};
    std::thread eventThread_;
};

}  // namespace aum
