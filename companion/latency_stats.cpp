#include "latency_stats.h"

#include "clock.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace aum {

// A new minimum-RTT clock sample is trusted immediately; older ones expire so that drift cannot accumulate.
static constexpr std::int64_t kClockSampleLifetimeNs = 5'000'000'000;

// "n/a" for a stage with no samples: a zero would read as no latency at all, and not every phone reports every stage.
static std::string Percentile(std::vector<double> values, double fraction, int decimals = 1) {
    if (values.empty()) {
        return "n/a";
    }
    const std::size_t index = std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, values[index]);
    return text;
}

static double Ms(std::int64_t ns) {
    return static_cast<double>(ns) / 1e6;
}

LatencyStats::LatencyStats(FILE* csv) : csv_(csv) {
    lastPrintNs_ = NowNs();
    if (csv_) {
        std::fprintf(csv_, "frame,bytes,pickup_ms,convert_ms,encode_ms,send_ms,link_ms,decode_ms,present_ms,total_ms,decoded_ns,present_ns\n");
    }
}

void LatencyStats::OnCaptured() {
    const std::lock_guard lock(mutex_);
    capturedRate_.Add(NowNs());
    ++second_.captured;
    ++whole_.captured;
}

void LatencyStats::OnAudio(const float* stereoSamples, std::uint32_t frames) {
    float peak = 0;
    for (std::uint32_t index = 0; index < frames * 2; ++index) {
        peak = std::max(peak, std::abs(stereoSamples[index]));
    }
    const std::lock_guard lock(mutex_);
    for (Window* window : {&second_, &whole_}) {
        window->audioFrames += frames;
        window->audioPeak = std::max(window->audioPeak, peak);
    }
}

void LatencyStats::OnDroppedBeforeEncode() {
    const std::lock_guard lock(mutex_);
    ++second_.dropped;
    ++whole_.dropped;
}

void LatencyStats::OnSubmitted(std::uint64_t frameId, std::int64_t capturedNs, std::int64_t pickedUpNs, std::int64_t submittedNs) {
    const std::lock_guard lock(mutex_);
    Trace& trace = Slot(frameId);
    trace = {};
    trace.frameId = frameId;
    trace.capturedNs = capturedNs;
    trace.pickedUpNs = pickedUpNs;
    trace.submittedNs = submittedNs;
}

void LatencyStats::OnEncoded(std::uint64_t frameId, std::int64_t encodedNs, std::uint32_t bytes) {
    const std::lock_guard lock(mutex_);
    Trace& trace = Slot(frameId);
    if (trace.frameId != frameId) {
        return;
    }
    trace.encodedNs = encodedNs;
    trace.bytes = bytes;
    encodedRate_.Add(encodedNs);
    ++second_.encoded;
    ++whole_.encoded;
    second_.bytes += bytes;
    whole_.bytes += bytes;
    const double encode = Ms(encodedNs - trace.submittedNs);
    const double pickup = Ms(trace.pickedUpNs - trace.capturedNs);
    const double convert = Ms(trace.submittedNs - trace.pickedUpNs);
    for (Window* window : {&second_, &whole_}) {
        window->pickup.push_back(pickup);
        window->convert.push_back(convert);
        window->encode.push_back(encode);
    }
}

void LatencyStats::OnSent(std::uint64_t frameId, std::int64_t sentNs) {
    const std::lock_guard lock(mutex_);
    Trace& trace = Slot(frameId);
    if (trace.frameId != frameId) {
        return;
    }
    trace.sentNs = sentNs;
    const double send = Ms(sentNs - trace.encodedNs);
    second_.send.push_back(send);
    whole_.send.push_back(send);
}

void LatencyStats::OnPhoneStats(std::uint64_t frameId, const FrameStatsPayload& phone) {
    const std::lock_guard lock(mutex_);
    const Trace& trace = Slot(frameId);
    if (trace.frameId != frameId || trace.sentNs == 0 || bestRttNs_ == INT64_MAX) {
        return;
    }
    const double link = Ms(phone.receivedNs - phoneMinusPcNs_ - trace.sentNs);
    const double decode = Ms(phone.decodedNs - phone.receivedNs);
    const double toDecoded = Ms(phone.decodedNs - phoneMinusPcNs_ - trace.capturedNs);
    decodedRate_.Add(NowNs());
    for (Window* window : {&second_, &whole_}) {
        window->link.push_back(link);
        window->decode.push_back(decode);
        window->toDecoded.push_back(toDecoded);
        ++window->decoded;
    }
    if (phone.presentNs == 0) {
        ++second_.skipped;
        ++whole_.skipped;
        return;
    }
    if (keepCaptureTimes_) {
        capturedBySchedule_[phone.presentNs] = trace.capturedNs + phoneMinusPcNs_;
    }
    const double present = Ms(phone.presentNs - phone.decodedNs);
    const double total = Ms(phone.presentNs - phoneMinusPcNs_ - trace.capturedNs);
    for (Window* window : {&second_, &whole_}) {
        window->present.push_back(present);
        window->total.push_back(total);
    }
    if (csv_) {
        std::fprintf(csv_, "%llu,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%lld,%lld\n", frameId, trace.bytes,
                     Ms(trace.pickedUpNs - trace.capturedNs), Ms(trace.submittedNs - trace.pickedUpNs),
                     Ms(trace.encodedNs - trace.submittedNs), Ms(trace.sentNs - trace.encodedNs), link, decode, present, total, phone.decodedNs, phone.presentNs);
    }
}

