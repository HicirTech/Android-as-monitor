#include "phone_cadence.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace aum {

static constexpr std::int64_t kStillPending = std::numeric_limits<std::int64_t>::max();
static constexpr double kShareOfHistoryBetweenReads = 0.6;

void PhoneCadence::AddLatencyDump(const std::string& dump) {
    std::istringstream fields(dump);
    std::int64_t period = 0;
    if (!(fields >> period) || period <= 0) {
        return;
    }
    reportedPeriodNs_ = period;

    std::int64_t desired = 0;
    std::int64_t present = 0;
    std::int64_t ready = 0;
    std::int64_t first = 0;
    std::int64_t previous = 0;
    while (fields >> desired >> present >> ready) {
        if (present <= 0 || present == kStillPending || present < previous) {
            continue;
        }
        // Buffers replaced before they reached the panel share the present time of the one that did, which comes last.
        Frame& frame = frames_[present];
        frame.desiredNs = desired;
        if (present == previous) {
            continue;
        }
        if (previous != 0) {
            frame.previousPresentNs = previous;
        } else {
            first = present;
        }
        previous = present;
    }
    if (previous > first) {
        lastReadSpanNs_ = previous - first;
    }
}

void PhoneCadence::AddLoopOutput(std::string_view piece) {
    loopOutput_.append(piece);
    for (;;) {
        const std::size_t start = loopOutput_.find(kLatencyDumpMark);
        const std::size_t next = start == std::string::npos ? std::string::npos : loopOutput_.find(kLatencyDumpMark, start + kLatencyDumpMark.size());
        if (next == std::string::npos) {
            return;
        }
        AddLatencyDump(loopOutput_.substr(start + kLatencyDumpMark.size(), next - start - kLatencyDumpMark.size()));
        loopOutput_.erase(0, next);
    }
}

std::optional<PhoneCadenceSummary> PhoneCadence::Summarize() const {
    const double period = static_cast<double>(reportedPeriodNs_);
    const auto vsyncsIn = [period](std::int64_t gapNs) { return std::max<std::int64_t>(1, std::llround(static_cast<double>(gapNs) / period)); };

    std::size_t gaps = 0;
    std::int64_t spanNs = 0;
    std::int64_t repeated = 0;
    std::int64_t longestNs = 0;
    std::size_t late = 0;

    // A straight line through present time against vsync number gives the panel's real period. Reads may leave holes between
    // them; the reported period is close enough to number the vsyncs across one.
    std::int64_t vsync = 0;
    double sumX = 0;
    double sumY = 0;
    double sumXY = 0;
    double sumXX = 0;
    const std::int64_t origin = frames_.empty() ? 0 : frames_.begin()->first;
    std::int64_t lastPresent = origin;

    for (const auto& [present, frame] : frames_) {
        if (present - frame.desiredNs > reportedPeriodNs_) {
            ++late;
        }
        if (frame.previousPresentNs != 0) {
            const std::int64_t gap = present - frame.previousPresentNs;
            ++gaps;
            spanNs += gap;
            repeated += vsyncsIn(gap) - 1;
            longestNs = std::max(longestNs, gap);
        }
        if (present != origin) {
            vsync += vsyncsIn(present - lastPresent);
        }
        lastPresent = present;
        const double x = static_cast<double>(vsync);
        const double y = static_cast<double>(present - origin);
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    if (gaps == 0) {
        return std::nullopt;
    }

    const double count = static_cast<double>(frames_.size());
    const double measuredPeriod = (count * sumXY - sumX * sumY) / (count * sumXX - sumX * sumX);

    PhoneCadenceSummary summary;
    summary.framesOnScreen = frames_.size();
    summary.framesPerSecond = static_cast<double>(gaps) * 1e9 / static_cast<double>(spanNs);
    summary.panelHz = 1e9 / measuredPeriod;
    summary.repeatedVsyncShare = static_cast<double>(repeated) / static_cast<double>(static_cast<std::int64_t>(gaps) + repeated);
    summary.longestGapMs = static_cast<double>(longestNs) / 1e6;
    summary.lateShare = static_cast<double>(late) / count;
    return summary;
}

std::vector<double> PhoneCadence::CaptureToScreenMs(const std::map<std::int64_t, std::int64_t>& capturedBySchedule) const {
    std::vector<double> latencies;
    for (const auto& [present, frame] : frames_) {
        // Frames are released half a period before their vsync. The period the app used can differ a little from the reported one.
        const std::int64_t scheduled = frame.desiredNs + reportedPeriodNs_ / 2;
        const auto match = capturedBySchedule.lower_bound(scheduled - reportedPeriodNs_ / 4);
        if (match != capturedBySchedule.end() && match->first < scheduled + reportedPeriodNs_ / 4) {
            latencies.push_back(static_cast<double>(present - match->second) / 1e6);
        }
    }
    return latencies;
}

unsigned long PhoneCadence::ReadIntervalMs() const {
    return static_cast<unsigned long>(static_cast<double>(lastReadSpanNs_) * kShareOfHistoryBetweenReads / 1e6);
}

}  // namespace aum
