#pragma once

#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace aum {

inline void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char message[256];
        sprintf_s(message, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

inline std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

inline std::wstring Widen(const std::string& narrow) {
    if (narrow.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), static_cast<int>(narrow.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), static_cast<int>(narrow.size()), wide.data(), size);
    return wide;
}

}  // namespace aum
