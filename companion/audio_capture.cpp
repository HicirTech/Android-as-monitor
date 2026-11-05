#include <initguid.h>

#include "audio_capture.h"

#include "win_error.h"

#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace aum {

static std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    ThrowIfFailed(device->OpenPropertyStore(STGM_READ, &properties), "OpenPropertyStore");
    PROPVARIANT value;
    PropVariantInit(&value);
    ThrowIfFailed(properties->GetValue(PKEY_Device_FriendlyName, &value), "read endpoint name");
    std::wstring name = value.vt == VT_LPWSTR ? value.pwszVal : L"";
    PropVariantClear(&value);
    return name;
}

static ComPtr<IMMDeviceEnumerator> CreateEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ThrowIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
    return enumerator;
}

std::vector<std::wstring> ListRenderEndpoints() {
    const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
    ComPtr<IMMDeviceCollection> collection;
    ThrowIfFailed(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "EnumAudioEndpoints");
    UINT count = 0;
    collection->GetCount(&count);
    std::vector<std::wstring> names;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(index, &device))) {
            names.push_back(FriendlyName(device.Get()));
        }
    }
    return names;
}

static ComPtr<IMMDevice> FindEndpoint(const std::wstring& name) {
    const ComPtr<IMMDeviceEnumerator> enumerator = CreateEnumerator();
    if (name.empty()) {
        ComPtr<IMMDevice> device;
        ThrowIfFailed(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "GetDefaultAudioEndpoint");
        return device;
    }
    ComPtr<IMMDeviceCollection> collection;
    ThrowIfFailed(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "EnumAudioEndpoints");
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(index, &device)) && FriendlyName(device.Get()).find(name) != std::wstring::npos) {
            return device;
        }
    }
    throw std::runtime_error("no playback endpoint contains the name " + Narrow(name));
}

AudioCapture::AudioCapture(const std::wstring& endpointName, Sink sink) : sink_(std::move(sink)) {
    ComPtr<IMMDevice> device = FindEndpoint(endpointName);
    endpointName_ = FriendlyName(device.Get());

    ComPtr<IAudioClient> client;
    ThrowIfFailed(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())), "activate IAudioClient");

    WAVEFORMATEX* mix = nullptr;
    ThrowIfFailed(client->GetMixFormat(&mix), "GetMixFormat");
    const bool isFloat32 = mix->wBitsPerSample == 32 && (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (!isFloat32) {
        CoTaskMemFree(mix);
        throw std::runtime_error("endpoint mix format is not float32");
    }
    sampleRate_ = mix->nSamplesPerSec;
    channels_ = mix->nChannels;

    static constexpr REFERENCE_TIME kBufferDuration100ns = 200'000;
    const HRESULT initialized = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                   kBufferDuration100ns, 0, mix, nullptr);
    CoTaskMemFree(mix);
    ThrowIfFailed(initialized, "IAudioClient::Initialize");

    readyEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ThrowIfFailed(client->SetEventHandle(readyEvent_), "SetEventHandle");

    ComPtr<IAudioCaptureClient> capture;
    ThrowIfFailed(client->GetService(IID_PPV_ARGS(&capture)), "IAudioCaptureClient");

    device_ = device.Detach();
    client_ = client.Detach();
    capture_ = capture.Detach();
}

AudioCapture::~AudioCapture() {
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (capture_) {
        static_cast<IAudioCaptureClient*>(capture_)->Release();
    }
    if (client_) {
        auto* client = static_cast<IAudioClient*>(client_);
        client->Stop();
        client->Release();
    }
    if (device_) {
        static_cast<IMMDevice*>(device_)->Release();
    }
    if (readyEvent_) {
        CloseHandle(readyEvent_);
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
    }
}

void AudioCapture::Start() {
    ThrowIfFailed(static_cast<IAudioClient*>(client_)->Start(), "IAudioClient::Start");
    thread_ = std::thread([this] { Run(readyEvent_); });
}

void AudioCapture::Run(HANDLE readyEvent) {
    DWORD taskIndex = 0;
    const HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto* capture = static_cast<IAudioCaptureClient*>(capture_);
    std::vector<float> stereo;
    const HANDLE handles[] = {readyEvent, stopEvent_};
    while (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 200) != WAIT_OBJECT_0 + 1) {
        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }
            stereo.assign(std::size_t{frames} * 2, 0.0f);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                const auto* source = reinterpret_cast<const float*>(data);
                // Surround mixes keep front left and front right; mono is duplicated to both channels.
                const std::uint32_t rightOffset = channels_ >= 2 ? 1 : 0;
                for (UINT32 frame = 0; frame < frames; ++frame) {
                    stereo[frame * 2] = source[frame * channels_];
                    stereo[frame * 2 + 1] = source[frame * channels_ + rightOffset];
                }
            }
            capture->ReleaseBuffer(frames);
            sink_(stereo.data(), frames);
        }
    }

    if (task) {
        AvRevertMmThreadCharacteristics(task);
    }
}

}  // namespace aum
