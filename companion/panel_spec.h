#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace aum {

// What the phone says about its panel, in the orientation it will be presented.
struct PanelSpec {
    std::string manufacturer;
    std::string model;
    std::string deviceId;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    double refreshHz = 0;
    double widthMm = 0;
    double heightMm = 0;
    bool hevcCoversNative = false;
    bool avcCoversNative = false;
    std::string hevcDecoder;
    std::string avcDecoder;
    std::uint32_t audioSampleRate = 48000;
    std::map<std::string, std::string> raw;
};

PanelSpec ParsePanelSpec(const std::string& hello);

}  // namespace aum
