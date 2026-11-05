#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace aum {

// A steady test tone in place of a Windows playback endpoint: proves PC to phone sound without any Windows audio state.
class ToneSource {
public:
    using Sink = std::function<void(const float* samples, std::uint32_t frames)>;
    static constexpr std::uint32_t kSampleRate = 48000;

    explicit ToneSource(Sink sink);
    ~ToneSource();

    ToneSource(const ToneSource&) = delete;
    ToneSource& operator=(const ToneSource&) = delete;

    void Start();

private:
    Sink sink_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace aum
