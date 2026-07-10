#include "skui_internal.h"

#include "include/ports/SkTypeface_win.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>

namespace skui {

namespace {

bool isTextareaScrollableNode(const Node& node) {
    return node.tag == "textarea";
}

struct UiFontResources {
    sk_sp<SkFontMgr> manager;
    sk_sp<SkTypeface> regular;
    sk_sp<SkTypeface> bold;
};

sk_sp<SkTypeface> pickUiTypeface(const sk_sp<SkFontMgr>& manager, bool bold) {
    if (!manager) {
        return nullptr;
    }

    const SkFontStyle style =
        bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
    constexpr std::array<const char*, 5> kFamilies = {
        "Microsoft YaHei UI",
        "Microsoft YaHei",
        "Segoe UI",
        "Arial",
        nullptr,
    };
    for (const char* family : kFamilies) {
        sk_sp<SkTypeface> typeface =
            manager->matchFamilyStyle(family, style);
        if (typeface) {
            return typeface;
        }
    }
    return nullptr;
}

UiFontResources createUiFontResources() {
    UiFontResources resources;
    resources.manager = SkFontMgr_New_DirectWrite();
    if (!resources.manager) {
        resources.manager = SkFontMgr_New_GDI();
    }
    resources.regular = pickUiTypeface(resources.manager, false);
    resources.bold = pickUiTypeface(resources.manager, true);
    return resources;
}

const UiFontResources& uiFontResources() {
    static const UiFontResources resources = createUiFontResources();
    return resources;
}

} // namespace

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

sk_sp<SkFontMgr> uiFontManager() {
    return uiFontResources().manager;
}

SkFont makeUiFont(float size, bool bold) {
    const UiFontResources& resources = uiFontResources();
    SkFont font(bold ? resources.bold : resources.regular, size);
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setSubpixel(true);
    return font;
}

float measureUiTextWidth(std::string_view value, float size, bool bold) {
    if (value.empty()) {
        return 0.0f;
    }

    const SkFont font = makeUiFont(size, bold);
    return font.measureText(
        value.data(),
        value.size(),
        SkTextEncoding::kUTF8);
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

float scrollViewportWidth(const Node& node) {
    float width = node.layout.w;
    if (node.style.scrollbarGutterStable &&
        (node.style.overflowY == Overflow::Auto || node.style.overflowY == Overflow::Scroll)) {
        width -= kSkuiScrollbarGutter;
    }
    return std::max(0.0f, width);
}

float scrollViewportHeight(const Node& node) {
    float height = node.layout.h;
    const bool needsHorizontalGutter = node.style.overflowX == Overflow::Scroll ||
        (node.style.overflowX == Overflow::Auto && node.scrollContentWidth > scrollViewportWidth(node));
    if (node.style.scrollbarGutterStable && needsHorizontalGutter) {
        height -= kSkuiScrollbarGutter;
    }
    return std::max(0.0f, height);
}

float scrollMaxX(const Node& node) {
    return std::max(0.0f, node.scrollContentWidth - scrollViewportWidth(node));
}

float scrollMaxY(const Node& node) {
    return std::max(0.0f, node.scrollContentHeight - scrollViewportHeight(node));
}

bool shouldShowScrollbarX(const Node& node) {
    return node.style.overflowX == Overflow::Scroll ||
           ((node.style.overflowX == Overflow::Auto || isTextareaScrollableNode(node)) &&
            scrollMaxX(node) > 0.0f);
}

bool shouldShowScrollbarY(const Node& node) {
    return node.style.overflowY == Overflow::Scroll ||
           ((node.style.overflowY == Overflow::Auto || isTextareaScrollableNode(node)) &&
            scrollMaxY(node) > 0.0f);
}

Rect scrollContentClipRect(const Node& node) {
    Rect clip = node.layout;
    if (node.style.scrollbarGutterStable) {
        if (node.style.overflowY == Overflow::Scroll ||
            node.style.overflowY == Overflow::Auto) {
            clip.w = std::max(0.0f, clip.w - kSkuiScrollbarGutter);
        }
        if (shouldShowScrollbarX(node)) {
            clip.h = std::max(0.0f, clip.h - kSkuiScrollbarGutter);
        }
    }
    return clip;
}

float stickyVisualOffsetY(const Node& node) {
    if (node.style.position != Position::Sticky) {
        return 0.0f;
    }

    for (const Node* ancestor = node.parent; ancestor; ancestor = ancestor->parent) {
        const bool scrollsY = ancestor->style.overflowY == Overflow::Auto ||
            ancestor->style.overflowY == Overflow::Scroll;
        if (scrollsY && ancestor->scrollY > 0.0f) {
            return ancestor->scrollY;
        }
    }
    return 0.0f;
}

std::filesystem::path pathFromUtf8(std::string_view text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::filesystem::path(std::string(text));
    }

    const int sourceSize = static_cast<int>(text.size());
    const int wideSize = MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             text.data(),
                                             sourceSize,
                                             nullptr,
                                             0);
    if (wideSize <= 0) {
        return std::filesystem::path(std::string(text));
    }

    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceSize, wide.data(), wideSize);
    return std::filesystem::path(wide);
#else
#ifdef __cpp_char8_t
    std::u8string value;
    value.reserve(text.size());
    for (char ch : text) {
        value.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(value);
#else
    return std::filesystem::path(std::string(text));
#endif
#endif
}

std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return path.string();
    }

    const int sourceSize = static_cast<int>(wide.size());
    const int utf8Size = WideCharToMultiByte(CP_UTF8,
                                             0,
                                             wide.data(),
                                             sourceSize,
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (utf8Size <= 0) {
        return path.string();
    }

    std::string utf8(static_cast<size_t>(utf8Size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        sourceSize,
                        utf8.data(),
                        utf8Size,
                        nullptr,
                        nullptr);
    return utf8;
#else
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
#endif
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
        YGNodeFreeRecursive(node_);
    }
}

YGNodeRef YogaNode::get() const {
    return node_;
}

}  // namespace skui
