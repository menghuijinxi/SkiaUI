#include "skui_internal.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>

namespace skui {

Theme Theme::dark() {
    return {};
}

SkColor rgb(unsigned r, unsigned g, unsigned b) {
    return SkColorSetRGB(static_cast<uint8_t>(std::min(r, 255u)),
                         static_cast<uint8_t>(std::min(g, 255u)),
                         static_cast<uint8_t>(std::min(b, 255u)));
}

SkColor rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return SkColorSetARGB(static_cast<uint8_t>(std::min(a, 255u)),
                          static_cast<uint8_t>(std::min(r, 255u)),
                          static_cast<uint8_t>(std::min(g, 255u)),
                          static_cast<uint8_t>(std::min(b, 255u)));
}

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

std::string trim(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::vector<std::string> splitWhitespace(std::string_view value) {
    std::vector<std::string> parts;
    std::istringstream stream{std::string(value)};
    std::string part;
    while (stream >> part) {
        parts.push_back(part);
    }
    return parts;
}

static bool parseHexByte(std::string_view text, unsigned& out) {
    unsigned value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

SkColor parseColor(std::string_view raw, SkColor fallback) {
    std::string value = trim(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (value.empty()) {
        return fallback;
    }
    if (value == "transparent") {
        return SK_ColorTRANSPARENT;
    }
    if (value == "white") {
        return SK_ColorWHITE;
    }
    if (value == "black") {
        return SK_ColorBLACK;
    }
    if (value.size() == 7 && value[0] == '#') {
        unsigned r = 0;
        unsigned g = 0;
        unsigned b = 0;
        if (parseHexByte(std::string_view(value).substr(1, 2), r) &&
            parseHexByte(std::string_view(value).substr(3, 2), g) &&
            parseHexByte(std::string_view(value).substr(5, 2), b)) {
            return rgb(r, g, b);
        }
    }
    if (value.rfind("rgba(", 0) == 0 && value.back() == ')') {
        std::string args = value.substr(5, value.size() - 6);
        std::replace(args.begin(), args.end(), ',', ' ');
        std::istringstream stream(args);
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
        if (stream >> r >> g >> b >> a) {
            return rgba(static_cast<unsigned>(clampf(r, 0.0f, 255.0f)),
                        static_cast<unsigned>(clampf(g, 0.0f, 255.0f)),
                        static_cast<unsigned>(clampf(b, 0.0f, 255.0f)),
                        static_cast<unsigned>(clampf(a <= 1.0f ? a * 255.0f : a, 0.0f, 255.0f)));
        }
    }
    if (value.rfind("rgb(", 0) == 0 && value.back() == ')') {
        std::string args = value.substr(4, value.size() - 5);
        std::replace(args.begin(), args.end(), ',', ' ');
        std::istringstream stream(args);
        unsigned r = 0;
        unsigned g = 0;
        unsigned b = 0;
        if (stream >> r >> g >> b) {
            return rgb(r, g, b);
        }
    }
    return fallback;
}

YogaNode::YogaNode() : node_(YGNodeNew()) {}

YogaNode::~YogaNode() {
    if (node_) {
        YGNodeFree(node_);
    }
}

YGNodeRef YogaNode::get() const {
    return node_;
}

}  // namespace skui
