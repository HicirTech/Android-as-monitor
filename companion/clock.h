#pragma once

#include <windows.h>

#include <cstdint>

namespace aum {

inline std::int64_t QpcFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

inline std::int64_t QpcToNs(std::int64_t qpc) {
    const std::int64_t frequency = QpcFrequency();
    return (qpc / frequency) * 1'000'000'000 + (qpc % frequency) * 1'000'000'000 / frequency;
}

inline std::int64_t NowNs() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return QpcToNs(value.QuadPart);
}

}  // namespace aum
