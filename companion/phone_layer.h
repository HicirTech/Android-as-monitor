#pragma once

#include <optional>
#include <string>

namespace aum {

// One row of the composition table in `dumpsys SurfaceFlinger`: what the phone's compositor does with the video.
struct PhoneVideoLayer {
    std::string composition;
    std::string transform;
    double displayWidth = 0;
    double displayHeight = 0;
    double sourceWidth = 0;
    double sourceHeight = 0;

    // True when one decoded pixel covers exactly one panel pixel. A quarter turn swaps the axes of the display frame.
    bool MapsPixelsOneToOne() const;
};

std::optional<PhoneVideoLayer> FindVideoLayer(const std::string& dumpsysSurfaceFlinger);

// The video layer's full name in `dumpsys SurfaceFlinger --list`, or empty while it does not exist.
std::string FindVideoLayerName(const std::string& dumpsysList);

}  // namespace aum
