#include "../edid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aum;

static int failures = 0;

static void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

static void FixBlockChecksum(std::uint8_t* block) {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < kEdidBlockSize - 1; ++i) {
        sum = static_cast<std::uint8_t>(sum + block[i]);
    }
    block[kEdidBlockSize - 1] = static_cast<std::uint8_t>(0x100 - sum);
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
        const auto timing = ParseNativeTiming(edid.data(), edid.size());
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
    const auto timing = ParseNativeTiming(edid.data(), edid.size());
    Check(std::abs(RefreshRate(*timing).value() - 96.5) < 0.0005, "96.5 Hz accuracy");
}

static void DetailedTimingCarriesTheNativeModeAndSize() {
    const auto edid = BuildEdid(Panel(2400, 1080, 120.00001, 143.6, 64.6));
    const auto timing = ParseNativeTiming(edid.data(), edid.size());
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

static bool SectionChecksumIsValid(const std::uint8_t* displayId) {
    std::uint8_t sum = 0;
    for (std::size_t i = 1; i <= kDisplayIdFirstDataBlockOffset + displayId[2]; ++i) {
        sum = static_cast<std::uint8_t>(sum + displayId[i]);
    }
    return sum == 0;
}

static void RatesBeyondTheDetailedTimingClockAreStatedInDisplayId() {
    struct Case { std::uint32_t width; std::uint32_t height; double reported; std::uint64_t exactHz; std::uint64_t baseHz; };
    const Case cases[] = {
        {2960, 1848, 120.00001, 120, 60},
        {2880, 1800, 144.0, 144, 72},
        {3840, 2160, 120.0, 120, 60},
        {3840, 2400, 240.0, 240, 60},
    };
    for (const Case& c : cases) {
        const std::string label = std::to_string(c.width) + "x" + std::to_string(c.height) + "@" + std::to_string(c.exactHz);
        const auto edid = BuildEdid(Panel(c.width, c.height, c.reported, 314.0, 196.0));

        Check(edid.size() == 3 * kEdidBlockSize && edid[kExtensionCountOffset] == 2, label + " has two extension blocks");
        for (std::size_t block = 0; block < 3; ++block) {
            Check(HasValidChecksum(edid.data() + block * kEdidBlockSize), label + " block checksum " + std::to_string(block));
        }
        const std::uint8_t* displayId = edid.data() + 2 * kEdidBlockSize;
        Check(displayId[0] == kDisplayIdExtensionTag && displayId[1] == 0x13, label + " is a DisplayID 1.3 extension");
        Check(SectionChecksumIsValid(displayId), label + " section checksum");

        const auto native = ParseNativeTiming(edid.data(), edid.size());
        Check(native.has_value(), label + " parses");
        Check(native->hActive == c.width && native->vActive == c.height, label + " native resolution");
        const Rational rate = RefreshRate(*native);
        Check(rate.numerator == c.exactHz && rate.denominator == 1, label + " exact native rate, got " + std::to_string(rate.value()));
        Check(native->pixelClockHz > 65535 * kDetailedTimingClockUnitHz, label + " clock is beyond a detailed timing");
        Check(native->widthMm == 314 && native->heightMm == 196, label + " physical size in millimetres");
        Check(edid[21] == 31 && edid[22] == 20, label + " physical size in centimetres");

        // Reading only the base block is what a parser without DisplayID sees: the same panel, at a fraction of its rate.
        const auto base = ParseNativeTiming(edid.data(), kEdidBlockSize);
        Check(base.has_value() && base->hActive == c.width && base->vActive == c.height, label + " base block resolution");
        const Rational baseRate = RefreshRate(*base);
        Check(baseRate.numerator == c.baseHz && baseRate.denominator == 1, label + " base block rate, got " + std::to_string(baseRate.value()));
    }
}

static void PanelsWithinTheDetailedTimingClockNeedNoDisplayId() {
    const auto edid = BuildEdid(Panel(2880, 1800, 120.0, 300.0, 188.0));
    Check(edid.size() == 2 * kEdidBlockSize && edid[kExtensionCountOffset] == 1, "a clock that fits stays in two blocks");
    const Rational rate = RefreshRate(*ParseNativeTiming(edid.data(), edid.size()));
    Check(rate.numerator == 120 && rate.denominator == 1, "and keeps its exact rate");
}

static void TheDisplayIdTimingIsFoundBehindOtherDataBlocks() {
    auto edid = BuildEdid(Panel(2960, 1848, 120.0, 314.0, 196.0));
    std::uint8_t* displayId = edid.data() + 2 * kEdidBlockSize;
    // Real monitors put a 12 byte display parameters block first.
    static constexpr std::size_t kParametersBytes = kDisplayIdDataBlockHeaderBytes + 12;
    static constexpr std::size_t kTimingBlockBytes = kDisplayIdDataBlockHeaderBytes + kDisplayIdTypeOneTimingBytes;
    std::copy_backward(displayId + kDisplayIdFirstDataBlockOffset, displayId + kDisplayIdFirstDataBlockOffset + kTimingBlockBytes,
                       displayId + kDisplayIdFirstDataBlockOffset + kParametersBytes + kTimingBlockBytes);
    std::fill(displayId + kDisplayIdFirstDataBlockOffset, displayId + kDisplayIdFirstDataBlockOffset + kParametersBytes, std::uint8_t{0});
    displayId[kDisplayIdFirstDataBlockOffset] = 0x01;
    displayId[kDisplayIdFirstDataBlockOffset + 2] = 12;
    displayId[2] = static_cast<std::uint8_t>(kParametersBytes + kTimingBlockBytes);
    FixBlockChecksum(displayId);

    const auto native = ParseNativeTiming(edid.data(), edid.size());
    Check(native.has_value() && native->hActive == 2960 && RefreshRate(*native).value() == 120.0, "the timing behind a parameters block is found");
}

static void AMalformedDisplayIdFallsBackToTheBaseBlock() {
    const auto good = BuildEdid(Panel(2960, 1848, 120.0, 314.0, 196.0));
    const auto expectBase = [](const std::vector<std::uint8_t>& edid, const char* what) {
        const auto timing = ParseNativeTiming(edid.data(), edid.size());
        Check(timing.has_value() && RefreshRate(*timing).value() == 60.0, what);
    };

    auto badChecksum = good;
    badChecksum[2 * kEdidBlockSize + kEdidBlockSize - 1] ^= 0xFF;
    expectBase(badChecksum, "a block with a wrong checksum is not read");

    auto sectionTooLong = good;
    sectionTooLong[2 * kEdidBlockSize + 2] = 200;
    FixBlockChecksum(sectionTooLong.data() + 2 * kEdidBlockSize);
    expectBase(sectionTooLong, "a section longer than its block is not read");

    auto dataBlockTooLong = good;
    dataBlockTooLong[2 * kEdidBlockSize + kDisplayIdFirstDataBlockOffset + 2] = 100;
    FixBlockChecksum(dataBlockTooLong.data() + 2 * kEdidBlockSize);
    expectBase(dataBlockTooLong, "a data block longer than its section is not read");

    auto truncated = good;
    truncated.resize(2 * kEdidBlockSize);
    expectBase(truncated, "an extension past the end of the data is not read");
}

static void UnrepresentablePanelsFailLoudly() {
    bool threw = false;
    try {
        BuildEdid(Panel(5120, 2880, 60.0, 600.0, 340.0));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Check(threw, "a panel wider than the base block's timing can state is refused");
}

int main() {
    ExactRatesSurviveTheTenKilohertzClockStep();
    UnusualRateKeepsItsValue();
    DetailedTimingCarriesTheNativeModeAndSize();
    NoFallbackModesAreDeclared();
    BlocksCarryValidChecksumsAndDeclareStereoAudio();
    ModelNameIsLimitedAndTerminated();
    RatesBeyondTheDetailedTimingClockAreStatedInDisplayId();
    PanelsWithinTheDetailedTimingClockNeedNoDisplayId();
    TheDisplayIdTimingIsFoundBehindOtherDataBlocks();
    AMalformedDisplayIdFallsBackToTheBaseBlock();
    UnrepresentablePanelsFailLoudly();
    std::printf(failures == 0 ? "edid tests passed\n" : "%d edid checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
