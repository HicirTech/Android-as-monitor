#include "nvenc_encoder.h"

#include "win_error.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace aum {

namespace {

// How many frames at the peak rate one frame may use. A single frame is what ultra low latency tuning defaults to, which
// starves a full-screen change; the USB link is much faster than the peak rate, so the burst costs a millisecond or two.
constexpr std::uint32_t kVbvFrames = 3;
constexpr std::uint32_t kPeakToAverageRatio = 2;

template <typename Parameters>
void SetLowDelayStreamOptions(Parameters& codec) {
    codec.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    // Parameter sets travel with every keyframe, so a decoder can join on any keyframe it asks for.
    codec.repeatSPSPPS = 1;
}

void SetColorSignalling(NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui) {
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    vui.videoFullRangeFlag = 0;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    // Declares that no frame waits for reordering, which lets decoders output each frame as soon as it is decoded.
    vui.bitstreamRestrictionFlag = 1;
}

}  // namespace

NvencEncoder::NvencEncoder(GpuDevice& gpu, const EncoderSettings& settings, EncodedFrameHandler onFrame)
    : settings_(settings), onFrame_(std::move(onFrame)) {
    name_ = settings_.codec == VideoCodec::Hevc ? L"NVENC HEVC" : L"NVENC H.264";
    LoadRuntime();
    OpenSession(gpu.device.Get());
    InitializeEncoder();

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
    bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    Check(api_.nvEncCreateBitstreamBuffer(session_, &bitstream), "nvEncCreateBitstreamBuffer");
    bitstream_ = bitstream.bitstreamBuffer;

    frameReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_ = std::thread([this] { Run(); });
}

NvencEncoder::~NvencEncoder() {
    if (worker_.joinable()) {
        SetEvent(stop_);
        worker_.join();
    }
    if (session_) {
        NV_ENC_PIC_PARAMS endOfStream{};
        endOfStream.version = NV_ENC_PIC_PARAMS_VER;
        endOfStream.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        api_.nvEncEncodePicture(session_, &endOfStream);
        for (const auto& [texture, resource] : registered_) {
            api_.nvEncUnregisterResource(session_, resource);
        }
        if (bitstream_) {
            api_.nvEncDestroyBitstreamBuffer(session_, bitstream_);
        }
        api_.nvEncDestroyEncoder(session_);
    }
    if (runtime_) {
        FreeLibrary(runtime_);
    }
    if (frameReady_) {
        CloseHandle(frameReady_);
    }
    if (stop_) {
        CloseHandle(stop_);
    }
}

void NvencEncoder::Check(NVENCSTATUS status, const char* what) const {
    if (status == NV_ENC_SUCCESS) {
        return;
    }
    const char* detail = session_ && api_.nvEncGetLastErrorString ? api_.nvEncGetLastErrorString(session_) : "";
    throw std::runtime_error(std::string(what) + " failed with NVENC status " + std::to_string(status) + ": " + (detail ? detail : ""));
}

void NvencEncoder::LoadRuntime() {
    runtime_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime_) {
        throw std::runtime_error("nvEncodeAPI64.dll is not available: no NVIDIA driver with NVENC");
    }
    using GetMaxVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    const auto getMaxVersion = reinterpret_cast<GetMaxVersion>(GetProcAddress(runtime_, "NvEncodeAPIGetMaxSupportedVersion"));
    const auto createInstance = reinterpret_cast<CreateInstance>(GetProcAddress(runtime_, "NvEncodeAPICreateInstance"));
    if (!getMaxVersion || !createInstance) {
        throw std::runtime_error("nvEncodeAPI64.dll does not export the NVENC entry points");
    }

    std::uint32_t driverVersion = 0;
    Check(getMaxVersion(&driverVersion), "NvEncodeAPIGetMaxSupportedVersion");
    const std::uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driverVersion < headerVersion) {
        throw std::runtime_error("the NVIDIA driver supports NVENC API " + std::to_string(driverVersion >> 4) + "." + std::to_string(driverVersion & 0xF) + " but this build needs " +
                                 std::to_string(NVENCAPI_MAJOR_VERSION) + "." + std::to_string(NVENCAPI_MINOR_VERSION) + ": update the driver");
    }

    api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    Check(createInstance(&api_), "NvEncodeAPICreateInstance");
}

void NvencEncoder::OpenSession(ID3D11Device* device) {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = device;
    open.apiVersion = NVENCAPI_VERSION;
    Check(api_.nvEncOpenEncodeSessionEx(&open, &session_), "nvEncOpenEncodeSessionEx");
}

