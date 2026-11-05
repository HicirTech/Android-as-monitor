#pragma once

#include "../shared/stream_protocol.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace aum {

struct RunSummary {
    double capturedFps = 0;
    double encodedFps = 0;
    double decodedFps = 0;
    double megabitsPerSecond = 0;
    std::uint64_t droppedBeforeEncode = 0;
    std::uint64_t skippedByPhone = 0;
};

// Follows every frame across the PC and the phone so one run shows where each millisecond goes.
class LatencyStats {
public:
    explicit LatencyStats(FILE* csv);

    void OnCaptured();
    void OnAudio(const float* stereoSamples, std::uint32_t frames);
    void OnDroppedBeforeEncode();
    void OnSubmitted(std::uint64_t frameId, std::int64_t capturedNs, std::int64_t pickedUpNs, std::int64_t submittedNs);
    void OnEncoded(std::uint64_t frameId, std::int64_t encodedNs, std::uint32_t bytes);
    void OnSent(std::uint64_t frameId, std::int64_t sentNs);
    void OnPhoneStats(std::uint64_t frameId, const FrameStatsPayload& phone);
    void OnPong(std::int64_t pingSentNs, std::int64_t phoneNs, std::int64_t receivedNs);

    void PrintSecond(double refreshHz);
    RunSummary Summarize();
    void PrintEndToEnd();

    // For checking frames against what really reached the panel. It grows with every frame, so only test runs turn it on.
    void KeepCaptureTimes() { keepCaptureTimes_ = true; }
    std::map<std::int64_t, std::int64_t> CapturedBySchedule();

private:
    struct Trace {
        std::uint64_t frameId = UINT64_MAX;
        std::int64_t capturedNs = 0;
        std::int64_t pickedUpNs = 0;
        std::int64_t submittedNs = 0;
        std::int64_t encodedNs = 0;
        std::int64_t sentNs = 0;
        std::uint32_t bytes = 0;
    };
    struct Window {
        std::vector<double> pickup, convert, encode, send, link, decode, present, total, toDecoded;
        std::uint64_t captured = 0, encoded = 0, decoded = 0, skipped = 0, dropped = 0, bytes = 0, audioFrames = 0;
        float audioPeak = 0;
    };

    // From a stage's first frame to its last, so that connecting and starting up do not count against its frame rate.
    struct Rate {
        std::uint64_t count = 0;
        std::int64_t firstNs = 0;
        std::int64_t lastNs = 0;

        void Add(std::int64_t nowNs) {
            if (count++ == 0) {
                firstNs = nowNs;
            }
            lastNs = nowNs;
        }
        double PerSecond() const { return lastNs > firstNs ? static_cast<double>(count - 1) * 1e9 / static_cast<double>(lastNs - firstNs) : 0; }
    };

    static constexpr std::size_t kTraceCapacity = 4096;

    Trace& Slot(std::uint64_t frameId) { return traces_[frameId % kTraceCapacity]; }

    std::mutex mutex_;
    FILE* csv_;
    std::array<Trace, kTraceCapacity> traces_{};
    Window second_;
    Window whole_;
    Rate capturedRate_;
    Rate encodedRate_;
    Rate decodedRate_;
    std::int64_t lastPrintNs_ = 0;
    std::int64_t bestRttNs_ = INT64_MAX;
    std::int64_t bestRttAtNs_ = 0;
    std::int64_t phoneMinusPcNs_ = 0;
    bool keepCaptureTimes_ = false;
    std::map<std::int64_t, std::int64_t> capturedBySchedule_;
};

}  // namespace aum
