#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace aum {

// --name value pairs after the subcommand.
class Options {
public:
    Options(int argc, wchar_t** argv, int first) {
        for (int index = first; index < argc; ++index) {
            const std::wstring argument = argv[index];
            if (argument.rfind(L"--", 0) != 0 || index + 1 >= argc) {
                throw std::runtime_error("expected --name value pairs");
            }
            values_[argument.substr(2)] = argv[++index];
        }
    }

    std::wstring Text(const wchar_t* name, const std::wstring& fallback = L"") const {
        const auto found = values_.find(name);
        return found == values_.end() ? fallback : found->second;
    }

    double Number(const wchar_t* name, double fallback) const {
        const auto found = values_.find(name);
        return found == values_.end() ? fallback : std::stod(found->second);
    }

private:
    std::map<std::wstring, std::wstring> values_;
};

}  // namespace aum
