#pragma once

#include "panel_spec.h"

#include "../shared/edid_timing.h"

#include <guiddef.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aum {

// Android reports a float such as 120.00001, so the intended exact rate has to be recovered first.
Rational SnapRefreshRate(double reportedHz);

// Picks the blanking whose pixel clock, in the 10 kHz steps both timing formats allow, comes closest to the rate.
// Empty when every blanking takes the clock past maxClockUnits of those steps.
std::optional<DetailedTiming> ChooseTiming(std::uint32_t width, std::uint32_t height, Rational refresh, std::uint32_t widthMm, std::uint32_t heightMm, std::uint64_t maxClockUnits);

std::vector<std::uint8_t> BuildEdid(const PanelSpec& spec);

GUID ContainerIdFor(const std::string& deviceId);

}  // namespace aum
