#include "edid.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace aum {

namespace {

constexpr std::uint32_t kMinHBlank = 80;
constexpr std::uint32_t kMaxHBlankSearch = 400;
constexpr std::uint32_t kMinVBlank = 8;
constexpr std::uint32_t kMaxVBlankSearch = 400;
constexpr std::uint32_t kHFrontPorch = 8;
constexpr std::uint32_t kHSyncWidth = 32;
constexpr std::uint32_t kVFrontPorch = 3;
constexpr std::uint32_t kVSyncWidth = 5;
constexpr std::uint32_t kMaxDetailedActive = 4095;
constexpr std::uint32_t kMaxDetailedBlank = 4095;
constexpr std::uint64_t kMaxDetailedClockUnits = 65535;
constexpr std::uint64_t kMaxDisplayIdClockUnits = 0xFFFFFF;
constexpr std::uint32_t kMaxDetailedImageMm = 4095;
constexpr double kSnapToleranceHz = 0.02;

constexpr std::size_t kDescriptorSize = 18;
constexpr std::size_t kNameDescriptorOffset = 72;
constexpr std::size_t kSerialDescriptorOffset = 90;
constexpr std::size_t kDummyDescriptorOffset = 108;
constexpr std::uint8_t kNameDescriptorTag = 0xFC;
constexpr std::uint8_t kSerialDescriptorTag = 0xFF;
constexpr std::uint8_t kDummyDescriptorTag = 0x10;

std::uint64_t Fnv1a64(const std::string& text, std::uint64_t seed = 0xCBF29CE484222325ull) {
    std::uint64_t hash = seed;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::uint16_t EncodePnpId(const char (&id)[4]) {
    return static_cast<std::uint16_t>(((id[0] - 'A' + 1) << 10) | ((id[1] - 'A' + 1) << 5) | (id[2] - 'A' + 1));
}

void WriteDetailedTiming(std::uint8_t* d, const DetailedTiming& t) {
    const std::uint64_t clockUnits = t.pixelClockHz / kDetailedTimingClockUnitHz;
    d[0] = static_cast<std::uint8_t>(clockUnits & 0xFF);
    d[1] = static_cast<std::uint8_t>(clockUnits >> 8);
    d[2] = static_cast<std::uint8_t>(t.hActive & 0xFF);
    d[3] = static_cast<std::uint8_t>(t.hBlank & 0xFF);
    d[4] = static_cast<std::uint8_t>(((t.hActive >> 8) << 4) | (t.hBlank >> 8));
    d[5] = static_cast<std::uint8_t>(t.vActive & 0xFF);
    d[6] = static_cast<std::uint8_t>(t.vBlank & 0xFF);
    d[7] = static_cast<std::uint8_t>(((t.vActive >> 8) << 4) | (t.vBlank >> 8));
    d[8] = kHFrontPorch;
    d[9] = kHSyncWidth;
    d[10] = static_cast<std::uint8_t>((kVFrontPorch << 4) | kVSyncWidth);
    d[11] = 0;
    d[12] = static_cast<std::uint8_t>(t.widthMm & 0xFF);
    d[13] = static_cast<std::uint8_t>(t.heightMm & 0xFF);
    d[14] = static_cast<std::uint8_t>(((t.widthMm >> 8) << 4) | (t.heightMm >> 8));
    d[15] = 0;
    d[16] = 0;
    // Non-interlaced, digital separate sync, both polarities positive.
    d[17] = 0x1E;
}

void WriteTextDescriptor(std::uint8_t* d, std::uint8_t tag, const std::string& text) {
    static constexpr std::size_t kTextCapacity = 13;
    d[3] = tag;
    std::size_t index = 0;
    for (; index < std::min(text.size(), kTextCapacity); ++index) {
        d[5 + index] = static_cast<std::uint8_t>(text[index]);
    }
    if (index < kTextCapacity) {
        d[5 + index++] = 0x0A;
    }
    for (; index < kTextCapacity; ++index) {
        d[5 + index] = 0x20;
    }
}

void WriteChecksum(std::uint8_t* block) {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < kEdidBlockSize - 1; ++i) {
        sum = static_cast<std::uint8_t>(sum + block[i]);
    }
    block[kEdidBlockSize - 1] = static_cast<std::uint8_t>(0x100 - sum);
}

// The same CTA-861 blocks an HDMI or DisplayPort monitor with speakers declares: stereo LPCM at 32, 44.1 and 48 kHz.
void WriteCtaExtension(std::uint8_t* block) {
    static constexpr std::uint8_t kBasicAudioFlag = 0x40;
    static constexpr std::uint8_t kDataBlocks[] = {
        0x23, 0x09, 0x07, 0x07,  // Audio data block: LPCM, 2 channels, 32/44.1/48 kHz, 16/20/24 bit.
        0x83, 0x01, 0x00, 0x00,  // Speaker allocation: front left and right.
    };
    block[0] = 0x02;
    block[1] = 0x03;
    block[2] = static_cast<std::uint8_t>(4 + sizeof(kDataBlocks));
    block[3] = kBasicAudioFlag;
    std::copy(std::begin(kDataBlocks), std::end(kDataBlocks), block + 4);
    WriteChecksum(block);
}

// DisplayID 1.3 with one Type I timing, which is where high refresh monitors state a mode whose clock needs more than 16 bits.
void WriteDisplayIdExtension(std::uint8_t* block, const DetailedTiming& t) {
    static constexpr std::uint8_t kDisplayIdVersion = 0x13;
    static constexpr std::uint8_t kPreferredWithUndefinedAspectRatio = 0x88;
    static constexpr std::uint32_t kPositiveSyncPolarity = 0x8000;
    static constexpr std::size_t kSectionBytes = kDisplayIdDataBlockHeaderBytes + kDisplayIdTypeOneTimingBytes;

    block[0] = kDisplayIdExtensionTag;
    block[1] = kDisplayIdVersion;
    block[2] = static_cast<std::uint8_t>(kSectionBytes);
    // Bytes 3 and 4 stay zero: this is an extension section, and no further sections follow it.
    std::uint8_t* dataBlock = block + kDisplayIdFirstDataBlockOffset;
    dataBlock[0] = kDisplayIdTypeOneTimingTag;
    dataBlock[2] = static_cast<std::uint8_t>(kDisplayIdTypeOneTimingBytes);

    // Type I fields hold each value minus one.
    std::uint8_t* d = dataBlock + kDisplayIdDataBlockHeaderBytes;
    const std::uint64_t clockUnits = t.pixelClockHz / kDetailedTimingClockUnitHz - 1;
    d[0] = static_cast<std::uint8_t>(clockUnits & 0xFF);
    d[1] = static_cast<std::uint8_t>((clockUnits >> 8) & 0xFF);
    d[2] = static_cast<std::uint8_t>((clockUnits >> 16) & 0xFF);
    d[3] = kPreferredWithUndefinedAspectRatio;
    const auto writeField = [d](std::size_t offset, std::uint32_t value) {
        d[offset] = static_cast<std::uint8_t>(value & 0xFF);
        d[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    writeField(4, t.hActive - 1);
    writeField(6, t.hBlank - 1);
    writeField(8, (kHFrontPorch - 1) | kPositiveSyncPolarity);
    writeField(10, kHSyncWidth - 1);
    writeField(12, t.vActive - 1);
    writeField(14, t.vBlank - 1);
    writeField(16, (kVFrontPorch - 1) | kPositiveSyncPolarity);
    writeField(18, kVSyncWidth - 1);

    // The section has its own checksum, over everything after the tag, in the byte right after its data blocks.
    const std::size_t checksumOffset = kDisplayIdFirstDataBlockOffset + kSectionBytes;
    std::uint8_t sum = 0;
    for (std::size_t i = 1; i < checksumOffset; ++i) {
        sum = static_cast<std::uint8_t>(sum + block[i]);
    }
    block[checksumOffset] = static_cast<std::uint8_t>(0x100 - sum);
    WriteChecksum(block);
}

}  // namespace

Rational SnapRefreshRate(double reportedHz) {
    const double ntscWhole = std::round(reportedHz * 1.001);
    if (std::abs(reportedHz - ntscWhole / 1.001) < kSnapToleranceHz) {
        return {static_cast<std::uint64_t>(ntscWhole) * 1000, 1001};
    }
    return Reduce(static_cast<std::uint64_t>(std::round(reportedHz * 1000.0)), 1000);
}

std::optional<DetailedTiming> ChooseTiming(std::uint32_t width, std::uint32_t height, Rational refresh, std::uint32_t widthMm, std::uint32_t heightMm, std::uint64_t maxClockUnits) {
    if (width > kMaxDetailedActive || height > kMaxDetailedActive) {
        throw std::runtime_error("panel is larger than the detailed timing every EDID must hold can describe");
    }
    if (widthMm == 0 || heightMm == 0 || widthMm > kMaxDetailedImageMm || heightMm > kMaxDetailedImageMm) {
        throw std::runtime_error("panel physical size is outside what a detailed timing can describe");
    }

    const double target = refresh.value();
    DetailedTiming best{};
    double bestError = std::numeric_limits<double>::max();
    for (std::uint32_t vBlank = kMinVBlank; vBlank <= kMaxVBlankSearch && bestError > 0; ++vBlank) {
        for (std::uint32_t hBlank = kMinHBlank; hBlank <= kMaxHBlankSearch && bestError > 0; ++hBlank) {
            const std::uint64_t total = std::uint64_t{width + hBlank} * (height + vBlank);
            const std::uint64_t idealClockHz = total * refresh.numerator / refresh.denominator;
            const std::uint64_t units = (idealClockHz + kDetailedTimingClockUnitHz / 2) / kDetailedTimingClockUnitHz;
            if (units == 0 || units > maxClockUnits) {
                continue;
            }
            // Exact integer comparison: units * 10000 / total == numerator / denominator.
            const std::uint64_t left = units * kDetailedTimingClockUnitHz * refresh.denominator;
            const std::uint64_t right = total * refresh.numerator;
            const double error = left == right ? 0.0 : std::abs(static_cast<double>(units * kDetailedTimingClockUnitHz) / static_cast<double>(total) - target) + 1e-12;
            if (error < bestError) {
                bestError = error;
                best = {width, height, hBlank, vBlank, units * kDetailedTimingClockUnitHz, widthMm, heightMm};
            }
        }
    }
    if (best.pixelClockHz == 0) {
        return std::nullopt;
    }
    if (best.hBlank > kMaxDetailedBlank || best.vBlank > kMaxDetailedBlank) {
        throw std::runtime_error("blanking search produced an unencodable timing");
    }
    return best;
}

std::vector<std::uint8_t> BuildEdid(const PanelSpec& spec) {
    // Not a registered PNP vendor ID, which a shipping product would need.
    static constexpr char kPnpId[4] = "AUM";

    const std::uint32_t widthMm = static_cast<std::uint32_t>(std::lround(spec.widthMm));
    const std::uint32_t heightMm = static_cast<std::uint32_t>(std::lround(spec.heightMm));
    const Rational refresh = SnapRefreshRate(spec.refreshHz);
    const auto timingAt = [&](Rational rate, std::uint64_t maxClockUnits) { return ChooseTiming(spec.width, spec.height, rate, widthMm, heightMm, maxClockUnits); };

    std::optional<DetailedTiming> baseTiming = timingAt(refresh, kMaxDetailedClockUnits);
    std::optional<DetailedTiming> displayIdTiming;
    if (!baseTiming) {
        displayIdTiming = timingAt(refresh, kMaxDisplayIdClockUnits);
        if (!displayIdTiming) {
            throw std::runtime_error("no timing reaches the panel's refresh rate of " + std::to_string(refresh.value()) + " Hz");
        }
        // The base block must hold a detailed timing, so it states the panel at a fraction of its rate. That never becomes a mode:
        // the driver offers only the native timing, and Windows takes the modes of an indirect display from its driver.
        Rational lowerRate = refresh;
        while (!baseTiming) {
            lowerRate = Reduce(lowerRate.numerator, lowerRate.denominator * 2);
            baseTiming = timingAt(lowerRate, kMaxDetailedClockUnits);
        }
    }

    const std::size_t extensionCount = displayIdTiming ? 2 : 1;
    std::vector<std::uint8_t> edid((1 + extensionCount) * kEdidBlockSize, 0);
    std::uint8_t* base = edid.data();

    static constexpr std::uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::copy(std::begin(kHeader), std::end(kHeader), base);

    const std::uint16_t pnp = EncodePnpId(kPnpId);
    base[8] = static_cast<std::uint8_t>(pnp >> 8);
    base[9] = static_cast<std::uint8_t>(pnp & 0xFF);

    const std::uint16_t productCode = static_cast<std::uint16_t>(Fnv1a64(spec.manufacturer + "/" + spec.model) & 0xFFFF);
    base[10] = static_cast<std::uint8_t>(productCode & 0xFF);
    base[11] = static_cast<std::uint8_t>(productCode >> 8);

    const std::uint32_t serial = static_cast<std::uint32_t>(Fnv1a64(spec.deviceId) & 0xFFFFFFFF);
    for (int i = 0; i < 4; ++i) {
        base[12 + i] = static_cast<std::uint8_t>(serial >> (8 * i));
    }

    SYSTEMTIME now;
    GetSystemTime(&now);
    base[16] = 0xFF;  // Week 0xFF marks byte 17 as a model year.
    base[17] = static_cast<std::uint8_t>(now.wYear - 1990);
    base[18] = 1;     // EDID 1.4
    base[19] = 4;
    base[20] = 0xA0;  // Digital input, 8 bits per colour, interface not specified.
    base[21] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(spec.widthMm / 10.0), 1, 255));
    base[22] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(spec.heightMm / 10.0), 1, 255));
    base[23] = 120;   // Gamma 2.2
    base[24] = 0x06;  // sRGB default, preferred timing is the native mode, non-continuous frequency.

    // sRGB chromaticity coordinates.
    static constexpr std::uint8_t kChromaticity[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
    std::copy(std::begin(kChromaticity), std::end(kChromaticity), base + 25);

    // No established or standard timings: the detailed timing is the only mode this monitor has.
    std::fill(base + 38, base + 54, std::uint8_t{0x01});

    WriteDetailedTiming(base + kFirstDetailedTimingOffset, *baseTiming);
    WriteTextDescriptor(base + kNameDescriptorOffset, kNameDescriptorTag, spec.model);
    char serialText[9];
    sprintf_s(serialText, "%08X", serial);
    WriteTextDescriptor(base + kSerialDescriptorOffset, kSerialDescriptorTag, serialText);
    (base + kDummyDescriptorOffset)[3] = kDummyDescriptorTag;
    static_assert(kDummyDescriptorOffset + kDescriptorSize == 126);

    base[kExtensionCountOffset] = static_cast<std::uint8_t>(extensionCount);
    WriteChecksum(base);
    WriteCtaExtension(edid.data() + kEdidBlockSize);
    if (displayIdTiming) {
        WriteDisplayIdExtension(edid.data() + 2 * kEdidBlockSize, *displayIdTiming);
    }
    return edid;
}

GUID ContainerIdFor(const std::string& deviceId) {
    const std::uint64_t high = Fnv1a64(deviceId);
    const std::uint64_t low = Fnv1a64(deviceId, high ^ 0x9E3779B97F4A7C15ull);
    GUID guid{};
    guid.Data1 = static_cast<unsigned long>(high >> 32);
    guid.Data2 = static_cast<unsigned short>(high >> 16);
    guid.Data3 = static_cast<unsigned short>((high & 0x0FFF) | 0x4000);
    for (int i = 0; i < 8; ++i) {
        guid.Data4[i] = static_cast<unsigned char>(low >> (8 * i));
    }
    guid.Data4[0] = static_cast<unsigned char>((guid.Data4[0] & 0x3F) | 0x80);
    return guid;
}

}  // namespace aum
