#include "phone_layer.h"

#include <cmath>
#include <sstream>
#include <vector>

namespace aum {

static constexpr const char* kVideoLayerName = "SurfaceView[dev.androidusbmo";
static constexpr const char* kBlastLayerMark = "(BLAST)";
static constexpr double kSizeTolerance = 0.01;

static std::vector<std::string> SplitColumns(const std::string& line) {
    std::vector<std::string> columns;
    std::istringstream stream(line);
    std::string column;
    while (std::getline(stream, column, '|')) {
        columns.push_back(column);
    }
    return columns;
}

static std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

static bool ReadRectSize(const std::string& text, double& width, double& height) {
    std::istringstream stream(text);
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    if (!(stream >> left >> top >> right >> bottom)) {
        return false;
    }
    width = right - left;
    height = bottom - top;
    return true;
}

bool PhoneVideoLayer::MapsPixelsOneToOne() const {
    const auto same = [](double a, double b) { return std::abs(a - b) < kSizeTolerance; };
    return (same(displayWidth, sourceWidth) && same(displayHeight, sourceHeight)) || (same(displayWidth, sourceHeight) && same(displayHeight, sourceWidth));
}

namespace {

// Android versions and vendors add columns to this table, so they are found by name.
struct TableColumns {
    std::size_t composition = 0;
    std::size_t transform = 0;
    std::size_t displayFrame = 0;
    std::size_t sourceCrop = 0;
};

std::optional<TableColumns> ReadTableHeader(const std::string& line) {
    if (line.find('|') == std::string::npos) {
        return std::nullopt;
    }
    const std::vector<std::string> names = SplitColumns(line);
    TableColumns columns;
    int found = 0;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const std::string name = Trim(names[index]);
        if (name == "Comp Type") {
            columns.composition = index;
            ++found;
        } else if (name == "Transform") {
            columns.transform = index;
            ++found;
        } else if (name == "Disp Frame (LTRB)") {
            columns.displayFrame = index;
            ++found;
        } else if (name == "Source Crop (LTRB)") {
            columns.sourceCrop = index;
            ++found;
        }
    }
    return found == 4 ? std::optional(columns) : std::nullopt;
}

bool NamesTheVideoLayer(const std::string& line) {
    return line.find(kVideoLayerName) != std::string::npos && line.find(kBlastLayerMark) != std::string::npos;
}

}  // namespace

std::optional<PhoneVideoLayer> FindVideoLayer(const std::string& dumpsysSurfaceFlinger) {
    std::optional<TableColumns> columns;
    std::istringstream lines(dumpsysSurfaceFlinger);
    std::string line;
    while (std::getline(lines, line)) {
        if (const auto header = ReadTableHeader(line)) {
            columns = header;
            continue;
        }
        // The table prints the name on its own line, without a column separator, and the values on the next one.
        if (!columns || !NamesTheVideoLayer(line) || line.find('|') != std::string::npos) {
            continue;
        }
        std::string row;
        if (!std::getline(lines, row)) {
            return std::nullopt;
        }
        const std::vector<std::string> values = SplitColumns(row);
        if (values.size() <= std::max({columns->composition, columns->transform, columns->displayFrame, columns->sourceCrop})) {
            continue;
        }
        PhoneVideoLayer layer;
        layer.composition = Trim(values[columns->composition]);
        layer.transform = Trim(values[columns->transform]);
        if (ReadRectSize(values[columns->displayFrame], layer.displayWidth, layer.displayHeight) &&
            ReadRectSize(values[columns->sourceCrop], layer.sourceWidth, layer.sourceHeight)) {
            return layer;
        }
    }
    return std::nullopt;
}

std::string FindVideoLayerName(const std::string& dumpsysList) {
    static constexpr std::string_view kWrapperStart = "RequestedLayerState{";
    static constexpr std::string_view kWrapperEnd = " parentId=";

    std::string name;
    std::istringstream lines(dumpsysList);
    std::string line;
    while (std::getline(lines, line)) {
        if (!NamesTheVideoLayer(line)) {
            continue;
        }
        const auto wrapper = line.find(kWrapperStart);
        const std::size_t start = wrapper == std::string::npos ? 0 : wrapper + kWrapperStart.size();
        // The newest layer comes last. An older one can linger for a moment after the app restarts.
        name = Trim(line.substr(start, line.find(kWrapperEnd) == std::string::npos ? std::string::npos : line.find(kWrapperEnd) - start));
    }
    return name;
}

}  // namespace aum
