#pragma once

#include "panel_spec.h"

#include "../shared/edid_timing.h"

#include <guiddef.h>

#include <cstdint>
#include <string>
#include <vector>

namespace aum {

// Android reports a float such as 120.00001, so the intended exact rate has to be recovered first.
Rational SnapRefreshRate(double reportedHz);

// Picks the blanking whose pixel clock, in the 10 kHz steps a detailed timing allows, comes closest to the rate.
DetailedTiming ChooseTiming(std::uint32_t width, std::uint32_t height, Rational refresh, std::uint32_t widthMm, std::uint32_t heightMm);

std::vector<std::uint8_t> BuildEdid(const PanelSpec& spec);

GUID ContainerIdFor(const std::string& deviceId);

}  // namespace aum
