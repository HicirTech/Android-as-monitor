#include <initguid.h>

#include "mf_encoder.h"

#include "win_error.h"

#include <mferror.h>

#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace aum {

namespace {

// Sample times only carry the frame id through the encoder; nothing in the stream depends on their spacing.
constexpr LONGLONG kSampleTimeUnitsPerFrameId = 10'000;

const GUID& SubtypeOf(VideoCodec codec) {
    return codec == VideoCodec::Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

void SetUint32(ICodecAPI* api, const GUID& property, ULONG value, const char* label) {
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    const HRESULT hr = api->SetValue(&property, &variant);
    if (FAILED(hr)) {
        std::printf("encoder: %s not applied (0x%08lX)\n", label, static_cast<unsigned long>(hr));
    }
}

void SetBool(ICodecAPI* api, const GUID& property, bool value, const char* label) {
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = api->SetValue(&property, &variant);
    if (FAILED(hr)) {
        std::printf("encoder: %s not applied (0x%08lX)\n", label, static_cast<unsigned long>(hr));
    }
}

}  // namespace


MfEncoder::MfEncoder(GpuDevice& gpu, const EncoderSettings& settings, EncodedFrameHandler onFrame)
    : gpu_(gpu), settings_(settings), onFrame_(std::move(onFrame)) {
    CreateTransform(gpu_.luid);
    ConfigureCodecApi();
    ConfigureMediaTypes();

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    ThrowIfFailed(transform_->GetOutputStreamInfo(outputStream_, &streamInfo), "GetOutputStreamInfo");
    outputProvidesSamples_ = (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputBufferBytes_ = streamInfo.cbSize;

    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "MFT flush");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "MFT begin streaming");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "MFT start of stream");
    eventThread_ = std::thread([this] { EventLoop(); });
}

MfEncoder::~MfEncoder() {
    stopping_ = true;
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    // Shutting the generator down is what releases the event thread from its blocking GetEvent.
    ComPtr<IMFShutdown> shutdown;
    if (events_ && SUCCEEDED(events_.As(&shutdown))) {
        shutdown->Shutdown();
    } else if (activate_) {
        activate_->ShutdownObject();
    }
    if (eventThread_.joinable()) {
        eventThread_.join();
    }
    if (activate_) {
        activate_->ShutdownObject();
    }
}

void MfEncoder::CreateTransform(LUID adapter) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, SubtypeOf(settings_.codec)};

    ComPtr<IMFAttributes> filter;
    ThrowIfFailed(MFCreateAttributes(&filter, 1), "MFCreateAttributes");
    ThrowIfFailed(filter->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapter), sizeof(adapter)), "set adapter filter");

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    ThrowIfFailed(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output, filter.Get(), &activates, &count),
                  "MFTEnum2");
    if (count == 0) {
        CoTaskMemFree(activates);
        throw std::runtime_error("no hardware encoder for this codec on GPU " + Narrow(DescribeAdapter(adapter)));
    }
    activate_ = activates[0];
    for (UINT32 index = 0; index < count; ++index) {
        activates[index]->Release();
    }
    CoTaskMemFree(activates);

    wchar_t* friendlyName = nullptr;
    UINT32 nameLength = 0;
    if (SUCCEEDED(activate_->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendlyName, &nameLength))) {
        name_ = friendlyName;
        CoTaskMemFree(friendlyName);
    }

    ThrowIfFailed(activate_->ActivateObject(IID_PPV_ARGS(&transform_)), "activate encoder");

    ComPtr<IMFAttributes> attributes;
    ThrowIfFailed(transform_->GetAttributes(&attributes), "encoder attributes");
    ThrowIfFailed(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "unlock async encoder");
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

    ThrowIfFailed(transform_.As(&events_), "encoder event generator");
    ThrowIfFailed(transform_.As(&codecApi_), "ICodecAPI query");

    UINT token = 0;
    ThrowIfFailed(MFCreateDXGIDeviceManager(&token, &deviceManager_), "MFCreateDXGIDeviceManager");
    ThrowIfFailed(deviceManager_->ResetDevice(gpu_.device.Get(), token), "reset device manager");
    ThrowIfFailed(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager_.Get())), "set D3D manager");

    DWORD inputCount = 0;
    DWORD outputCount = 0;
    ThrowIfFailed(transform_->GetStreamCount(&inputCount, &outputCount), "GetStreamCount");
    const HRESULT ids = transform_->GetStreamIDs(1, &inputStream_, 1, &outputStream_);
    if (ids == E_NOTIMPL) {
        inputStream_ = 0;
        outputStream_ = 0;
    } else {
        ThrowIfFailed(ids, "GetStreamIDs");
    }
}

void MfEncoder::ConfigureCodecApi() {
    ICodecAPI* api = codecApi_.Get();
    SetBool(api, CODECAPI_AVLowLatencyMode, true, "low latency mode");
    SetUint32(api, CODECAPI_AVScenarioInfo, eAVScenarioInfo_DisplayRemoting, "display remoting scenario");
    SetUint32(api, CODECAPI_AVEncMPVDefaultBPictureCount, 0, "no B pictures");
    // Keyframes only on request: a periodic one would spike the bitrate on a reliable link for no benefit.
    SetUint32(api, CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(settings_.refresh.value() * 600), "GOP size");
    // CBR would pad an idle desktop up to the full bitrate; peak VBR spends bits only when the picture changes.
    SetUint32(api, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR, "peak VBR rate control");
    SetUint32(api, CODECAPI_AVEncCommonMaxBitRate, settings_.bitrateBps * 2, "peak bitrate");
    SetUint32(api, CODECAPI_AVEncCommonMeanBitRate, settings_.bitrateBps, "mean bitrate");
    SetUint32(api, CODECAPI_AVEncCommonQualityVsSpeed, 0, "fastest preset");
}

