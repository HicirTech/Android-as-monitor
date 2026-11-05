#pragma once

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>

namespace aum {

inline constexpr std::size_t kEdidBlockSize = 128;
inline constexpr std::size_t kFirstDetailedTimingOffset = 54;
inline constexpr std::uint64_t kDetailedTimingClockUnitHz = 10'000;

struct DetailedTiming {
    std::uint32_t hActive;
    std::uint32_t vActive;
    std::uint32_t hBlank;
    std::uint32_t vBlank;
    std::uint64_t pixelClockHz;
    std::uint32_t widthMm;
    std::uint32_t heightMm;

    std::uint64_t hTotal() const { return std::uint64_t{hActive} + hBlank; }
    std::uint64_t vTotal() const { return std::uint64_t{vActive} + vBlank; }
};

struct Rational {
    std::uint64_t numerator;
    std::uint64_t denominator;

    double value() const { return static_cast<double>(numerator) / static_cast<double>(denominator); }
};

inline Rational Reduce(std::uint64_t numerator, std::uint64_t denominator) {
    const std::uint64_t divisor = std::gcd(numerator, denominator);
    return {numerator / divisor, denominator / divisor};
}

inline Rational RefreshRate(const DetailedTiming& timing) {
    return Reduce(timing.pixelClockHz, timing.hTotal() * timing.vTotal());
}

inline bool HasValidChecksum(const std::uint8_t* block) {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < kEdidBlockSize; ++i) {
        sum = static_cast<std::uint8_t>(sum + block[i]);
    }
    return sum == 0;
}

inline std::optional<DetailedTiming> ParseFirstDetailedTiming(const std::uint8_t* edid, std::size_t size) {
    static constexpr std::uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (size < kEdidBlockSize || !HasValidChecksum(edid)) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < sizeof(header); ++i) {
        if (edid[i] != header[i]) {
            return std::nullopt;
        }
    }

    const std::uint8_t* d = edid + kFirstDetailedTimingOffset;
    const std::uint32_t clockUnits = d[0] | (std::uint32_t{d[1]} << 8);
    if (clockUnits == 0) {
        return std::nullopt;
    }

    DetailedTiming timing{};
    timing.pixelClockHz = clockUnits * kDetailedTimingClockUnitHz;
    timing.hActive = d[2] | (std::uint32_t{d[4] & 0xF0u} << 4);
    timing.hBlank = d[3] | (std::uint32_t{d[4] & 0x0Fu} << 8);
    timing.vActive = d[5] | (std::uint32_t{d[7] & 0xF0u} << 4);
    timing.vBlank = d[6] | (std::uint32_t{d[7] & 0x0Fu} << 8);
    timing.widthMm = d[12] | (std::uint32_t{d[14] & 0xF0u} << 4);
    timing.heightMm = d[13] | (std::uint32_t{d[14] & 0x0Fu} << 8);
    return timing;
}

}  // namespace aum
