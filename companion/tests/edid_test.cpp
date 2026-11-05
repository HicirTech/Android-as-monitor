#include "../edid.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static PanelSpec Panel(std::uint32_t width, std::uint32_t height, double refreshHz, double widthMm, double heightMm) {
    PanelSpec spec;
    spec.manufacturer = "samsung";
    spec.model = "SM-G991B";
    spec.deviceId = "0123456789abcdef";
    spec.width = width;
    spec.height = height;
    spec.refreshHz = refreshHz;
    spec.widthMm = widthMm;
    spec.heightMm = heightMm;
    return spec;
}

static void ExactRatesSurviveTheTenKilohertzClockStep() {
    struct Case { double reported; std::uint64_t numerator; std::uint64_t denominator; };
    const Case cases[] = {
        {60.000004, 60, 1}, {90.0, 90, 1}, {120.00001, 120, 1}, {119.99999, 120, 1}, {144.0, 144, 1}, {165.0, 165, 1},
        {59.94006, 60000, 1001}, {119.88012, 120000, 1001},
    };
    for (const Case& c : cases) {
        const Rational refresh = SnapRefreshRate(c.reported);
        const std::string label = "rate " + std::to_string(c.reported);
        Check(refresh.numerator == c.numerator && refresh.denominator == c.denominator, label + " snap");

        const auto edid = BuildEdid(Panel(2400, 1080, c.reported, 144.0, 65.0));
        const auto timing = ParseFirstDetailedTiming(edid.data(), edid.size());
        Check(timing.has_value(), label + " parses");
        const Rational parsed = RefreshRate(*timing);
        const double wanted = static_cast<double>(c.numerator) / static_cast<double>(c.denominator);
        Check(std::abs(parsed.value() - wanted) < 0.0005, label + " accuracy " + std::to_string(parsed.value()));
        if (c.denominator == 1) {
            Check(parsed.numerator == c.numerator && parsed.denominator == 1, label + " is an exact whole rate");
        }
    }
}

static void UnusualRateKeepsItsValue() {
    const auto edid = BuildEdid(Panel(2400, 1080, 96.5, 144.0, 65.0));
    const auto timing = ParseFirstDetailedTiming(edid.data(), edid.size());
    Check(std::abs(RefreshRate(*timing).value() - 96.5) < 0.0005, "96.5 Hz accuracy");
}

static void DetailedTimingCarriesTheNativeModeAndSize() {
    const auto edid = BuildEdid(Panel(2400, 1080, 120.00001, 143.6, 64.6));
    const auto timing = ParseFirstDetailedTiming(edid.data(), edid.size());
    Check(timing->hActive == 2400 && timing->vActive == 1080, "native resolution");
    Check(timing->widthMm == 144 && timing->heightMm == 65, "physical size in millimetres");
    Check(edid[21] == 14 && edid[22] == 6, "physical size in centimetres");
}

static void NoFallbackModesAreDeclared() {
    const auto edid = BuildEdid(Panel(2400, 1080, 120.0, 144.0, 65.0));
    Check(edid[35] == 0 && edid[36] == 0 && edid[37] == 0, "no established timings");
    for (int i = 38; i < 54; ++i) {
        Check(edid[i] == 0x01, "unused standard timing slot " + std::to_string(i));
    }
}

static void BlocksCarryValidChecksumsAndDeclareStereoAudio() {
    const auto edid = BuildEdid(Panel(2400, 1080, 120.0, 144.0, 65.0));
    Check(edid.size() == 2 * kEdidBlockSize && edid[126] == 1, "one extension block");
    Check(HasValidChecksum(edid.data()), "base block checksum");
    Check(HasValidChecksum(edid.data() + kEdidBlockSize), "extension checksum");
    const std::uint8_t* cta = edid.data() + kEdidBlockSize;
    Check(cta[0] == 0x02 && (cta[3] & 0x40) != 0, "CTA extension with basic audio");
    Check(cta[4] == 0x23 && cta[5] == 0x09, "audio data block declares two channel LPCM");
}

static void ModelNameIsLimitedAndTerminated() {
    PanelSpec spec = Panel(2400, 1080, 120.0, 144.0, 65.0);
    spec.model = "A-very-long-model-name";
    const auto edid = BuildEdid(spec);
    Check(std::string(reinterpret_cast<const char*>(&edid[77]), 13) == "A-very-long-m", "long name truncated to 13");

    const auto shortEdid = BuildEdid(Panel(2400, 1080, 120.0, 144.0, 65.0));
    Check(std::string(reinterpret_cast<const char*>(&shortEdid[77]), 13) == "SM-G991B\n    ", "short name terminated and padded");
}

static void UnrepresentablePanelsFailLoudly() {
    bool threw = false;
    try {
        BuildEdid(Panel(3840, 2160, 120.0, 150.0, 85.0));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Check(threw, "pixel clock above 655.35 MHz is refused");
}

int main() {
    ExactRatesSurviveTheTenKilohertzClockStep();
    UnusualRateKeepsItsValue();
    DetailedTimingCarriesTheNativeModeAndSize();
    NoFallbackModesAreDeclared();
    BlocksCarryValidChecksumsAndDeclareStereoAudio();
    ModelNameIsLimitedAndTerminated();
    UnrepresentablePanelsFailLoudly();
    std::printf(failures == 0 ? "edid tests passed\n" : "%d edid checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
