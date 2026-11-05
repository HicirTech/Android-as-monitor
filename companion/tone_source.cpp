#include "tone_source.h"

#include "clock.h"

#include <cmath>
#include <vector>

namespace aum {

static constexpr double kToneHz = 440.0;
static constexpr float kAmplitude = 0.1f;
static constexpr std::uint32_t kPacketFrames = 480;
static constexpr double kTwoPi = 6.283185307179586;

ToneSource::ToneSource(Sink sink) : sink_(std::move(sink)) {}

ToneSource::~ToneSource() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ToneSource::Start() {
    thread_ = std::thread([this] {
        const std::int64_t packetNs = std::int64_t{kPacketFrames} * 1'000'000'000 / kSampleRate;
        std::vector<float> samples(std::size_t{kPacketFrames} * 2);
        double phase = 0;
        std::int64_t nextNs = NowNs();
        while (!stopping_) {
            for (std::uint32_t frame = 0; frame < kPacketFrames; ++frame) {
                const float value = kAmplitude * static_cast<float>(std::sin(phase));
                samples[frame * 2] = value;
                samples[frame * 2 + 1] = value;
                phase = std::fmod(phase + kTwoPi * kToneHz / kSampleRate, kTwoPi);
            }
            sink_(samples.data(), kPacketFrames);
            nextNs += packetNs;
            const std::int64_t waitMs = (nextNs - NowNs()) / 1'000'000;
            if (waitMs > 0) {
                Sleep(static_cast<DWORD>(waitMs));
            }
        }
    });
}

}  // namespace aum
