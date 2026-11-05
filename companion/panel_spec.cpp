#include "panel_spec.h"

#include <sstream>
#include <stdexcept>

namespace aum {

static const std::string& Required(const std::map<std::string, std::string>& values, const char* key) {
    const auto found = values.find(key);
    if (found == values.end()) {
        throw std::runtime_error(std::string("phone hello is missing ") + key);
    }
    return found->second;
}

static std::string Optional(const std::map<std::string, std::string>& values, const char* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::string{} : found->second;
}

PanelSpec ParsePanelSpec(const std::string& hello) {
    PanelSpec spec;
    std::istringstream lines(hello);
    std::string line;
    while (std::getline(lines, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos) {
            spec.raw[line.substr(0, separator)] = line.substr(separator + 1);
        }
    }

    spec.manufacturer = Required(spec.raw, "manufacturer");
    spec.model = Required(spec.raw, "model");
    spec.deviceId = Required(spec.raw, "deviceId");
    spec.width = static_cast<std::uint32_t>(std::stoul(Required(spec.raw, "panelWidth")));
    spec.height = static_cast<std::uint32_t>(std::stoul(Required(spec.raw, "panelHeight")));
    spec.surfaceWidth = static_cast<std::uint32_t>(std::stoul(Required(spec.raw, "surfaceWidth")));
    spec.surfaceHeight = static_cast<std::uint32_t>(std::stoul(Required(spec.raw, "surfaceHeight")));
    spec.refreshHz = std::stod(Required(spec.raw, "refreshHz"));
    spec.widthMm = std::stod(Required(spec.raw, "widthMm"));
    spec.heightMm = std::stod(Required(spec.raw, "heightMm"));
    spec.hevcDecoder = Optional(spec.raw, "hevcDecoder");
    spec.avcDecoder = Optional(spec.raw, "avcDecoder");
    spec.hevcCoversNative = Optional(spec.raw, "hevcCoversNative") == "true";
    spec.avcCoversNative = Optional(spec.raw, "avcCoversNative") == "true";
    spec.audioSampleRate = static_cast<std::uint32_t>(std::stoul(Required(spec.raw, "audioSampleRate")));
    return spec;
}

}  // namespace aum