void MfEncoder::ConfigureMediaTypes() {
    ComPtr<IMFMediaType> outputType;
    ThrowIfFailed(MFCreateMediaType(&outputType), "MFCreateMediaType");
    ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "output major type");
    ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, SubtypeOf(settings_.codec)), "output subtype");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, settings_.bitrateBps), "output bitrate");
    ThrowIfFailed(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, settings_.width, settings_.height), "output size");
    ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(settings_.refresh.numerator), static_cast<UINT32>(settings_.refresh.denominator)), "output rate");
    ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "output interlace");
    ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "output aspect");
    outputType->SetUINT32(MF_MT_MPEG2_PROFILE, settings_.codec == VideoCodec::Hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
    outputType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outputType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    outputType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outputType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    ThrowIfFailed(transform_->SetOutputType(outputStream_, outputType.Get(), 0), "SetOutputType");

    ComPtr<IMFMediaType> inputType;
    ThrowIfFailed(MFCreateMediaType(&inputType), "MFCreateMediaType");
    ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "input major type");
    ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "input subtype");
    ThrowIfFailed(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, settings_.width, settings_.height), "input size");
    ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(settings_.refresh.numerator), static_cast<UINT32>(settings_.refresh.denominator)), "input rate");
    ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "input interlace");
    ThrowIfFailed(transform_->SetInputType(inputStream_, inputType.Get(), 0), "SetInputType");
}

bool MfEncoder::Submit(ID3D11Texture2D* nv12, std::uint64_t frameId, bool forceKeyframe) {
    if (forceKeyframe) {
        SetUint32(codecApi_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, "force keyframe");
    }
    ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer), "MFCreateDXGISurfaceBuffer");
    ComPtr<IMF2DBuffer> buffer2d;
    ThrowIfFailed(buffer.As(&buffer2d), "IMF2DBuffer query");
    DWORD length = 0;
    ThrowIfFailed(buffer2d->GetContiguousLength(&length), "GetContiguousLength");
    ThrowIfFailed(buffer->SetCurrentLength(length), "SetCurrentLength");

    ComPtr<IMFSample> sample;
    ThrowIfFailed(MFCreateSample(&sample), "MFCreateSample");
    ThrowIfFailed(sample->AddBuffer(buffer.Get()), "AddBuffer");
    ThrowIfFailed(sample->SetSampleTime(static_cast<LONGLONG>(frameId) * kSampleTimeUnitsPerFrameId), "SetSampleTime");
    ThrowIfFailed(sample->SetSampleDuration(kSampleTimeUnitsPerFrameId), "SetSampleDuration");

    pendingInputRequests_.fetch_sub(1);
    ThrowIfFailed(transform_->ProcessInput(inputStream_, sample.Get(), 0), "ProcessInput");
    return false;
}

void MfEncoder::EventLoop() {
    while (!stopping_) {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(events_->GetEvent(0, &event))) {
            return;
        }
        MediaEventType type = MEUnknown;
        if (FAILED(event->GetType(&type))) {
            return;
        }
        if (type == METransformNeedInput) {
            pendingInputRequests_.fetch_add(1);
        } else if (type == METransformHaveOutput) {
            DrainOutput();
        }
    }
}

void MfEncoder::DrainOutput() {
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = outputStream_;
    ComPtr<IMFSample> ownSample;
    if (!outputProvidesSamples_) {
        ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateMemoryBuffer(outputBufferBytes_, &buffer), "MFCreateMemoryBuffer");
        ThrowIfFailed(MFCreateSample(&ownSample), "MFCreateSample");
        ThrowIfFailed(ownSample->AddBuffer(buffer.Get()), "AddBuffer");
        output.pSample = ownSample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) {
        output.pEvents->Release();
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        ComPtr<IMFMediaType> type;
        ThrowIfFailed(transform_->GetOutputAvailableType(outputStream_, 0, &type), "GetOutputAvailableType");
        ThrowIfFailed(transform_->SetOutputType(outputStream_, type.Get(), 0), "SetOutputType after stream change");
        return;
    }
    if (FAILED(hr)) {
        if (!stopping_) {
            std::printf("encoder: ProcessOutput failed 0x%08lX\n", static_cast<unsigned long>(hr));
        }
        return;
    }

    ComPtr<IMFSample> produced;
    if (ownSample) {
        produced = ownSample;
    } else {
        produced.Attach(output.pSample);
    }
    if (produced) {
        ForwardSample(produced.Get());
    }
}

void MfEncoder::ForwardSample(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    ThrowIfFailed(sample->ConvertToContiguousBuffer(&buffer), "ConvertToContiguousBuffer");

    LONGLONG sampleTime = 0;
    sample->GetSampleTime(&sampleTime);
    UINT32 cleanPoint = 0;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint);

    BYTE* data = nullptr;
    DWORD size = 0;
    ThrowIfFailed(buffer->Lock(&data, nullptr, &size), "Lock output");

    EncodedFrame result{static_cast<std::uint64_t>(sampleTime / kSampleTimeUnitsPerFrameId), cleanPoint != 0, data, size};

    onFrame_(result);
    buffer->Unlock();
}

}  // namespace aum
