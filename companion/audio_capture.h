#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace aum {

std::vector<std::wstring> ListRenderEndpoints();

// Captures what Windows plays to one playback endpoint, as interleaved stereo float32.
class AudioCapture {
public:
    using Sink = std::function<void(const float* samples, std::uint32_t frames)>;

    // An empty name means the default playback endpoint.
    AudioCapture(const std::wstring& endpointName, Sink sink);
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    std::uint32_t SampleRate() const { return sampleRate_; }
    const std::wstring& EndpointName() const { return endpointName_; }

    void Start();

private:
    void Run(HANDLE readyEvent);

    Sink sink_;
    std::wstring endpointName_;
    std::uint32_t sampleRate_ = 0;
    std::uint32_t channels_ = 0;
    void* device_ = nullptr;
    void* client_ = nullptr;
    void* capture_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::thread thread_;
};

}  // namespace aum
