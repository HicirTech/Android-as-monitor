#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace aum {

class Adb {
public:
    Adb(std::wstring executable, std::string serial);

    static std::wstring Locate(const std::wstring& override);

    std::string Run(const std::string& arguments, unsigned long timeoutMs = 20000) const;
    std::string RunOrEmpty(const std::string& arguments) const;

    // Runs a command that keeps printing, handing each piece of its output to onOutput, until stop is signalled or it ends.
    void Stream(const std::string& arguments, HANDLE stop, const std::function<void(std::string_view)>& onOutput) const;

private:
    struct Started {
        HANDLE process;
        HANDLE output;
    };
    Started Start(const std::string& arguments) const;

    std::wstring executable_;
    std::string serial_;
};

}  // namespace aum