void LatencyStats::OnPong(std::int64_t pingSentNs, std::int64_t phoneNs, std::int64_t receivedNs) {
    const std::lock_guard lock(mutex_);
    const std::int64_t rtt = receivedNs - pingSentNs;
    if (rtt <= bestRttNs_ || receivedNs - bestRttAtNs_ > kClockSampleLifetimeNs) {
        bestRttNs_ = rtt;
        bestRttAtNs_ = receivedNs;
        phoneMinusPcNs_ = phoneNs - (pingSentNs + rtt / 2);
    }
}

void LatencyStats::PrintSecond(double refreshHz) {
    Window second;
    double elapsed = 0;
    {
        const std::lock_guard lock(mutex_);
        const std::int64_t now = NowNs();
        elapsed = Ms(now - lastPrintNs_) / 1000.0;
        lastPrintNs_ = now;
        std::swap(second, second_);
    }
    if (elapsed <= 0) {
        return;
    }
    // Outside the lock: writing to a console can take milliseconds, and the encoder thread needs that lock for every frame.
    std::printf(
        "cap %5.1f enc %5.1f dec %5.1f/%.0f fps | %6.1f Mbps | pickup %4s convert %4s encode %4s send %4s link %4s decode %4s present %4s | total p50 %5s p95 %5s ms | dropped %llu skipped %llu | audio %.1f kHz peak %.0f dB\n",
        static_cast<double>(second.captured) / elapsed, static_cast<double>(second.encoded) / elapsed, static_cast<double>(second.decoded) / elapsed,
        refreshHz, static_cast<double>(second.bytes) * 8 / 1e6 / elapsed,
        Percentile(second.pickup, 0.5).c_str(), Percentile(second.convert, 0.5).c_str(), Percentile(second.encode, 0.5).c_str(),
        Percentile(second.send, 0.5).c_str(), Percentile(second.link, 0.5).c_str(), Percentile(second.decode, 0.5).c_str(),
        Percentile(second.present, 0.5).c_str(), Percentile(second.total, 0.5).c_str(), Percentile(second.total, 0.95).c_str(), second.dropped, second.skipped,
        static_cast<double>(second.audioFrames) / 1000 / elapsed, second.audioPeak > 0 ? 20 * std::log10(second.audioPeak) : -120.0);
    std::fflush(stdout);
}

RunSummary LatencyStats::Summarize() {
    const std::lock_guard lock(mutex_);
    RunSummary summary;
    summary.capturedFps = capturedRate_.PerSecond();
    summary.encodedFps = encodedRate_.PerSecond();
    summary.decodedFps = decodedRate_.PerSecond();
    const double encodingSeconds = static_cast<double>(encodedRate_.lastNs - encodedRate_.firstNs) / 1e9;
    summary.megabitsPerSecond = encodingSeconds > 0 ? static_cast<double>(whole_.bytes) * 8 / 1e6 / encodingSeconds : 0;
    summary.droppedBeforeEncode = whole_.dropped;
    summary.skippedByPhone = whole_.skipped;
    const auto printStages = [this](const char* label, double fraction) {
        const auto stage = [fraction](const std::vector<double>& values) { return Percentile(values, fraction, 2); };
        std::printf("%s pickup %s, convert %s, encode %s, send %s, link %s, decode %s, present %s\n", label, stage(whole_.pickup).c_str(),
                    stage(whole_.convert).c_str(), stage(whole_.encode).c_str(), stage(whole_.send).c_str(), stage(whole_.link).c_str(),
                    stage(whole_.decode).c_str(), stage(whole_.present).c_str());
    };
    printStages("\nmedian ms:", 0.5);
    printStages("p95 ms:   ", 0.95);
    printStages("p99.9 ms: ", 0.999);
    return summary;
}

std::map<std::int64_t, std::int64_t> LatencyStats::CapturedBySchedule() {
    const std::lock_guard lock(mutex_);
    return capturedBySchedule_;
}

void LatencyStats::PrintEndToEnd() {
    const std::lock_guard lock(mutex_);
    std::printf("capture to decoder output: p50 %s ms, p95 %s ms\n", Percentile(whole_.toDecoded, 0.5).c_str(), Percentile(whole_.toDecoded, 0.95).c_str());
    std::printf("capture to scheduled vsync: p50 %s ms, p95 %s ms\n", Percentile(whole_.total, 0.5).c_str(), Percentile(whole_.total, 0.95).c_str());
}

}  // namespace aum
