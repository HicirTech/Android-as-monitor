#pragma once

#include "pending_frame_slot.h"
#include "video_encoder.h"

#include <nvEncodeAPI.h>

#include <map>
#include <thread>

namespace aum {

// NVENC directly. It keeps full rate while the same GPU's 3D engine is saturated, and it needs no Media Foundation
// transform, which fails to activate on NVIDIA here.
class NvencEncoder final : public VideoEncoder {
public:
    NvencEncoder(GpuDevice& gpu, const EncoderSettings& settings, EncodedFrameHandler onFrame);
    ~NvencEncoder() override;

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    const std::wstring& Name() const override { return name_; }
    bool CanAccept() const override { return true; }
    bool Submit(ID3D11Texture2D* nv12, std::uint64_t frameId, bool forceKeyframe) override;

private:
    void Check(NVENCSTATUS status, const char* what) const;
    void LoadRuntime();
    void OpenSession(ID3D11Device* device);
    void InitializeEncoder();
    NV_ENC_REGISTERED_PTR RegisteredResourceFor(ID3D11Texture2D* texture);
    void Run();
    void EncodeOne(const PendingFrame& frame);

    EncoderSettings settings_;
    EncodedFrameHandler onFrame_;
    std::wstring name_;
    HMODULE runtime_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* session_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    std::map<ID3D11Texture2D*, NV_ENC_REGISTERED_PTR> registered_;

    // One frame encoding and one waiting. The worker owns every NVENC call after start-up.
    PendingFrameSlot pending_;
    HANDLE frameReady_ = nullptr;
    HANDLE stop_ = nullptr;
    std::thread worker_;
};

}  // namespace aum