void NvencEncoder::InitializeEncoder() {
    const GUID codec = settings_.codec == VideoCodec::Hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    const std::uint32_t frameRateNum = static_cast<std::uint32_t>(settings_.refresh.numerator);
    const std::uint32_t frameRateDen = static_cast<std::uint32_t>(settings_.refresh.denominator);

    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    Check(api_.nvEncGetEncodePresetConfigEx(session_, codec, NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset), "nvEncGetEncodePresetConfigEx");

    NV_ENC_CONFIG config = preset.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.gopLength = NVENC_INFINITE_GOPLENGTH;
    config.frameIntervalP = 1;
    // Peak constrained VBR spends bits only when the picture changes; CBR would pad an idle desktop up to the full rate.
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.averageBitRate = settings_.bitrateBps;
    config.rcParams.maxBitRate = settings_.bitrateBps * kPeakToAverageRatio;
    const std::uint64_t vbvBits = std::uint64_t{config.rcParams.maxBitRate} * kVbvFrames * frameRateDen / frameRateNum;
    config.rcParams.vbvBufferSize = static_cast<std::uint32_t>(vbvBits);
    config.rcParams.vbvInitialDelay = static_cast<std::uint32_t>(vbvBits);
    if (settings_.codec == VideoCodec::Hevc) {
        SetLowDelayStreamOptions(config.encodeCodecConfig.hevcConfig);
        SetColorSignalling(config.encodeCodecConfig.hevcConfig.hevcVUIParameters);
    } else {
        SetLowDelayStreamOptions(config.encodeCodecConfig.h264Config);
        SetColorSignalling(config.encodeCodecConfig.h264Config.h264VUIParameters);
    }

    NV_ENC_INITIALIZE_PARAMS initialize{};
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = codec;
    initialize.presetGUID = NV_ENC_PRESET_P1_GUID;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initialize.encodeWidth = settings_.width;
    initialize.encodeHeight = settings_.height;
    initialize.darWidth = settings_.width;
    initialize.darHeight = settings_.height;
    initialize.maxEncodeWidth = settings_.width;
    initialize.maxEncodeHeight = settings_.height;
    initialize.frameRateNum = frameRateNum;
    initialize.frameRateDen = frameRateDen;
    initialize.enablePTD = 1;
    initialize.encodeConfig = &config;
    Check(api_.nvEncInitializeEncoder(session_, &initialize), "nvEncInitializeEncoder");
}

NV_ENC_REGISTERED_PTR NvencEncoder::RegisteredResourceFor(ID3D11Texture2D* texture) {
    const auto found = registered_.find(texture);
    if (found != registered_.end()) {
        return found->second;
    }
    NV_ENC_REGISTER_RESOURCE resource{};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.width = settings_.width;
    resource.height = settings_.height;
    resource.resourceToRegister = texture;
    resource.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;
    Check(api_.nvEncRegisterResource(session_, &resource), "nvEncRegisterResource");
    registered_[texture] = resource.registeredResource;
    return resource.registeredResource;
}

bool NvencEncoder::Submit(ID3D11Texture2D* nv12, std::uint64_t frameId, bool forceKeyframe) {
    const bool replaced = pending_.Put({nv12, frameId, forceKeyframe});
    SetEvent(frameReady_);
    return replaced;
}

void NvencEncoder::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const HANDLE handles[] = {frameReady_, stop_};
    try {
        while (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
            while (const auto frame = pending_.Take()) {
                EncodeOne(*frame);
            }
        }
    } catch (const std::exception& error) {
        // Nothing can recover a lost encoder mid-stream. Exiting also unplugs the monitor, through the driver pipe.
        std::fprintf(stderr, "error: %s\n", error.what());
        ExitProcess(1);
    }
}

void NvencEncoder::EncodeOne(const PendingFrame& frame) {
    NV_ENC_MAP_INPUT_RESOURCE input{};
    input.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    input.registeredResource = RegisteredResourceFor(frame.texture);
    Check(api_.nvEncMapInputResource(session_, &input), "nvEncMapInputResource");

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = settings_.width;
    picture.inputHeight = settings_.height;
    picture.inputPitch = settings_.width;
    picture.encodePicFlags = frame.forceKeyframe ? (NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) : 0;
    picture.inputTimeStamp = frame.frameId;
    picture.inputBuffer = input.mappedResource;
    picture.outputBitstream = bitstream_;
    picture.bufferFmt = input.mappedBufferFmt;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    Check(api_.nvEncEncodePicture(session_, &picture), "nvEncEncodePicture");

    // Without B frames every picture comes straight back; this waits until the hardware has finished it.
    NV_ENC_LOCK_BITSTREAM output{};
    output.version = NV_ENC_LOCK_BITSTREAM_VER;
    output.outputBitstream = bitstream_;
    Check(api_.nvEncLockBitstream(session_, &output), "nvEncLockBitstream");

    onFrame_({output.outputTimeStamp, output.pictureType == NV_ENC_PIC_TYPE_IDR, static_cast<const std::uint8_t*>(output.bitstreamBufferPtr), output.bitstreamSizeInBytes});

    Check(api_.nvEncUnlockBitstream(session_, bitstream_), "nvEncUnlockBitstream");
    Check(api_.nvEncUnmapInputResource(session_, input.mappedResource), "nvEncUnmapInputResource");
}

}  // namespace aum
