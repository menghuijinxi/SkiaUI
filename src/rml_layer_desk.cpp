#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include "d3d_presenter.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkVertices.h"
#include "include/effects/SkGradient.h"
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
#include "include/gpu/ganesh/GrRecordingContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#endif
#include "include/ports/SkTypeface_win.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr int kBaseWidth = 1672;
constexpr int kBaseHeight = 941;
constexpr int kRmlSupersample = 2;
constexpr float kDefaultDpi = 96.0f;
constexpr float kPi = 3.14159265358979323846f;
constexpr COLORREF kWin32Background = RGB(7, 12, 18);
float gRmlTextureScale = 1.0f;

struct BrowserLinearGradient {
    float angleDeg = 180.0f;
    std::vector<SkColor> colors;
    std::vector<SkScalar> positions;
};

struct BrowserBoxShadow {
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float blur = 0.0f;
    float spread = 0.0f;
    SkColor color = SK_ColorTRANSPARENT;
    bool inset = false;
};

struct BrowserPaintStyle {
    bool hasBackgroundGradient = false;
    BrowserLinearGradient backgroundGradient;
    bool hasBoxShadow = false;
    BrowserBoxShadow boxShadow;
};

using BrowserPaintStyleMap = std::map<std::string, BrowserPaintStyle>;

struct RmlBenchCounters {
    int generatedStrings = 0;
    int compiledGeometries = 0;
    int renderedGeometries = 0;
    int generatedTextures = 0;
    uint64_t generatedTextureBytes = 0;
    uint64_t renderedVertices = 0;
    uint64_t renderedIndices = 0;
};

struct RmlDrawTiming {
    double prepareMs = 0.0;
    double chromeMs = 0.0;
    double chromeBackgroundMs = 0.0;
    double chromeSidebarMs = 0.0;
    double chromePanelMs = 0.0;
    double chromeCompassMs = 0.0;
    double chromeStatusMs = 0.0;
    double chromeFrameMs = 0.0;
    double rmlMs = 0.0;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    SkRect sk() const {
        return SkRect::MakeXYWH(x, y, w, h);
    }
};

SkColor rgb(uint8_t r, uint8_t g, uint8_t b) {
    return SkColorSetRGB(r, g, b);
}

SkColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return SkColorSetARGB(a, r, g, b);
}

uint8_t clampByte(float value) {
    return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, value)));
}

std::string_view trimView(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

std::string trimCopy(std::string_view value) {
    const std::string_view trimmed = trimView(value);
    return std::string(trimmed.data(), trimmed.size());
}

std::string lowerCopy(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size(), suffix.size()) == suffix;
}

bool parseFloatStrict(std::string_view value, float& result) {
    const std::string text = trimCopy(value);
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) {
        return false;
    }
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    result = parsed;
    return true;
}

std::vector<std::string> splitTopLevel(std::string_view value, char delimiter) {
    std::vector<std::string> parts;
    int functionDepth = 0;
    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '(') {
            ++functionDepth;
        } else if (value[i] == ')' && functionDepth > 0) {
            --functionDepth;
        } else if (value[i] == delimiter && functionDepth == 0) {
            std::string part = trimCopy(value.substr(start, i - start));
            if (!part.empty()) {
                parts.push_back(std::move(part));
            }
            start = i + 1;
        }
    }

    std::string part = trimCopy(value.substr(start));
    if (!part.empty()) {
        parts.push_back(std::move(part));
    }
    return parts;
}

std::vector<std::string> splitWhitespace(std::string_view value) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < value.size()) {
        while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        const size_t start = i;
        while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) {
            ++i;
        }
        if (i > start) {
            parts.emplace_back(value.substr(start, i - start));
        }
    }
    return parts;
}

size_t findTopLevelColon(std::string_view value) {
    int functionDepth = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '(') {
            ++functionDepth;
        } else if (value[i] == ')' && functionDepth > 0) {
            --functionDepth;
        } else if (value[i] == ':' && functionDepth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

int cssHexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool parseCssHexByte(std::string_view value, size_t offset, uint8_t& byte) {
    if (offset + 1 >= value.size()) {
        return false;
    }
    const int high = cssHexNibble(value[offset]);
    const int low = cssHexNibble(value[offset + 1]);
    if (high < 0 || low < 0) {
        return false;
    }
    byte = static_cast<uint8_t>((high << 4) | low);
    return true;
}

bool parseBrowserColor(std::string_view value, SkColor& color) {
    const std::string text = trimCopy(value);
    if (text == "transparent") {
        color = SK_ColorTRANSPARENT;
        return true;
    }
    if (text.empty() || text[0] != '#') {
        return false;
    }

    if (text.size() == 4 || text.size() == 5) {
        const int r = cssHexNibble(text[1]);
        const int g = cssHexNibble(text[2]);
        const int b = cssHexNibble(text[3]);
        const int a = text.size() == 5 ? cssHexNibble(text[4]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) {
            return false;
        }
        color = SkColorSetARGB(static_cast<uint8_t>(a * 17),
                               static_cast<uint8_t>(r * 17),
                               static_cast<uint8_t>(g * 17),
                               static_cast<uint8_t>(b * 17));
        return true;
    }

    if (text.size() != 7 && text.size() != 9) {
        return false;
    }
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
    if (!parseCssHexByte(text, 1, r) || !parseCssHexByte(text, 3, g) || !parseCssHexByte(text, 5, b)) {
        return false;
    }
    if (text.size() == 9 && !parseCssHexByte(text, 7, a)) {
        return false;
    }
    color = SkColorSetARGB(a, r, g, b);
    return true;
}

bool parseCssLength(std::string_view value, float& length) {
    std::string text = trimCopy(value);
    const std::string lower = lowerCopy(text);
    if (endsWith(lower, "px")) {
        text.resize(text.size() - 2);
    }
    return parseFloatStrict(text, length);
}

bool parseCssPercent(std::string_view value, float& percent) {
    std::string text = trimCopy(value);
    if (!endsWith(lowerCopy(text), "%")) {
        return false;
    }
    text.resize(text.size() - 1);
    float parsed = 0.0f;
    if (!parseFloatStrict(text, parsed)) {
        return false;
    }
    percent = std::clamp(parsed / 100.0f, 0.0f, 1.0f);
    return true;
}

bool parseCssAngle(std::string_view value, float& angleDeg) {
    std::string text = trimCopy(value);
    const std::string lower = lowerCopy(text);
    if (endsWith(lower, "deg")) {
        text.resize(text.size() - 3);
        return parseFloatStrict(text, angleDeg);
    }

    const std::vector<std::string> words = splitWhitespace(lower);
    if (words.empty() || words.front() != "to") {
        return false;
    }

    const bool top = std::find(words.begin(), words.end(), "top") != words.end();
    const bool right = std::find(words.begin(), words.end(), "right") != words.end();
    const bool bottom = std::find(words.begin(), words.end(), "bottom") != words.end();
    const bool left = std::find(words.begin(), words.end(), "left") != words.end();
    if (top && right) {
        angleDeg = 45.0f;
    } else if (bottom && right) {
        angleDeg = 135.0f;
    } else if (bottom && left) {
        angleDeg = 225.0f;
    } else if (top && left) {
        angleDeg = 315.0f;
    } else if (top) {
        angleDeg = 0.0f;
    } else if (right) {
        angleDeg = 90.0f;
    } else if (bottom) {
        angleDeg = 180.0f;
    } else if (left) {
        angleDeg = 270.0f;
    } else {
        return false;
    }
    return true;
}

bool parseColorStop(std::string_view value, SkColor& color, SkScalar& position, bool& hasPosition) {
    std::string text = trimCopy(value);
    if (parseBrowserColor(text, color)) {
        position = 0.0f;
        hasPosition = false;
        return true;
    }

    const size_t colorEnd = text.find_first_of(" \t\r\n");
    if (colorEnd == std::string::npos) {
        return false;
    }
    if (!parseBrowserColor(text.substr(0, colorEnd), color)) {
        return false;
    }

    position = 0.0f;
    hasPosition = false;
    const std::string rest = trimCopy(text.substr(colorEnd + 1));
    if (!rest.empty() && parseCssPercent(rest, position)) {
        hasPosition = true;
    }
    return true;
}

bool parseBrowserLinearGradient(std::string_view value, BrowserLinearGradient& gradient) {
    const std::string text = trimCopy(value);
    const std::string lower = lowerCopy(text);
    constexpr std::string_view prefix = "linear-gradient(";
    if (!lower.starts_with(prefix) || text.size() <= prefix.size() || text.back() != ')') {
        return false;
    }

    const std::string_view inner(text.data() + prefix.size(), text.size() - prefix.size() - 1);
    const std::vector<std::string> parts = splitTopLevel(inner, ',');
    if (parts.size() < 2) {
        return false;
    }

    BrowserLinearGradient parsed;
    size_t colorStart = 0;
    if (parseCssAngle(parts[0], parsed.angleDeg)) {
        colorStart = 1;
    } else {
        parsed.angleDeg = 180.0f;
    }
    if (parts.size() - colorStart < 2) {
        return false;
    }

    std::vector<SkScalar> positions;
    std::vector<bool> explicitPositions;
    for (size_t i = colorStart; i < parts.size(); ++i) {
        SkColor color = SK_ColorTRANSPARENT;
        SkScalar position = 0.0f;
        bool hasPosition = false;
        if (!parseColorStop(parts[i], color, position, hasPosition)) {
            return false;
        }
        parsed.colors.push_back(color);
        positions.push_back(position);
        explicitPositions.push_back(hasPosition);
    }

    const size_t stopCount = parsed.colors.size();
    parsed.positions.resize(stopCount);
    for (size_t i = 0; i < stopCount; ++i) {
        parsed.positions[i] = explicitPositions[i]
                                  ? positions[i]
                                  : (stopCount > 1 ? static_cast<SkScalar>(i) / static_cast<SkScalar>(stopCount - 1) : 0.0f);
    }

    gradient = std::move(parsed);
    return true;
}

bool parseBrowserBoxShadow(std::string_view value, BrowserBoxShadow& shadow) {
    const std::vector<std::string> shadowLayers = splitTopLevel(value, ',');
    if (shadowLayers.empty() || lowerCopy(shadowLayers.front()) == "none") {
        return false;
    }

    BrowserBoxShadow parsed;
    std::vector<float> lengths;
    bool hasColor = false;
    for (const std::string& token : splitWhitespace(shadowLayers.front())) {
        if (lowerCopy(token) == "inset") {
            parsed.inset = true;
            continue;
        }

        SkColor color = SK_ColorTRANSPARENT;
        if (parseBrowserColor(token, color)) {
            parsed.color = color;
            hasColor = true;
            continue;
        }

        float length = 0.0f;
        if (parseCssLength(token, length)) {
            lengths.push_back(length);
            continue;
        }
        return false;
    }

    if (lengths.size() < 2 || lengths.size() > 4) {
        return false;
    }
    parsed.offsetX = lengths[0];
    parsed.offsetY = lengths[1];
    parsed.blur = lengths.size() >= 3 ? std::max(0.0f, lengths[2]) : 0.0f;
    parsed.spread = lengths.size() >= 4 ? lengths[3] : 0.0f;
    if (!hasColor) {
        parsed.color = SK_ColorBLACK;
    }

    shadow = parsed;
    return true;
}

std::string extractStyleText(std::string_view rml) {
    std::string css;
    size_t search = 0;
    while (true) {
        const size_t styleStart = rml.find("<style", search);
        if (styleStart == std::string_view::npos) {
            break;
        }
        const size_t contentStart = rml.find('>', styleStart);
        if (contentStart == std::string_view::npos) {
            break;
        }
        const size_t styleEnd = rml.find("</style>", contentStart + 1);
        if (styleEnd == std::string_view::npos) {
            break;
        }
        css.append(rml.substr(contentStart + 1, styleEnd - contentStart - 1));
        css.push_back('\n');
        search = styleEnd + 8;
    }
    return css;
}

std::string removeCssComments(std::string_view css) {
    std::string result;
    result.reserve(css.size());
    for (size_t i = 0; i < css.size();) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            const size_t end = css.find("*/", i + 2);
            if (end == std::string_view::npos) {
                break;
            }
            i = end + 2;
            continue;
        }
        result.push_back(css[i]);
        ++i;
    }
    return result;
}

std::vector<std::string> parseIdSelectors(std::string_view selectorText) {
    std::vector<std::string> ids;
    for (const std::string& selector : splitTopLevel(selectorText, ',')) {
        const std::string trimmed = trimCopy(selector);
        if (trimmed.empty() || trimmed.front() != '#') {
            continue;
        }
        size_t end = 1;
        while (end < trimmed.size()) {
            const unsigned char c = static_cast<unsigned char>(trimmed[end]);
            if (!std::isalnum(c) && trimmed[end] != '_' && trimmed[end] != '-') {
                break;
            }
            ++end;
        }
        if (end > 1) {
            ids.emplace_back(trimmed.substr(1, end - 1));
        }
    }
    return ids;
}

void parseBrowserDeclarations(std::string_view block, BrowserPaintStyle& style) {
    for (const std::string& declaration : splitTopLevel(block, ';')) {
        const size_t colon = findTopLevelColon(declaration);
        if (colon == std::string_view::npos) {
            continue;
        }

        const std::string name = lowerCopy(trimView(std::string_view(declaration).substr(0, colon)));
        const std::string value = trimCopy(std::string_view(declaration).substr(colon + 1));
        if (name == "background" || name == "background-image") {
            BrowserLinearGradient gradient;
            if (parseBrowserLinearGradient(value, gradient)) {
                style.backgroundGradient = std::move(gradient);
                style.hasBackgroundGradient = true;
            }
        } else if (name == "box-shadow") {
            BrowserBoxShadow shadow;
            if (parseBrowserBoxShadow(value, shadow)) {
                style.boxShadow = shadow;
                style.hasBoxShadow = true;
            }
        }
    }
}

BrowserPaintStyleMap parseBrowserPaintStyles(std::string_view rml) {
    const std::string css = removeCssComments(extractStyleText(rml));
    BrowserPaintStyleMap styles;
    size_t search = 0;
    while (true) {
        const size_t blockStart = css.find('{', search);
        if (blockStart == std::string::npos) {
            break;
        }
        const size_t blockEnd = css.find('}', blockStart + 1);
        if (blockEnd == std::string::npos) {
            break;
        }

        const size_t selectorStart = css.rfind('}', blockStart);
        const size_t selectorOffset = selectorStart == std::string::npos ? 0 : selectorStart + 1;
        const std::string selectorText = trimCopy(std::string_view(css).substr(selectorOffset, blockStart - selectorOffset));
        const std::vector<std::string> ids = parseIdSelectors(selectorText);
        if (!ids.empty()) {
            const std::string_view block(css.data() + blockStart + 1, blockEnd - blockStart - 1);
            for (const std::string& id : ids) {
                parseBrowserDeclarations(block, styles[id]);
            }
        }
        search = blockEnd + 1;
    }
    return styles;
}

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(0, count)), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), count);
    }
    return wide;
}

class RmlSystem final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_).count();
    }

    bool LogMessage(Rml::Log::Type, const Rml::String& message) override {
        std::wstring wide = utf8ToWide(message);
        wide += L"\n";
        OutputDebugStringW(wide.c_str());
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

class LayerIconElement final : public Rml::Element {
public:
    explicit LayerIconElement(const Rml::String& tag) : Rml::Element(tag) {}

    ~LayerIconElement() {
        texture_.Release();
    }

protected:
    void OnRender() override {
        Rml::RenderManager* renderManager = GetRenderManager();
        if (!renderManager) {
            return;
        }

        Rml::Vector2f size = GetBox().GetSize();
        if (size.x <= 0.0f || size.y <= 0.0f) {
            return;
        }

        const Rml::String name = GetAttribute<Rml::String>("name", Rml::String());
        const SkColor color = toSkColor(GetComputedValues().color(), GetComputedValues().opacity());
        const float textureScale = std::max(1.0f, gRmlTextureScale);
        const int textureWidth = std::max(1, static_cast<int>(std::ceil(size.x * textureScale)));
        const int textureHeight = std::max(1, static_cast<int>(std::ceil(size.y * textureScale)));

        if (!ensureTexture(*renderManager, name, color, textureWidth, textureHeight)) {
            return;
        }

        Rml::Mesh mesh;
        Rml::MeshUtilities::GenerateQuad(mesh,
                                         GetAbsoluteOffset(Rml::BoxArea::Content),
                                         size,
                                         Rml::ColourbPremultiplied(255, 255, 255, 255));
        Rml::Geometry geometry = renderManager->MakeGeometry(std::move(mesh));
        geometry.Render(Rml::Vector2f(0.0f, 0.0f), static_cast<Rml::Texture>(texture_));
    }

private:
    Rml::CallbackTexture texture_;
    std::string textureKey_;

    bool ensureTexture(Rml::RenderManager& renderManager,
                       const Rml::String& name,
                       SkColor color,
                       int textureWidth,
                       int textureHeight) {
        std::string svg = svgForIcon(name, color);
        if (svg.empty()) {
            return false;
        }

        std::string key = name;
        key += ':';
        key += std::to_string(textureWidth);
        key += 'x';
        key += std::to_string(textureHeight);
        key += ':';
        key += std::to_string(color);
        if (key == textureKey_) {
            return true;
        }

        texture_.Release();
        textureKey_ = key;
        texture_ = renderManager.MakeCallbackTexture(
            [svg = std::move(svg), textureWidth, textureHeight](const Rml::CallbackTextureInterface& textureInterface) {
                std::vector<uint8_t> rgba(static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 4u, 0u);
                const SkImageInfo info = SkImageInfo::Make(textureWidth,
                                                           textureHeight,
                                                           kRGBA_8888_SkColorType,
                                                           kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, rgba.data(), static_cast<size_t>(textureWidth) * 4u);
                if (!surface) {
                    return false;
                }

                SkMemoryStream stream(svg.data(), svg.size(), false);
                sk_sp<SkSVGDOM> dom = SkSVGDOM::MakeFromStream(stream);
                if (!dom) {
                    return false;
                }

                SkCanvas* canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);
                dom->setContainerSize(SkSize::Make(24.0f, 24.0f));
                const float iconSize = static_cast<float>(std::min(textureWidth, textureHeight));
                canvas->translate((static_cast<float>(textureWidth) - iconSize) * 0.5f,
                                  (static_cast<float>(textureHeight) - iconSize) * 0.5f);
                canvas->scale(iconSize / 24.0f, iconSize / 24.0f);
                dom->render(canvas);
                return textureInterface.GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()),
                                                        {textureWidth, textureHeight});
            });
        return true;
    }

    static SkColor toSkColor(Rml::Colourb color, float opacity) {
        const float alpha = static_cast<float>(color.alpha) * std::clamp(opacity, 0.0f, 1.0f);
        return SkColorSetARGB(clampByte(alpha),
                              color.red,
                              color.green,
                              color.blue);
    }

    static SkColor withAlpha(SkColor color, uint8_t alpha) {
        const int scaledAlpha = static_cast<int>(SkColorGetA(color)) * static_cast<int>(alpha) / 255;
        return SkColorSetARGB(static_cast<U8CPU>(scaledAlpha),
                              SkColorGetR(color),
                              SkColorGetG(color),
                              SkColorGetB(color));
    }

    static std::string svgHex(SkColor color) {
        char buf[16];
        std::snprintf(buf,
                      sizeof(buf),
                      "#%02X%02X%02X",
                      SkColorGetR(color),
                      SkColorGetG(color),
                      SkColorGetB(color));
        return buf;
    }

    static std::string svgOpacity(SkColor color) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<float>(SkColorGetA(color)) / 255.0f);
        return buf;
    }

    static std::string svgFloat(float value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }

    static std::string svgStrokeAttrs(SkColor color, float width) {
        return "stroke=\"" + svgHex(color) + "\" stroke-opacity=\"" + svgOpacity(color) +
               "\" stroke-width=\"" + svgFloat(width) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
    }

    static std::string svgFillAttrs(SkColor color) {
        return "fill=\"" + svgHex(color) + "\" fill-opacity=\"" + svgOpacity(color) + "\"";
    }

    static std::string svgWrap(std::string_view body, const std::string& attrs) {
        std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" ";
        svg += attrs;
        svg += ">";
        svg += body;
        svg += "</svg>";
        return svg;
    }

    static std::string svgForIcon(const Rml::String& name, SkColor color) {
        if (name == "menu") {
            return svgWrap("<path d=\"M4 6h16\"/><path d=\"M4 12h16\"/><path d=\"M4 18h16\"/>",
                           svgStrokeAttrs(color, 2.4f));
        }

        if (name == "layer" || name == "layer-active") {
            std::string body;
            body += "<path d=\"M12 3.2 4.2 7.2 12 11.2l7.8-4-7.8-4Z\" ";
            body += svgFillAttrs(withAlpha(color, 235));
            body += "/>";
            body += "<path d=\"M4.2 12 12 16l7.8-4\"/>"
                    "<path d=\"M4.2 16.6 12 20.6l7.8-4\"/>";
            return svgWrap(body, svgStrokeAttrs(color, 2.05f));
        }

        if (name == "search") {
            return svgWrap("<circle cx=\"10.5\" cy=\"10.5\" r=\"6.3\"/><path d=\"M15.4 15.4 21 21\"/>",
                           svgStrokeAttrs(color, 2.7f));
        }

        if (name == "upload") {
            return svgWrap("<path d=\"M12 16.5V5.4\"/>"
                           "<path d=\"M7.1 10.3 12 5.4l4.9 4.9\"/>"
                           "<path d=\"M5.2 15.6v3.2h13.6v-3.2\"/>",
                           svgStrokeAttrs(color, 2.15f));
        }

        if (name == "plus") {
            return svgWrap("<path d=\"M12 5v14\"/><path d=\"M5 12h14\"/>", svgStrokeAttrs(color, 1.9f));
        }

        if (name == "edit") {
            return svgWrap("<path d=\"M12 3.6H5.7c-1.2 0-2.1.9-2.1 2.1v12.6c0 1.2.9 2.1 2.1 2.1h12.6c1.2 0 2.1-.9 2.1-2.1V12\"/>"
                           "<path d=\"M16.8 3.9c.8-.8 2.1-.8 2.9 0s.8 2.1 0 2.9l-8.9 8.9-4 1 1-4 9-8.8Z\"/>"
                           "<path d=\"M15.5 5.2l3.3 3.3\"/>",
                           svgStrokeAttrs(color, 2.05f));
        }

        if (name == "pencil") {
            return svgWrap("<path d=\"M4.2 19.8 5 16.2 16.4 4.8l2.8 2.8L7.8 19l-3.6.8Z\"/>"
                           "<path d=\"M14.8 6.4 17.6 9.2\"/>",
                           svgStrokeAttrs(color, 2.0f));
        }

        if (name == "target") {
            std::string body = "<circle cx=\"12\" cy=\"12\" r=\"8.1\"/>"
                               "<circle cx=\"12\" cy=\"12\" r=\"3.8\"/>";
            body += "<circle cx=\"12\" cy=\"12\" r=\"1.25\" ";
            body += svgFillAttrs(color);
            body += " stroke=\"none\"/>";
            return svgWrap(body, svgStrokeAttrs(color, 1.85f));
        }

        if (name == "doc") {
            return svgWrap("<path d=\"M6.1 3.5h11.8v17H6.1Z\"/>"
                           "<path d=\"M9 7.2h6\"/>"
                           "<path d=\"M9 11.5h6\"/>"
                           "<path d=\"M9 15.8h6\"/>",
                           svgStrokeAttrs(color, 1.95f));
        }

        if (name == "switch") {
            return svgWrap("<path d=\"M4 7.2h15\"/>"
                           "<path d=\"M15.1 3.9 19 7.2l-3.9 3.3\"/>"
                           "<path d=\"M20 16.8H5\"/>"
                           "<path d=\"M8.9 13.5 5 16.8l3.9 3.3\"/>",
                           svgStrokeAttrs(color, 1.95f));
        }

        if (name == "gear") {
            return svgWrap("<path d=\"M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.09a2 2 0 0 1 1 1.74v.5a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.38a2 2 0 0 0-.73-2.73l-.15-.09a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2Z\"/>"
                           "<circle cx=\"12\" cy=\"12\" r=\"3\"/>",
                           svgStrokeAttrs(color, 2.0f));
        }

        if (name == "geom-poly") {
            return svgWrap("<path d=\"M12.2 2.9 20.1 8.8 17.2 21 5.7 20.2 3.7 9.1 12.2 2.9Z\"/>",
                           svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-line") {
            return svgWrap("<path d=\"M3.8 5.4 9.1 9.6 13.6 13.2 20.2 18.3\"/>"
                           "<circle cx=\"3.8\" cy=\"5.4\" r=\"1.35\"/>"
                           "<circle cx=\"20.2\" cy=\"18.3\" r=\"1.35\"/>",
                           svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-point") {
            return svgWrap("<circle cx=\"12\" cy=\"12\" r=\"6.2\"/>", svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-river") {
            return svgWrap("<path d=\"M3.8 9.4c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/>"
                           "<path d=\"M3.8 14.8c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/>",
                           svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-box") {
            return svgWrap("<path d=\"M5.1 5.1h13.8v13.8H5.1Z\"/>", svgStrokeAttrs(color, 2.1f));
        }

        if (name == "eye") {
            std::string body = "<path d=\"M3.2 12s3.3-5.2 8.8-5.2 8.8 5.2 8.8 5.2-3.3 5.2-8.8 5.2S3.2 12 3.2 12Z\"/>";
            body += "<circle cx=\"12\" cy=\"12\" r=\"2.25\" ";
            body += svgFillAttrs(color);
            body += " stroke=\"none\"/>";
            return svgWrap(body, svgStrokeAttrs(color, 1.9f));
        }

        if (name == "chart") {
            return svgWrap("<path d=\"M4.4 4.4h15.2v15.2H4.4Z\"/>"
                           "<path d=\"M8 15.7v-4.2M12 15.7V8.2M16 15.7v-6\"/>",
                           svgStrokeAttrs(color, 1.8f));
        }

        if (name == "drag") {
            std::string body;
            for (float y : {6.0f, 12.0f, 18.0f}) {
                body += "<circle cx=\"8\" cy=\"" + svgFloat(y) + "\" r=\"1.9\" ";
                body += svgFillAttrs(color);
                body += " stroke=\"none\"/>";
                body += "<circle cx=\"16\" cy=\"" + svgFloat(y) + "\" r=\"1.9\" ";
                body += svgFillAttrs(color);
                body += " stroke=\"none\"/>";
            }
            return svgWrap(body, "stroke=\"none\"");
        }

        if (name == "more") {
            std::string body;
            for (float y : {5.0f, 12.0f, 19.0f}) {
                body += "<circle cx=\"12\" cy=\"" + svgFloat(y) + "\" r=\"2\" ";
                body += svgFillAttrs(color);
                body += " stroke=\"none\"/>";
            }
            return svgWrap(body, "stroke=\"none\"");
        }

        if (name == "compass") {
            std::string body = "<circle cx=\"12\" cy=\"12\" r=\"10.8\"/>";
            const SkColor tick = withAlpha(color, 180);
            for (int i = 0; i < 24; ++i) {
                const float angle = -kPi * 0.5f + static_cast<float>(i) * (2.0f * kPi / 24.0f);
                const float outer = 22.0f;
                const float inner = outer - ((i % 6 == 0) ? 4.6f : 2.6f);
                const float x1 = 12.0f + std::cos(angle) * inner * 0.48f;
                const float y1 = 12.0f + std::sin(angle) * inner * 0.48f;
                const float x2 = 12.0f + std::cos(angle) * outer * 0.48f;
                const float y2 = 12.0f + std::sin(angle) * outer * 0.48f;
                body += "<path d=\"M" + svgFloat(x1) + " " + svgFloat(y1) + " " + svgFloat(x2) + " " + svgFloat(y2) +
                        "\" " + svgStrokeAttrs(tick, 0.35f) + "/>";
            }
            body += "<path d=\"M12 3 14.2 11.5 12 10 9.8 11.5Z\" ";
            body += svgFillAttrs(SkColorSetARGB(SkColorGetA(color), 227, 74, 82));
            body += " stroke=\"none\"/>";
            body += "<path d=\"M12 21 14.2 12.5 12 14 9.8 12.5Z\" ";
            body += svgFillAttrs(SkColorSetARGB(static_cast<U8CPU>(std::min(230, static_cast<int>(SkColorGetA(color)))), 224, 232, 239));
            body += " stroke=\"none\"/>";
            return svgWrap(body, svgStrokeAttrs(withAlpha(color, 145), 0.35f));
        }

        return {};
    }

};

class SkiaFontEngine final : public Rml::FontEngineInterface {
public:
    struct Face {
        int size = 16;
        bool bold = false;
        sk_sp<SkTypeface> typeface;
        Rml::FontMetrics metrics{};
    };

    SkiaFontEngine() {
        fontMgr_ = SkFontMgr_New_DirectWrite();
        if (!fontMgr_) {
            fontMgr_ = SkFontMgr_New_GDI();
        }
    }

    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family,
                                          Rml::Style::FontStyle,
                                          Rml::Style::FontWeight weight,
                                          int size) override {
        const int clampedSize = std::max(1, size);
        const bool bold = static_cast<int>(weight) >= static_cast<int>(Rml::Style::FontWeight::Bold);
        const uint64_t key = (static_cast<uint64_t>(clampedSize) << 1) | (bold ? 1u : 0u);
        auto [it, inserted] = faces_.try_emplace(key);
        if (inserted) {
            Face& face = it->second;
            face.size = clampedSize;
            face.bold = bold;
            face.typeface = pickTypeface(family, bold);
            face.metrics = measureMetrics(face);
        }
        return reinterpret_cast<Rml::FontFaceHandle>(&it->second);
    }

    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override {
        return face(handle).metrics;
    }

    int GetStringWidth(Rml::FontFaceHandle handle,
                       Rml::StringView string,
                       const Rml::TextShapingContext& textShaping,
                       Rml::Character = Rml::Character::Null) override {
        return measureString(face(handle), string, textShaping.letter_spacing);
    }

    int GenerateString(Rml::RenderManager& renderManager,
                       Rml::FontFaceHandle handle,
                       Rml::FontEffectsHandle,
                       Rml::StringView string,
                       Rml::Vector2f position,
                       Rml::ColourbPremultiplied color,
                       float,
                       const Rml::TextShapingContext& textShaping,
                       Rml::TexturedMeshList& meshList) override {
        const Face& fontFace = face(handle);
        const Rml::String text(string.begin(), string.end());
        const int width = measureString(fontFace, string, textShaping.letter_spacing);
        if (text.empty() || width <= 0) {
            return std::max(0, width);
        }
        if (counters_) {
            ++counters_->generatedStrings;
        }

        const float textureScale = textureScale_;
        const float texturePadding = 4.0f;
        const float logicalTextureWidth = static_cast<float>(width) + texturePadding * 2.0f;
        const float logicalTextureHeight = std::ceil(fontFace.metrics.line_spacing) + texturePadding * 2.0f;
        const int textureWidth = std::max(1, static_cast<int>(std::ceil(logicalTextureWidth * textureScale)));
        const int textureHeight = std::max(1, static_cast<int>(std::ceil(logicalTextureHeight * textureScale)));
        const float baselineY = texturePadding + fontFace.metrics.ascent;

        Rml::CallbackTexture callback = renderManager.MakeCallbackTexture(
            [text, fontFace, color, textureScale, textureWidth, textureHeight, texturePadding, baselineY](const Rml::CallbackTextureInterface& textureInterface) {
                std::vector<uint8_t> rgba(static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 4u, 0u);

                const SkImageInfo info = SkImageInfo::Make(textureWidth,
                                                           textureHeight,
                                                           kRGBA_8888_SkColorType,
                                                           kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, rgba.data(), static_cast<size_t>(textureWidth) * 4u);
                if (surface) {
                    SkCanvas* canvas = surface->getCanvas();
                    canvas->clear(SK_ColorTRANSPARENT);
                    SkPaint paint;
                    paint.setAntiAlias(true);
                    paint.setStyle(SkPaint::kFill_Style);
                    paint.setColor(toSkColor(color));

                    SkFont font(fontFace.typeface, static_cast<float>(fontFace.size) * textureScale);
                    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
                    font.setHinting(SkFontHinting::kNormal);
                    font.setSubpixel(true);
                    canvas->drawSimpleText(text.data(),
                                           text.size(),
                                           SkTextEncoding::kUTF8,
                                           texturePadding * textureScale,
                                           baselineY * textureScale,
                                           font,
                                           paint);
                }

                return textureInterface.GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {textureWidth, textureHeight});
            });

        Rml::TexturedMesh texturedMesh;
        texturedMesh.texture = static_cast<Rml::Texture>(callback);
        const Rml::Vector2f topLeft = (position + Rml::Vector2f(0.0f, -fontFace.metrics.ascent) -
                                      Rml::Vector2f(texturePadding, texturePadding));
        const Rml::Vector2f dimensions(static_cast<float>(textureWidth) / textureScale,
                                       static_cast<float>(textureHeight) / textureScale);
        Rml::MeshUtilities::GenerateQuad(texturedMesh.mesh, topLeft, dimensions, Rml::ColourbPremultiplied(255, 255, 255, 255));
        meshList.push_back(std::move(texturedMesh));
        liveTextures_.push_back(std::move(callback));
        return width;
    }

    int GetVersion(Rml::FontFaceHandle) override {
        return version_;
    }

    void ReleaseFontResources() override {
        releaseTextTextures();
    }

    void beginFrame(float textureScale) {
        const float clampedScale = std::max(1.0f, textureScale);
        if (std::abs(textureScale_ - clampedScale) > 0.001f) {
            textureScale_ = clampedScale;
            ++version_;
            releaseTextTextures();
        }
    }

    void setCounters(RmlBenchCounters* counters) {
        counters_ = counters;
    }

private:
    sk_sp<SkFontMgr> fontMgr_;
    float textureScale_ = 1.0f;
    int version_ = 1;
    RmlBenchCounters* counters_ = nullptr;

    static Face& face(Rml::FontFaceHandle handle) {
        return *reinterpret_cast<Face*>(handle);
    }

    sk_sp<SkTypeface> pickTypeface(const Rml::String& requestedFamily, bool bold) const {
        const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
        std::array<const char*, 6> families = {
            requestedFamily.empty() ? nullptr : requestedFamily.c_str(),
            "Microsoft YaHei UI",
            "Microsoft YaHei",
            "Segoe UI",
            "Arial",
            nullptr};

        for (const char* family : families) {
            if (!fontMgr_ || !family) {
                continue;
            }
            sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(family, style);
            if (typeface) {
                return typeface;
            }
        }
        return nullptr;
    }

    static SkFont font(const Face& fontFace) {
        SkFont skFont(fontFace.typeface, static_cast<float>(fontFace.size));
        skFont.setEdging(SkFont::Edging::kSubpixelAntiAlias);
        skFont.setHinting(SkFontHinting::kNormal);
        skFont.setSubpixel(true);
        return skFont;
    }

    static SkColor toSkColor(Rml::ColourbPremultiplied color) {
        if (color.alpha == 0) {
            return SK_ColorTRANSPARENT;
        }
        const auto unpremultiply = [alpha = color.alpha](uint8_t value) {
            return static_cast<uint8_t>(std::min(255, static_cast<int>(value) * 255 / static_cast<int>(alpha)));
        };
        return SkColorSetARGB(color.alpha,
                              unpremultiply(color.red),
                              unpremultiply(color.green),
                              unpremultiply(color.blue));
    }

    static Rml::FontMetrics measureMetrics(const Face& fontFace) {
        Rml::FontMetrics metrics{};
        metrics.size = fontFace.size;

        SkFontMetrics skMetrics{};
        font(fontFace).getMetrics(&skMetrics);
        metrics.ascent = std::max(1.0f, -skMetrics.fAscent);
        metrics.descent = std::max(1.0f, skMetrics.fDescent);
        metrics.line_spacing = std::max(static_cast<float>(fontFace.size),
                                        metrics.ascent + metrics.descent + std::max(0.0f, skMetrics.fLeading));
        metrics.x_height = skMetrics.fXHeight < 0.0f ? -skMetrics.fXHeight : static_cast<float>(fontFace.size) * 0.54f;

        SkScalar underlinePosition = 0.0f;
        SkScalar underlineThickness = 0.0f;
        metrics.underline_position = skMetrics.hasUnderlinePosition(&underlinePosition)
                                         ? underlinePosition
                                         : static_cast<float>(fontFace.size) * 0.12f;
        metrics.underline_thickness = skMetrics.hasUnderlineThickness(&underlineThickness)
                                          ? std::max(1.0f, underlineThickness)
                                          : std::max(1.0f, static_cast<float>(fontFace.size) * (fontFace.bold ? 0.08f : 0.06f));
        metrics.has_ellipsis = true;
        return metrics;
    }

    static int measureString(const Face& fontFace, Rml::StringView string, float letterSpacing) {
        if (string.empty()) {
            return 0;
        }
        const Rml::String text(string.begin(), string.end());
        SkRect bounds;
        const float width = font(fontFace).measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
        const float spacing = std::max(0, static_cast<int>(Rml::StringUtilities::LengthUTF8(string)) - 1) * letterSpacing;
        return std::max(1, static_cast<int>(std::ceil(width + spacing)));
    }

    void releaseTextTextures() {
        for (Rml::CallbackTexture& texture : liveTextures_) {
            texture.Release();
        }
        liveTextures_.clear();
    }

    std::map<uint64_t, Face> faces_;
    std::vector<Rml::CallbackTexture> liveTextures_;
};

class SkiaRenderInterface final : public Rml::RenderInterface {
public:
    struct Geometry {
        std::vector<SkPoint> positions;
        std::vector<SkColor> colors;
        std::vector<SkPoint> normalizedTexCoords;
        std::vector<uint16_t> indices;
        mutable sk_sp<SkVertices> cachedVertices;
        mutable int cachedTextureWidth = 0;
        mutable int cachedTextureHeight = 0;

        sk_sp<SkVertices> verticesForTexture(int textureWidth, int textureHeight) const {
            if (cachedVertices && cachedTextureWidth == textureWidth && cachedTextureHeight == textureHeight) {
                return cachedVertices;
            }

            std::vector<SkPoint> texCoords;
            texCoords.reserve(normalizedTexCoords.size());
            for (const SkPoint& texCoord : normalizedTexCoords) {
                texCoords.push_back(SkPoint::Make(texCoord.x() * static_cast<float>(textureWidth),
                                                  texCoord.y() * static_cast<float>(textureHeight)));
            }

            cachedVertices = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode,
                                                  static_cast<int>(positions.size()),
                                                  positions.data(),
                                                  texCoords.empty() ? nullptr : texCoords.data(),
                                                  colors.data(),
                                                  static_cast<int>(indices.size()),
                                                  indices.data());
            cachedTextureWidth = textureWidth;
            cachedTextureHeight = textureHeight;
            return cachedVertices;
        }
    };

    struct Texture {
        int width = 0;
        int height = 0;
        sk_sp<SkImage> image;
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        mutable GrDirectContext* gpuContext = nullptr;
        mutable sk_sp<SkImage> gpuImage;
#endif

        sk_sp<SkImage> imageForCanvas(SkCanvas* canvas) const {
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
            if (canvas && image) {
                GrDirectContext* directContext = GrAsDirectContext(canvas->recordingContext());
                if (directContext) {
                    if (gpuImage && gpuContext != directContext) {
                        gpuImage.reset();
                        gpuContext = nullptr;
                    }
                    if (!gpuImage) {
                        gpuImage = SkImages::TextureFromImage(directContext,
                                                              image.get(),
                                                              skgpu::Mipmapped::kNo,
                                                              skgpu::Budgeted::kYes);
                        gpuContext = gpuImage ? directContext : nullptr;
                    }
                    if (gpuImage) {
                        return gpuImage;
                    }
                }
            }
#else
            (void)canvas;
#endif
            return image;
        }
    };

    void begin(uint32_t* pixels, int width, int height, float scale) {
        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        surface_ = SkSurfaces::WrapPixels(info, pixels, static_cast<size_t>(width) * sizeof(uint32_t));
        beginCanvas(surface_ ? surface_->getCanvas() : nullptr, width, height, scale, true);
    }

    void begin(SkCanvas& canvas, int width, int height, float scale) {
        beginCanvas(&canvas, width, height, scale, false);
    }

    void end() {
        if (canvas_) {
            if (scissorSaveCount_ != 0) {
                canvas_->restoreToCount(scissorSaveCount_);
                scissorSaveCount_ = 0;
            }
            if (canvasSaveCount_ != 0) {
                canvas_->restoreToCount(canvasSaveCount_);
                canvasSaveCount_ = 0;
            }
        }
        canvas_ = nullptr;
        surface_.reset();
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        if (counters_) {
            ++counters_->compiledGeometries;
        }
        if (vertices.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max()) ||
            indices.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return 0;
        }

        auto* geometry = new Geometry;
        geometry->positions.reserve(vertices.size());
        geometry->colors.reserve(vertices.size());
        geometry->normalizedTexCoords.reserve(vertices.size());
        for (const Rml::Vertex& vertex : vertices) {
            geometry->positions.push_back(SkPoint::Make(vertex.position.x, vertex.position.y));
            geometry->colors.push_back(toSkColor(vertex.colour));
            geometry->normalizedTexCoords.push_back(SkPoint::Make(vertex.tex_coord.x, vertex.tex_coord.y));
        }

        geometry->indices.reserve(indices.size());
        for (int index : indices) {
            if (index < 0 ||
                index >= static_cast<int>(vertices.size()) ||
                index > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
                delete geometry;
                return 0;
            }
            geometry->indices.push_back(static_cast<uint16_t>(index));
        }
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle) override {
        const auto* geometry = reinterpret_cast<const Geometry*>(handle);
        const auto* texture = reinterpret_cast<const Texture*>(textureHandle);
        if (!geometry || !canvas_ || geometry->positions.empty() || geometry->indices.empty()) {
            return;
        }

        sk_sp<SkImage> image = texture ? texture->imageForCanvas(canvas_) : nullptr;
        if (!image) {
            return;
        }
        if (counters_) {
            ++counters_->renderedGeometries;
            counters_->renderedVertices += geometry->positions.size();
            counters_->renderedIndices += geometry->indices.size();
        }

        sk_sp<SkVertices> vertices = geometry->verticesForTexture(texture->width, texture->height);
        if (!vertices) {
            return;
        }

        SkPaint paint;
        paint.setAntiAlias(false);
        paint.setColor(SK_ColorWHITE);
        paint.setBlendMode(SkBlendMode::kSrcOver);
        paint.setShader(image->makeShader(SkTileMode::kClamp,
                                          SkTileMode::kClamp,
                                          SkSamplingOptions(SkFilterMode::kLinear)));
        const int saveCount = canvas_->save();
        canvas_->translate(translation.x, translation.y);
        canvas_->drawVertices(vertices, SkBlendMode::kModulate, paint);
        canvas_->restoreToCount(saveCount);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        delete reinterpret_cast<Geometry*>(handle);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& textureDimensions, const Rml::String&) override {
        textureDimensions = {0, 0};
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i sourceDimensions) override {
        if (sourceDimensions.x <= 0 || sourceDimensions.y <= 0 || source.empty()) {
            return 0;
        }
        if (counters_) {
            ++counters_->generatedTextures;
            counters_->generatedTextureBytes += source.size();
        }
        auto* texture = new Texture;
        texture->width = sourceDimensions.x;
        texture->height = sourceDimensions.y;
        sk_sp<SkData> data = SkData::MakeWithCopy(source.data(), source.size());
        const SkImageInfo info = SkImageInfo::Make(sourceDimensions.x,
                                                   sourceDimensions.y,
                                                   kRGBA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        texture->image = SkImages::RasterFromData(info, std::move(data), static_cast<size_t>(sourceDimensions.x) * 4u);
        if (!texture->image) {
            delete texture;
            return 0;
        }
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        delete reinterpret_cast<Texture*>(texture);
    }

    void EnableScissorRegion(bool enable) override {
        if (scissorEnabled_ == enable) {
            return;
        }
        scissorEnabled_ = enable;
        updateScissorClip();
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_ = region;
        if (scissorEnabled_) {
            updateScissorClip();
        }
    }

    void setCounters(RmlBenchCounters* counters) {
        counters_ = counters;
    }

private:
    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0f;
    int canvasSaveCount_ = 0;
    bool scissorEnabled_ = false;
    int scissorSaveCount_ = 0;
    Rml::Rectanglei scissor_ = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    RmlBenchCounters* counters_ = nullptr;

    void beginCanvas(SkCanvas* canvas, int width, int height, float scale, bool clear) {
        end();
        width_ = width;
        height_ = height;
        scale_ = std::max(0.1f, scale);
        canvas_ = canvas;
        scissorEnabled_ = false;
        scissor_ = Rml::Rectanglei::FromPositionSize({0, 0}, {width, height});
        if (canvas_) {
            if (clear) {
                canvas_->clear(SK_ColorTRANSPARENT);
            }
            canvasSaveCount_ = canvas_->save();
            canvas_->scale(scale_, scale_);
        }
    }

    static SkColor toSkColor(Rml::ColourbPremultiplied color) {
        if (color.alpha == 0) {
            return SK_ColorTRANSPARENT;
        }
        const auto unpremultiply = [alpha = color.alpha](uint8_t value) {
            return static_cast<uint8_t>(std::min(255, static_cast<int>(value) * 255 / static_cast<int>(alpha)));
        };
        return SkColorSetARGB(color.alpha,
                              unpremultiply(color.red),
                              unpremultiply(color.green),
                              unpremultiply(color.blue));
    }

    void updateScissorClip() {
        if (!canvas_) {
            return;
        }
        if (scissorSaveCount_ != 0) {
            canvas_->restoreToCount(scissorSaveCount_);
            scissorSaveCount_ = 0;
        }
        if (!scissorEnabled_) {
            return;
        }
        scissorSaveCount_ = canvas_->save();
        canvas_->clipRect(SkRect::MakeLTRB(static_cast<float>(scissor_.Left()),
                                           static_cast<float>(scissor_.Top()),
                                           static_cast<float>(scissor_.Right()),
                                           static_cast<float>(scissor_.Bottom())),
                          true);
    }
};

class SkiaChromeRenderer {
public:
    void drawApp(SkCanvas& c,
                 Rml::ElementDocument* document,
                 float width,
                 float height,
                 const BrowserPaintStyleMap& paintStyles,
                 RmlDrawTiming* timing = nullptr) const {
        const auto backgroundStart = std::chrono::steady_clock::now();
        drawBackground(c, width, height);
        const auto sidebarStart = std::chrono::steady_clock::now();
        if (document) {
            drawSidebar(c, document, height);
            const auto panelStart = std::chrono::steady_clock::now();
            drawLayerPanel(c, document, paintStyles);
            const auto compassStart = std::chrono::steady_clock::now();
            drawCompass(c, document, width - 94.0f, height - 162.0f);
            const auto statusStart = std::chrono::steady_clock::now();
            drawStatusBar(c, document, width, height);
            const auto frameStart = std::chrono::steady_clock::now();
            drawFrame(c, document, width, height);
            const auto stop = std::chrono::steady_clock::now();
            addTiming(timing, backgroundStart, sidebarStart, panelStart, compassStart, statusStart, frameStart, stop);
            return;
        }

        drawSidebar(c, nullptr, height);
        const auto panelStart = std::chrono::steady_clock::now();
        drawLayerPanel(c, nullptr, paintStyles);
        const auto compassStart = std::chrono::steady_clock::now();
        drawCompass(c, nullptr, width - 94.0f, height - 162.0f);
        const auto statusStart = std::chrono::steady_clock::now();
        drawStatusBar(c, nullptr, width, height);
        const auto frameStart = std::chrono::steady_clock::now();
        drawFrame(c, nullptr, width, height);
        const auto stop = std::chrono::steady_clock::now();
        addTiming(timing, backgroundStart, sidebarStart, panelStart, compassStart, statusStart, frameStart, stop);
    }

private:
    struct ChromeBox {
        Rect rect;
        float radius = 0.0f;
        SkColor fillColor = SK_ColorTRANSPARENT;
        SkColor borderColor = SK_ColorTRANSPARENT;
        float borderWidth = 0.0f;
    };

    SkPaint fill(SkColor color) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        p.setColor(color);
        return p;
    }

    SkPaint stroke(SkColor color, float width = 1.0f) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(width);
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setStrokeJoin(SkPaint::kRound_Join);
        p.setColor(color);
        return p;
    }

    SkRRect rr(const Rect& r, float radius) const {
        return SkRRect::MakeRectXY(r.sk(), radius, radius);
    }

    static SkColor toSkColor(Rml::Colourb color, float opacity = 1.0f) {
        const float alpha = static_cast<float>(color.alpha) * std::clamp(opacity, 0.0f, 1.0f);
        return SkColorSetARGB(clampByte(alpha), color.red, color.green, color.blue);
    }

    static int hexNibble(char value) {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    static bool parseHexByte(const Rml::String& value, size_t offset, uint8_t& byte) {
        if (offset + 1 >= value.size()) {
            return false;
        }
        const int high = hexNibble(value[offset]);
        const int low = hexNibble(value[offset + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        byte = static_cast<uint8_t>((high << 4) | low);
        return true;
    }

    static bool parseDomColor(const Rml::String& value, SkColor& color) {
        if (value.size() != 7 && value.size() != 9) {
            return false;
        }
        if (value[0] != '#') {
            return false;
        }
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;
        if (!parseHexByte(value, 1, r) || !parseHexByte(value, 3, g) || !parseHexByte(value, 5, b)) {
            return false;
        }
        if (value.size() == 9 && !parseHexByte(value, 7, a)) {
            return false;
        }
        color = SkColorSetARGB(a, r, g, b);
        return true;
    }

    ChromeBox boxFromElement(Rml::ElementDocument* document,
                             const char* id,
                             const Rect& fallback,
                             float fallbackRadius,
                             SkColor fallbackFill,
                             SkColor fallbackBorder,
                             float fallbackBorderWidth) const {
        ChromeBox box{fallback, fallbackRadius, fallbackFill, fallbackBorder, fallbackBorderWidth};
        if (!document) {
            return box;
        }

        Rml::Element* element = document->GetElementById(id);
        if (!element) {
            return box;
        }

        const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        if (size.x <= 0.0f || size.y <= 0.0f) {
            return box;
        }

        const Rml::Style::ComputedValues& values = element->GetComputedValues();
        box.rect = {position.x, position.y, size.x, size.y};
        box.radius = std::max({values.border_top_left_radius(),
                               values.border_top_right_radius(),
                               values.border_bottom_right_radius(),
                               values.border_bottom_left_radius()});
        if (box.radius <= 0.0f) {
            box.radius = fallbackRadius;
        }
        box.fillColor = toSkColor(values.background_color(), values.opacity());
        if (SkColorGetA(box.fillColor) == 0) {
            box.fillColor = fallbackFill;
        }
        box.borderWidth = std::max({values.border_top_width(),
                                    values.border_right_width(),
                                    values.border_bottom_width(),
                                    values.border_left_width()});
        if (box.borderWidth <= 0.0f) {
            box.borderWidth = fallbackBorderWidth;
        }
        if (values.border_right_width() == box.borderWidth) {
            box.borderColor = toSkColor(values.border_right_color(), values.opacity());
        } else if (values.border_bottom_width() == box.borderWidth) {
            box.borderColor = toSkColor(values.border_bottom_color(), values.opacity());
        } else if (values.border_left_width() == box.borderWidth) {
            box.borderColor = toSkColor(values.border_left_color(), values.opacity());
        } else {
            box.borderColor = toSkColor(values.border_top_color(), values.opacity());
        }
        if (SkColorGetA(box.borderColor) == 0 ||
            (box.borderColor == SK_ColorWHITE && fallbackBorder != SK_ColorWHITE)) {
            box.borderColor = fallbackBorder;
        }
        SkColor domColor = SK_ColorTRANSPARENT;
        if (parseDomColor(element->GetAttribute<Rml::String>("data-fill", Rml::String()), domColor)) {
            box.fillColor = domColor;
        }
        if (parseDomColor(element->GetAttribute<Rml::String>("data-border", Rml::String()), domColor)) {
            box.borderColor = domColor;
        }
        return box;
    }

    void line(SkCanvas& c, float x1, float y1, float x2, float y2, SkColor color, float width = 1.0f) const {
        c.drawLine(x1, y1, x2, y2, stroke(color, width));
    }

    std::vector<SkColor4f> gradientColors(const SkColor* colors, int count) const {
        std::vector<SkColor4f> stops;
        stops.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            stops.push_back(SkColor4f::FromColor(colors[i]));
        }
        return stops;
    }

    std::vector<float> gradientPositions(const SkScalar* positions, int count) const {
        std::vector<float> stops;
        if (!positions) {
            return stops;
        }
        stops.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            stops.push_back(positions[i]);
        }
        return stops;
    }

    SkGradient makeGradient(SkSpan<const SkColor4f> colors, SkSpan<const float> positions) const {
        return SkGradient(SkGradient::Colors(colors, positions, SkTileMode::kClamp), {});
    }

    SkPaint linearPaint(SkPoint a,
                        SkPoint b,
                        const SkColor* colors,
                        const SkScalar* positions,
                        int count,
                        uint8_t alpha = 255) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        p.setAlphaf(static_cast<float>(alpha) / 255.0f);
        const SkPoint pts[2] = {a, b};
        const std::vector<SkColor4f> gradientColorStops = gradientColors(colors, count);
        const std::vector<float> gradientPositionStops = gradientPositions(positions, count);
        const SkGradient gradient = makeGradient(gradientColorStops, gradientPositionStops);
        p.setShader(SkShaders::LinearGradient(pts, gradient));
        return p;
    }

    SkPaint linearPaint(const Rect& rect, const BrowserLinearGradient& gradient) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);

        const float angle = (gradient.angleDeg - 90.0f) * kPi / 180.0f;
        const float dx = std::cos(angle);
        const float dy = std::sin(angle);
        const float halfLine = std::abs(dx) * rect.w * 0.5f + std::abs(dy) * rect.h * 0.5f;
        const float cx = rect.x + rect.w * 0.5f;
        const float cy = rect.y + rect.h * 0.5f;
        const SkPoint pts[2] = {
            SkPoint::Make(cx - dx * halfLine, cy - dy * halfLine),
            SkPoint::Make(cx + dx * halfLine, cy + dy * halfLine),
        };

        const std::vector<SkColor4f> gradientColorStops = gradientColors(gradient.colors.data(),
                                                                         static_cast<int>(gradient.colors.size()));
        const std::vector<float> gradientPositionStops = gradientPositions(gradient.positions.data(),
                                                                           static_cast<int>(gradient.positions.size()));
        const SkGradient skGradient = makeGradient(gradientColorStops, gradientPositionStops);
        p.setShader(SkShaders::LinearGradient(pts, skGradient));
        return p;
    }

    SkPaint radialPaint(SkPoint center,
                        float radius,
                        const SkColor* colors,
                        const SkScalar* positions,
                        int count) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        const std::vector<SkColor4f> gradientColorStops = gradientColors(colors, count);
        const std::vector<float> gradientPositionStops = gradientPositions(positions, count);
        const SkGradient gradient = makeGradient(gradientColorStops, gradientPositionStops);
        p.setShader(SkShaders::RadialGradient(center, radius, gradient));
        return p;
    }

    void drawBackground(SkCanvas& c, float width, float height) const {
        const SkColor baseColors[] = {rgb(7, 13, 20), rgb(21, 34, 49), rgb(12, 22, 33)};
        const SkScalar basePos[] = {0.0f, 0.48f, 1.0f};
        c.drawRect(SkRect::MakeWH(width, height),
                   linearPaint(SkPoint::Make(0.0f, 0.0f), SkPoint::Make(width, height), baseColors, basePos, 3));

        const SkColor upperGlow[] = {rgba(83, 119, 151, 120), rgba(26, 55, 83, 70), rgba(8, 15, 22, 0)};
        const SkScalar glowPos[] = {0.0f, 0.38f, 1.0f};
        c.drawRect(SkRect::MakeWH(width, height),
                   radialPaint(SkPoint::Make(575.0f, -130.0f), 820.0f, upperGlow, glowPos, 3));

        const SkColor sideGlow[] = {rgba(48, 84, 115, 70), rgba(16, 31, 47, 20), rgba(4, 9, 14, 0)};
        c.drawRect(SkRect::MakeWH(width, height),
                   radialPaint(SkPoint::Make(width * 0.78f, 190.0f), 980.0f, sideGlow, glowPos, 3));

        const SkColor vignette[] = {rgba(0, 0, 0, 0), rgba(2, 5, 8, 155)};
        const SkScalar vignettePos[] = {0.42f, 1.0f};
        c.drawRect(SkRect::MakeWH(width, height),
                   radialPaint(SkPoint::Make(width * 0.54f, height * 0.44f), width * 0.82f, vignette, vignettePos, 2));
    }

    void drawSidebar(SkCanvas& c, Rml::ElementDocument* document, float height) const {
        const ChromeBox sidebarBox = boxFromElement(document,
                                                    "chrome-sidebar",
                                                    {0.0f, 0.0f, 118.0f, height - 60.0f},
                                                    0.0f,
                                                    rgba(5, 10, 16, 238),
                                                    rgba(83, 118, 143, 46),
                                                    1.0f);
        const Rect sidebar = sidebarBox.rect;
        const SkColor colors[] = {rgba(5, 10, 16, 238), rgba(10, 18, 27, 218)};
        const SkScalar pos[] = {0.0f, 1.0f};
        c.drawRect(sidebar.sk(),
                   linearPaint(SkPoint::Make(sidebar.x, sidebar.y), SkPoint::Make(sidebar.x + sidebar.w, sidebar.y), colors, pos, 2));
        line(c, sidebar.x + sidebar.w - 0.5f, sidebar.y, sidebar.x + sidebar.w - 0.5f, sidebar.y + sidebar.h, sidebarBox.borderColor, sidebarBox.borderWidth);

        const ChromeBox activeBox = boxFromElement(document,
                                                   "chrome-nav-active",
                                                   {3.0f, 88.0f, 115.0f, 102.0f},
                                                   0.0f,
                                                   rgba(16, 224, 207, 38),
                                                   rgb(34, 224, 211),
                                                   0.0f);
        const Rect active = activeBox.rect;
        const SkColor activeColors[] = {rgba(16, 224, 207, 38), rgba(81, 178, 203, 16)};
        c.drawRect(active.sk(),
                   linearPaint(SkPoint::Make(active.x, 0.0f), SkPoint::Make(active.x + active.w, 0.0f), activeColors, pos, 2));
        c.drawRect(SkRect::MakeXYWH(active.x, active.y, 5.0f, active.h), fill(activeBox.borderColor));
    }

    static const BrowserPaintStyle* paintStyleForId(const BrowserPaintStyleMap& paintStyles, const char* id) {
        const auto it = paintStyles.find(id);
        return it == paintStyles.end() ? nullptr : &it->second;
    }

    static void addTiming(RmlDrawTiming* timing,
                          std::chrono::steady_clock::time_point backgroundStart,
                          std::chrono::steady_clock::time_point sidebarStart,
                          std::chrono::steady_clock::time_point panelStart,
                          std::chrono::steady_clock::time_point compassStart,
                          std::chrono::steady_clock::time_point statusStart,
                          std::chrono::steady_clock::time_point frameStart,
                          std::chrono::steady_clock::time_point stop) {
        if (!timing) {
            return;
        }
        timing->chromeBackgroundMs += std::chrono::duration<double, std::milli>(sidebarStart - backgroundStart).count();
        timing->chromeSidebarMs += std::chrono::duration<double, std::milli>(panelStart - sidebarStart).count();
        timing->chromePanelMs += std::chrono::duration<double, std::milli>(compassStart - panelStart).count();
        timing->chromeCompassMs += std::chrono::duration<double, std::milli>(statusStart - compassStart).count();
        timing->chromeStatusMs += std::chrono::duration<double, std::milli>(frameStart - statusStart).count();
        timing->chromeFrameMs += std::chrono::duration<double, std::milli>(stop - frameStart).count();
    }

    void drawBoxShadow(SkCanvas& c, const ChromeBox& box, const BrowserBoxShadow& shadow) const {
        if (shadow.inset || SkColorGetA(shadow.color) == 0) {
            return;
        }

        Rect shadowRect = box.rect;
        shadowRect.x += shadow.offsetX - shadow.spread;
        shadowRect.y += shadow.offsetY - shadow.spread;
        shadowRect.w += shadow.spread * 2.0f;
        shadowRect.h += shadow.spread * 2.0f;
        if (shadowRect.w <= 0.0f || shadowRect.h <= 0.0f) {
            return;
        }

        SkPaint shadowPaint = fill(shadow.color);
        if (shadow.blur > 0.0f) {
            shadowPaint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, shadow.blur * 0.5f));
        }
        c.drawRRect(rr(shadowRect, std::max(0.0f, box.radius + shadow.spread)), shadowPaint);
    }

    void drawLayerPanel(SkCanvas& c,
                        Rml::ElementDocument* document,
                        const BrowserPaintStyleMap& paintStyles) const {
        const ChromeBox panelBox = boxFromElement(document,
                                                  "chrome-panel",
                                                  {134.0f, 25.0f, 602.0f, 825.0f},
                                                  9.0f,
                                                  rgba(23, 39, 55, 214),
                                                  rgba(146, 179, 199, 160),
                                                  1.25f);
        const Rect panel = panelBox.rect;
        const BrowserPaintStyle* panelStyle = paintStyleForId(paintStyles, "chrome-panel");
        if (panelStyle && panelStyle->hasBoxShadow) {
            drawBoxShadow(c, panelBox, panelStyle->boxShadow);
        } else {
            const SkColor shadowColors[] = {rgba(0, 0, 0, 70), rgba(0, 0, 0, 0)};
            const SkScalar shadowPos[] = {0.0f, 1.0f};
            c.drawRoundRect(SkRect::MakeXYWH(panel.x - 8.0f, panel.y - 8.0f, panel.w + 16.0f, panel.h + 16.0f),
                            panelBox.radius + 5.0f,
                            panelBox.radius + 5.0f,
                            radialPaint(SkPoint::Make(panel.x + panel.w * 0.45f, panel.y + panel.h * 0.5f),
                                        520.0f,
                                        shadowColors,
                                        shadowPos,
                                        2));
        }

        const SkColor panelFill[] = {rgba(23, 39, 55, 214), rgba(11, 22, 32, 228), rgba(20, 34, 48, 205)};
        const SkScalar panelPos[] = {0.0f, 0.58f, 1.0f};
        c.drawRRect(rr(panel, panelBox.radius),
                    panelStyle && panelStyle->hasBackgroundGradient
                        ? linearPaint(panel, panelStyle->backgroundGradient)
                        : linearPaint(SkPoint::Make(panel.x, panel.y),
                                      SkPoint::Make(panel.x + panel.w, panel.y + panel.h),
                                      panelFill,
                                      panelPos,
                                      3));
        c.drawRRect(rr(panel, panelBox.radius), stroke(panelBox.borderColor, panelBox.borderWidth));
        c.drawRRect(rr({panel.x + 1.0f, panel.y + 1.0f, panel.w - 2.0f, panel.h - 2.0f}, std::max(0.0f, panelBox.radius - 1.0f)),
                    stroke(rgba(222, 244, 252, 38), 1.0f));

        drawActionButton(c, boxFromElement(document, "chrome-button-primary", {160.0f, 110.0f, 253.0f, 47.0f}, 7.0f, rgba(18, 190, 178, 130), rgba(38, 237, 218, 175), 1.1f), true);
        drawActionButton(c, boxFromElement(document, "chrome-button-secondary", {431.0f, 110.0f, 266.0f, 47.0f}, 7.0f, rgba(19, 32, 45, 104), rgba(142, 170, 193, 142), 1.1f), false);
        drawSearchBox(c, boxFromElement(document, "chrome-search", {161.0f, 180.0f, 536.0f, 46.0f}, 7.0f, rgba(11, 22, 32, 130), rgba(132, 164, 190, 130), 1.15f));
        drawTableRows(c, document);
        drawLayerDetails(c,
                         boxFromElement(document, "chrome-details", {149.0f, 566.0f, 568.0f, 262.0f}, 10.0f, rgba(11, 23, 33, 108), rgba(142, 170, 193, 135), 1.1f),
                         boxFromElement(document, "chrome-pill", {327.0f, 585.0f, 82.0f, 29.0f}, 15.0f, rgba(20, 212, 198, 108), SK_ColorTRANSPARENT, 0.0f));
    }

    void drawActionButton(SkCanvas& c, const ChromeBox& box, bool primary) const {
        const Rect& r = box.rect;
        if (primary) {
            const SkColor colors[] = {rgba(18, 190, 178, 130), rgba(8, 95, 102, 150)};
            const SkScalar pos[] = {0.0f, 1.0f};
            c.drawRRect(rr(r, box.radius),
                        linearPaint(SkPoint::Make(r.x, r.y), SkPoint::Make(r.x + r.w, r.y + r.h), colors, pos, 2));
            c.drawRRect(rr(r, box.radius), stroke(box.borderColor, box.borderWidth));
        } else {
            c.drawRRect(rr(r, box.radius), fill(box.fillColor));
            c.drawRRect(rr(r, box.radius), stroke(box.borderColor, box.borderWidth));
        }
    }

    void drawSearchBox(SkCanvas& c, const ChromeBox& box) const {
        c.drawRRect(rr(box.rect, box.radius), fill(box.fillColor));
        c.drawRRect(rr(box.rect, box.radius), stroke(box.borderColor, box.borderWidth));
    }

    void drawTableRows(SkCanvas& c, Rml::ElementDocument* document) const {
        drawLayerRow(c, boxFromElement(document, "chrome-row-0", {149.0f, 280.0f, 570.0f, 54.0f}, 0.0f, rgba(0, 168, 150, 150), SK_ColorTRANSPARENT, 0.0f), true);
        drawLayerRow(c, boxFromElement(document, "chrome-row-1", {149.0f, 334.0f, 570.0f, 54.0f}, 4.0f, rgba(11, 23, 33, 60), rgba(120, 149, 169, 32), 1.0f), false);
        drawLayerRow(c, boxFromElement(document, "chrome-row-2", {149.0f, 388.0f, 570.0f, 54.0f}, 4.0f, rgba(11, 23, 33, 60), rgba(120, 149, 169, 32), 1.0f), false);
        drawLayerRow(c, boxFromElement(document, "chrome-row-3", {149.0f, 442.0f, 570.0f, 54.0f}, 4.0f, rgba(11, 23, 33, 60), rgba(120, 149, 169, 32), 1.0f), false);
        drawLayerRow(c, boxFromElement(document, "chrome-row-4", {149.0f, 496.0f, 570.0f, 54.0f}, 4.0f, rgba(11, 23, 33, 60), rgba(120, 149, 169, 32), 1.0f), false);
    }

    void drawLayerRow(SkCanvas& c, const ChromeBox& box, bool selected) const {
        const Rect& row = box.rect;
        if (selected) {
            const SkColor colors[] = {rgba(0, 168, 150, 150), rgba(20, 173, 166, 105)};
            const SkScalar pos[] = {0.0f, 1.0f};
            c.drawRect(row.sk(),
                       linearPaint(SkPoint::Make(row.x, row.y), SkPoint::Make(row.x + row.w, row.y), colors, pos, 2));
            return;
        }

        c.drawRRect(rr({row.x, row.y, row.w, row.h - 1.0f}, box.radius), fill(box.fillColor));
        line(c, row.x + 2.0f, row.y + row.h - 0.5f, row.x + row.w - 2.0f, row.y + row.h - 0.5f, box.borderColor, box.borderWidth);
    }

    void drawLayerDetails(SkCanvas& c, const ChromeBox& cardBox, const ChromeBox& pillBox) const {
        const Rect& card = cardBox.rect;
        c.drawRRect(rr(card, cardBox.radius), fill(cardBox.fillColor));
        c.drawRRect(rr(card, cardBox.radius), stroke(cardBox.borderColor, cardBox.borderWidth));

        const Rect& pill = pillBox.rect;
        const SkColor pillColors[] = {rgba(20, 212, 198, 108), rgba(10, 121, 125, 120)};
        const SkScalar pillPos[] = {0.0f, 1.0f};
        c.drawRoundRect(pill.sk(), pillBox.radius, pillBox.radius,
                        linearPaint(SkPoint::Make(pill.x, pill.y), SkPoint::Make(pill.x + pill.w, pill.y), pillColors, pillPos, 2));

        line(c, card.x + 20.0f, card.y + 64.0f, card.x + card.w - 20.0f, card.y + 64.0f, rgba(140, 170, 191, 104), 1.0f);
    }

    void drawStatusBar(SkCanvas& c, Rml::ElementDocument* document, float width, float height) const {
        const ChromeBox barBox = boxFromElement(document,
                                                "chrome-status",
                                                {0.0f, height - 60.0f, width, 60.0f},
                                                0.0f,
                                                rgba(8, 16, 24, 214),
                                                rgba(108, 136, 158, 66),
                                                1.0f);
        const Rect& bar = barBox.rect;
        const SkColor colors[] = {rgba(8, 16, 24, 214), rgba(4, 10, 16, 236)};
        const SkScalar pos[] = {0.0f, 1.0f};
        c.drawRect(bar.sk(),
                   linearPaint(SkPoint::Make(0.0f, bar.y), SkPoint::Make(0.0f, height), colors, pos, 2));
        line(c, bar.x, bar.y + 0.5f, bar.x + bar.w, bar.y + 0.5f, barBox.borderColor, barBox.borderWidth);
    }

    void drawCompass(SkCanvas& c, Rml::ElementDocument* document, float fallbackCx, float fallbackCy) const {
        ChromeBox compassBox = boxFromElement(document,
                                              "chrome-compass",
                                              {fallbackCx - 51.0f, fallbackCy - 51.0f, 102.0f, 102.0f},
                                              51.0f,
                                              SK_ColorTRANSPARENT,
                                              rgba(197, 213, 224, 145),
                                              1.1f);
        const float cx = compassBox.rect.x + compassBox.rect.w * 0.5f;
        const float cy = compassBox.rect.y + compassBox.rect.h * 0.5f;
        const float r = std::min(compassBox.rect.w, compassBox.rect.h) * 0.5f;
        c.drawCircle(cx, cy, r, stroke(rgba(197, 213, 224, 145), 1.1f));
        c.drawCircle(cx, cy, r * 0.74f, stroke(rgba(197, 213, 224, 45), 1.0f));

        for (int i = 0; i < 24; ++i) {
            const float angle = -kPi * 0.5f + i * (2.0f * kPi / 24.0f);
            const float len = (i % 6 == 0) ? 11.0f : 6.0f;
            const float x1 = cx + std::cos(angle) * (r - len);
            const float y1 = cy + std::sin(angle) * (r - len);
            const float x2 = cx + std::cos(angle) * r;
            const float y2 = cy + std::sin(angle) * r;
            line(c, x1, y1, x2, y2, rgba(207, 220, 230, i % 6 == 0 ? 150 : 95), 1.0f);
        }

        SkPathBuilder north;
        north.moveTo(cx, cy - r + 12.0f);
        north.lineTo(cx + 9.0f, cy - 1.0f);
        north.lineTo(cx, cy - 8.0f);
        north.lineTo(cx - 9.0f, cy - 1.0f);
        north.close();
        c.drawPath(north.detach(), fill(rgb(227, 74, 82)));

        SkPathBuilder south;
        south.moveTo(cx, cy + r - 13.0f);
        south.lineTo(cx + 9.0f, cy + 1.0f);
        south.lineTo(cx, cy + 8.0f);
        south.lineTo(cx - 9.0f, cy + 1.0f);
        south.close();
        c.drawPath(south.detach(), fill(rgba(224, 232, 239, 230)));
        c.drawCircle(cx, cy, 6.5f, fill(rgba(11, 20, 30, 240)));
        c.drawCircle(cx, cy, 3.0f, fill(rgba(190, 210, 222, 215)));
    }

    void drawFrame(SkCanvas& c, Rml::ElementDocument* document, float width, float height) const {
        const ChromeBox frame = boxFromElement(document,
                                               "chrome-frame",
                                               {2.0f, 2.0f, width - 4.0f, height - 4.0f},
                                               10.0f,
                                               SK_ColorTRANSPARENT,
                                               rgba(125, 155, 173, 105),
                                               1.0f);
        if (frame.borderWidth > 0.0f && SkColorGetA(frame.borderColor) > 0) {
            c.drawRoundRect(frame.rect.sk(), frame.radius, frame.radius, stroke(frame.borderColor, frame.borderWidth));
        }
    }
};

class RmlLayerRenderer {
public:
    RmlLayerRenderer() {
        Rml::SetSystemInterface(&system_);
        Rml::SetRenderInterface(&render_);
        Rml::SetFontEngineInterface(&font_);
        rmlReady_ = Rml::Initialise();
        if (rmlReady_) {
            Rml::Factory::RegisterElementInstancer("icon", &iconInstancer_);
            context_ = Rml::CreateContext("rml_layer_desk", {kBaseWidth, kBaseHeight});
            loadDocument();
        }
    }

    ~RmlLayerRenderer() {
        if (rmlReady_) {
            font_.ReleaseFontResources();
            Rml::RemoveContext("rml_layer_desk");
            Rml::Shutdown();
        }
    }

    void draw(uint32_t* pixels, int width, int height, float dpiScale = 1.0f) {
        if (!pixels || width <= 0 || height <= 0) {
            return;
        }

        if (copyCachedFrame(pixels, width, height, dpiScale)) {
            return;
        }
        if (ensureCachedFrame(width, height, dpiScale) &&
            copyCachedFrame(pixels, width, height, dpiScale)) {
            return;
        }

        const SkImageInfo chromeInfo = SkImageInfo::Make(width,
                                                         height,
                                                         kBGRA_8888_SkColorType,
                                                         kPremul_SkAlphaType);
        sk_sp<SkSurface> chromeSurface = SkSurfaces::WrapPixels(chromeInfo,
                                                                pixels,
                                                                static_cast<size_t>(width) * sizeof(uint32_t));
        if (chromeSurface) {
            draw(*chromeSurface->getCanvas(), width, height, dpiScale);
            updateCachedFrame(pixels, width, height, dpiScale);
        }
    }

    void draw(SkCanvas& canvas,
              int width,
              int height,
              float dpiScale = 1.0f,
              RmlDrawTiming* timing = nullptr) {
        if (width <= 0 || height <= 0) {
            return;
        }
        if (!timing && drawCachedFrame(canvas, width, height, dpiScale)) {
            return;
        }
        if (!timing &&
            ensureCachedFrame(width, height, dpiScale) &&
            drawCachedFrame(canvas, width, height, dpiScale)) {
            return;
        }

        drawUncached(canvas, width, height, dpiScale, timing);
    }

    void drawUncached(SkCanvas& canvas, int width, int height, float dpiScale, RmlDrawTiming* timing) {
        if (width <= 0 || height <= 0) {
            return;
        }

        Rml::ElementDocument* document = nullptr;
        const float scale = std::max(0.1f, dpiScale);
        const int logicalWidth = std::max(1, static_cast<int>(std::round(static_cast<float>(width) / scale)));
        const int logicalHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(height) / scale)));
        const auto prepareStart = std::chrono::steady_clock::now();
        prepareFrame(logicalWidth, logicalHeight, document);
        const auto chromeStart = std::chrono::steady_clock::now();
        drawChrome(canvas, document, logicalWidth, logicalHeight, scale, timing);
        const auto rmlStart = std::chrono::steady_clock::now();
        renderRmlOverlay(canvas, width, height, scale);
        const auto stop = std::chrono::steady_clock::now();
        if (timing) {
            timing->prepareMs += std::chrono::duration<double, std::milli>(chromeStart - prepareStart).count();
            timing->chromeMs += std::chrono::duration<double, std::milli>(rmlStart - chromeStart).count();
            timing->rmlMs += std::chrono::duration<double, std::milli>(stop - rmlStart).count();
        }
    }

    bool savePng(const wchar_t* path, int width = kBaseWidth, int height = kBaseHeight, float dpiScale = 1.0f) {
        if (!path || width <= 0 || height <= 0) {
            return false;
        }

        std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff070c12u);
        draw(pixels.data(), width, height, dpiScale);
        for (uint32_t& pixel : pixels) {
            const uint8_t a = static_cast<uint8_t>((pixel >> 24u) & 0xffu);
            if (a == 0 || a == 255) {
                continue;
            }
            const uint8_t b = static_cast<uint8_t>(pixel & 0xffu);
            const uint8_t g = static_cast<uint8_t>((pixel >> 8u) & 0xffu);
            const uint8_t r = static_cast<uint8_t>((pixel >> 16u) & 0xffu);
            const auto unpremul = [a](uint8_t value) {
                return static_cast<uint8_t>(std::min(255u, (static_cast<unsigned>(value) * 255u + a / 2u) / a));
            };
            pixel = (static_cast<uint32_t>(a) << 24u) |
                    (static_cast<uint32_t>(unpremul(r)) << 16u) |
                    (static_cast<uint32_t>(unpremul(g)) << 8u) |
                    static_cast<uint32_t>(unpremul(b));
        }

        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;

        bool ok = false;
        const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool comReady = SUCCEEDED(initResult) || initResult == RPC_E_CHANGED_MODE;
        const bool shouldUninitializeCom = SUCCEEDED(initResult);
        if (comReady) {
            const HRESULT factoryResult = CoCreateInstance(CLSID_WICImagingFactory,
                                                           nullptr,
                                                           CLSCTX_INPROC_SERVER,
                                                           IID_PPV_ARGS(&factory));
            if (SUCCEEDED(factoryResult) &&
                SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
                SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) &&
                SUCCEEDED(frame->Initialize(properties)) &&
                SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                if (SUCCEEDED(frame->SetPixelFormat(&format)) &&
                    IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
                    SUCCEEDED(frame->WritePixels(static_cast<UINT>(height),
                                                 static_cast<UINT>(width * sizeof(uint32_t)),
                                                 static_cast<UINT>(pixels.size() * sizeof(uint32_t)),
                                                 reinterpret_cast<BYTE*>(pixels.data()))) &&
                    SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(encoder->Commit())) {
                    ok = true;
                }
            }
        }

        if (properties) {
            properties->Release();
        }
        if (frame) {
            frame->Release();
        }
        if (encoder) {
            encoder->Release();
        }
        if (stream) {
            stream->Release();
        }
        if (factory) {
            factory->Release();
        }
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return ok;
    }

    double benchDraw(int width,
                     int height,
                     float dpiScale,
                     int frames,
                     RmlDrawTiming* timing,
                     RmlBenchCounters* counters) {
        width = std::max(1, width);
        height = std::max(1, height);
        frames = std::max(1, frames);
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff070c12u);

        draw(pixels.data(), width, height, dpiScale);
        render_.setCounters(counters);
        font_.setCounters(counters);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) {
            const SkImageInfo info = SkImageInfo::Make(width,
                                                       height,
                                                       kBGRA_8888_SkColorType,
                                                       kPremul_SkAlphaType);
            sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info,
                                                               pixels.data(),
                                                               static_cast<size_t>(width) * sizeof(uint32_t));
            if (surface) {
                draw(*surface->getCanvas(), width, height, dpiScale, timing);
            }
        }
        const auto stop = std::chrono::steady_clock::now();
        render_.setCounters(nullptr);
        font_.setCounters(nullptr);
        const auto elapsed = std::chrono::duration<double, std::milli>(stop - start).count();
        return elapsed / static_cast<double>(frames);
    }

    double benchCachedDraw(int width, int height, float dpiScale, int frames) {
        width = std::max(1, width);
        height = std::max(1, height);
        frames = std::max(1, frames);
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff070c12u);

        draw(pixels.data(), width, height, dpiScale);
        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info,
                                                          pixels.data(),
                                                          static_cast<size_t>(width) * sizeof(uint32_t));
        if (!surface) {
            return 0.0;
        }

        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) {
            draw(*surface->getCanvas(), width, height, dpiScale);
        }
        const auto stop = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double, std::milli>(stop - start).count();
        return elapsed / static_cast<double>(frames);
    }

private:
    RmlSystem system_;
    SkiaFontEngine font_;
    SkiaRenderInterface render_;
    SkiaChromeRenderer chrome_;
    Rml::ElementInstancerGeneric<LayerIconElement> iconInstancer_;
    std::string rmlDocument_;
    BrowserPaintStyleMap browserPaintStyles_;
    bool rmlReady_ = false;
    Rml::Context* context_ = nullptr;
    int logicalWidth_ = 0;
    int logicalHeight_ = 0;
    std::vector<uint32_t> cachedFramePixels_;
    sk_sp<SkImage> cachedFrameImage_;
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
    mutable GrDirectContext* cachedFrameGpuContext_ = nullptr;
    mutable sk_sp<SkImage> cachedFrameGpuImage_;
#endif
    int cachedFrameWidth_ = 0;
    int cachedFrameHeight_ = 0;
    float cachedFrameDpiScale_ = 0.0f;
    bool buildingCachedFrame_ = false;

    bool cacheMatches(int width, int height, float dpiScale) const {
        return width == cachedFrameWidth_ &&
               height == cachedFrameHeight_ &&
               std::abs(dpiScale - cachedFrameDpiScale_) <= 0.001f &&
               !cachedFramePixels_.empty();
    }

    bool copyCachedFrame(uint32_t* pixels, int width, int height, float dpiScale) const {
        if (!pixels || !cacheMatches(width, height, dpiScale)) {
            return false;
        }
        std::memcpy(pixels,
                    cachedFramePixels_.data(),
                    cachedFramePixels_.size() * sizeof(uint32_t));
        return true;
    }

    bool drawCachedFrame(SkCanvas& canvas, int width, int height, float dpiScale) const {
        if (!cachedFrameImage_ || !cacheMatches(width, height, dpiScale)) {
            return false;
        }
        sk_sp<SkImage> image = cachedFrameImage_;
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        GrDirectContext* directContext = GrAsDirectContext(canvas.recordingContext());
        if (directContext) {
            if (cachedFrameGpuImage_ && cachedFrameGpuContext_ != directContext) {
                cachedFrameGpuImage_.reset();
                cachedFrameGpuContext_ = nullptr;
            }
            if (!cachedFrameGpuImage_) {
                cachedFrameGpuImage_ = SkImages::TextureFromImage(directContext,
                                                                  cachedFrameImage_.get(),
                                                                  skgpu::Mipmapped::kNo,
                                                                  skgpu::Budgeted::kYes);
                cachedFrameGpuContext_ = cachedFrameGpuImage_ ? directContext : nullptr;
            }
            if (cachedFrameGpuImage_) {
                image = cachedFrameGpuImage_;
            }
        }
#endif
        canvas.drawImage(image, 0.0f, 0.0f);
        return true;
    }

    bool ensureCachedFrame(int width, int height, float dpiScale) {
        if (cacheMatches(width, height, dpiScale)) {
            return true;
        }
        if (buildingCachedFrame_) {
            return false;
        }

        buildingCachedFrame_ = true;
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff070c12u);
        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info,
                                                          pixels.data(),
                                                          static_cast<size_t>(width) * sizeof(uint32_t));
        if (surface) {
            drawUncached(*surface->getCanvas(), width, height, dpiScale, nullptr);
            updateCachedFrame(pixels.data(), width, height, dpiScale);
        }
        buildingCachedFrame_ = false;
        return cacheMatches(width, height, dpiScale);
    }

    void updateCachedFrame(const uint32_t* pixels, int width, int height, float dpiScale) {
        if (!pixels || width <= 0 || height <= 0) {
            return;
        }

        cachedFrameWidth_ = width;
        cachedFrameHeight_ = height;
        cachedFrameDpiScale_ = dpiScale;
        cachedFramePixels_.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height));

        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        cachedFrameImage_ = SkImages::RasterFromData(
            info,
            SkData::MakeWithCopy(cachedFramePixels_.data(),
                                 cachedFramePixels_.size() * sizeof(uint32_t)),
            static_cast<size_t>(width) * sizeof(uint32_t));
#if defined(SKIATEST_USE_SKIA_D3D) && SKIATEST_USE_SKIA_D3D
        cachedFrameGpuImage_.reset();
        cachedFrameGpuContext_ = nullptr;
#endif
    }

    void prepareFrame(int logicalWidth, int logicalHeight, Rml::ElementDocument*& document) {
        document = context_ ? context_->GetDocument(0) : nullptr;
        if (!context_) {
            return;
        }

        if (logicalWidth != logicalWidth_ || logicalHeight != logicalHeight_) {
            logicalWidth_ = logicalWidth;
            logicalHeight_ = logicalHeight;
            context_->SetDimensions({logicalWidth, logicalHeight});
            context_->Update();
        }
        document = context_->GetDocument(0);
    }

    void drawChrome(SkCanvas& canvas,
                    Rml::ElementDocument* document,
                    int logicalWidth,
                    int logicalHeight,
                    float scale,
                    RmlDrawTiming* timing) {
        canvas.clear(rgb(7, 12, 18));
        canvas.save();
        canvas.scale(scale, scale);
        chrome_.drawApp(canvas,
                        document,
                        static_cast<float>(logicalWidth),
                        static_cast<float>(logicalHeight),
                        browserPaintStyles_,
                        timing);
        canvas.restore();
    }

    void renderRmlOverlay(SkCanvas& canvas, int width, int height, float scale) {
        if (!context_) {
            return;
        }

        gRmlTextureScale = scale;
        font_.beginFrame(scale);
        render_.begin(canvas, width, height, scale);
        context_->Render();
        render_.end();
    }


    static std::filesystem::path executableDirectory() {
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD length = 0;
        while (true) {
            length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return {};
            }
            if (length < buffer.size() - 1) {
                break;
            }
            buffer.resize(buffer.size() * 2, L'\0');
        }
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }

    static std::string readTextFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return {};
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    static std::string fallbackRmlDocument() {
        return R"RML(
<rml>
<head>
<style>
body { margin: 0; background-color: transparent; }
.text { font-family: "Microsoft YaHei UI"; color: #eff7fd; font-size: 22px; }
</style>
</head>
<body>
<div class="text">Missing assets/rml_layer_desk.rml</div>
</body>
</rml>
)RML";
    }

    static std::string loadRmlDocument() {
        const std::filesystem::path relative = std::filesystem::path("assets") / "rml_layer_desk.rml";
        std::vector<std::filesystem::path> candidates;
        candidates.push_back(relative);

        const std::filesystem::path exeDir = executableDirectory();
        if (!exeDir.empty()) {
            candidates.push_back(exeDir.parent_path().parent_path() / relative);
            candidates.push_back(exeDir / relative);
            candidates.push_back(exeDir.parent_path() / relative);
        }

        for (const std::filesystem::path& candidate : candidates) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(candidate, error)) {
                continue;
            }
            std::string document = readTextFile(candidate);
            if (!document.empty()) {
                return document;
            }
        }

        return fallbackRmlDocument();
    }

    void loadDocument() {
        if (!context_) {
            return;
        }
        rmlDocument_ = loadRmlDocument();
        browserPaintStyles_ = parseBrowserPaintStyles(rmlDocument_);
        Rml::ElementDocument* document = context_->LoadDocumentFromMemory(rmlDocument_.c_str(), "rml_layer_desk.rml");
        if (document) {
            document->Show();
        }
    }

};

class AppWindow {
public:
    ~AppWindow() {
        if (backgroundBrush_) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
    }

    int run(HINSTANCE instance, int showCmd) {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
        dpi_ = getSystemDpi();
        const float dpiScale = dpiScaleForDpi(dpi_);
        backgroundBrush_ = CreateSolidBrush(kWin32Background);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance = instance;
        wc.lpfnWndProc = &AppWindow::WndProc;
        wc.lpszClassName = L"RmlLayerDeskWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = backgroundBrush_;
        RegisterClassExW(&wc);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        const int desiredClientWidth = static_cast<int>(std::round(kBaseWidth * dpiScale));
        const int desiredClientHeight = static_cast<int>(std::round(kBaseHeight * dpiScale));
        RECT desired{0, 0, desiredClientWidth, desiredClientHeight};
        adjustWindowRectForDpi(desired, dpi_);
        const int frameWidth = desired.right - desired.left;
        const int frameHeight = desired.bottom - desired.top;
        const float fit = std::min(1.0f, std::min((workWidth - 40.0f) / frameWidth, (workHeight - 40.0f) / frameHeight));
        const int clientWidth = static_cast<int>(std::round(desiredClientWidth * std::max(0.70f, fit)));
        const int clientHeight = static_cast<int>(std::round(desiredClientHeight * std::max(0.70f, fit)));
        RECT initialRect{0, 0, clientWidth, clientHeight};
        adjustWindowRectForDpi(initialRect, dpi_);
        const int windowWidth = initialRect.right - initialRect.left;
        const int windowHeight = initialRect.bottom - initialRect.top;
        const int x = workArea.left + (workWidth - windowWidth) / 2;
        const int y = workArea.top + (workHeight - windowHeight) / 2;

        HWND hwnd = CreateWindowExW(0,
                                    wc.lpszClassName,
                                    L"RmlLayerDesk",
                                    WS_OVERLAPPEDWINDOW,
                                    x,
                                    y,
                                    windowWidth,
                                    windowHeight,
                                    nullptr,
                                    nullptr,
                                    instance,
                                    this);
        if (!hwnd) {
            return 1;
        }

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        ShowWindow(hwnd, showCmd == SW_HIDE ? SW_SHOWNORMAL : showCmd);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    D3DPresenter d3d_{kWin32Background, true};
    RmlLayerRenderer renderer_;
    std::vector<uint32_t> fallbackPixels_;
    int fallbackWidth_ = 0;
    int fallbackHeight_ = 0;
    UINT dpi_ = static_cast<UINT>(kDefaultDpi);
    HBRUSH backgroundBrush_ = nullptr;
    bool paintActive_ = false;

    static float dpiScaleForDpi(UINT dpi) {
        return static_cast<float>(std::max<UINT>(dpi, 1)) / kDefaultDpi;
    }

    static UINT getSystemDpi() {
        const UINT dpi = GetDpiForSystem();
        return dpi != 0 ? dpi : static_cast<UINT>(kDefaultDpi);
    }

    static UINT getWindowDpi(HWND hwnd) {
        const UINT dpi = GetDpiForWindow(hwnd);
        return dpi != 0 ? dpi : getSystemDpi();
    }

    static void adjustWindowRectForDpi(RECT& rect, UINT dpi) {
        if (!AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
            AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
        }
    }

    static AppWindow* get(HWND hwnd) {
        return reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    void requestRepaint(HWND hwnd, bool immediate) {
        const UINT flags = RDW_INVALIDATE | RDW_NOCHILDREN |
                           (immediate ? (RDW_ERASE | RDW_UPDATENOW) : 0);
        RedrawWindow(hwnd, nullptr, nullptr, flags);
    }

    void eraseBackground(HWND hwnd, HDC hdc) {
        RECT client{};
        GetClientRect(hwnd, &client);
        if (backgroundBrush_) {
            FillRect(hdc, &client, backgroundBrush_);
            return;
        }

        SetDCBrushColor(hdc, kWin32Background);
        FillRect(hdc, &client, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        AppWindow* app = get(hwnd);
        if (!app) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        switch (message) {
        case WM_WINDOWPOSCHANGING: {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            if (pos && !(pos->flags & SWP_NOSIZE)) {
                pos->flags |= SWP_NOCOPYBITS;
            }
            break;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                return 0;
            }
            app->requestRepaint(hwnd, true);
            return 0;
        case WM_EXITSIZEMOVE:
            app->requestRepaint(hwnd, true);
            return 0;
        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app->requestRepaint(hwnd, true);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_PAINT:
            app->paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            app->eraseBackground(hwnd, reinterpret_cast<HDC>(wParam));
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool renderCpuPixels(uint32_t* pixels, int width, int height, size_t rowBytes) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!pixels || rowBytes < static_cast<size_t>(width) * sizeof(uint32_t)) {
            return false;
        }

        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
        if (!surface) {
            return false;
        }

        renderer_.draw(*surface->getCanvas(), width, height, dpiScaleForDpi(dpi_));
        return true;
    }

    bool renderFallbackPixels(int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (width != fallbackWidth_ || height != fallbackHeight_ || fallbackPixels_.empty()) {
            fallbackWidth_ = width;
            fallbackHeight_ = height;
            fallbackPixels_.resize(static_cast<size_t>(fallbackWidth_) * static_cast<size_t>(fallbackHeight_));
        }

        return renderCpuPixels(fallbackPixels_.data(),
                               fallbackWidth_,
                               fallbackHeight_,
                               static_cast<size_t>(fallbackWidth_) * sizeof(uint32_t));
    }

    bool renderD3D(HWND hwnd, int width, int height) {
        return d3d_.render(
            hwnd,
            width,
            height,
            [this](SkCanvas& canvas, int drawWidth, int drawHeight) {
                // Ganesh already provides a GPU-backed canvas. Rebuilding the raster
                // cache here makes every new resize step pay the CPU full-frame cost.
                renderer_.drawUncached(canvas, drawWidth, drawHeight, dpiScaleForDpi(dpi_), nullptr);
            },
            [this](uint32_t* pixels, int drawWidth, int drawHeight, size_t rowBytes) {
                return renderCpuPixels(pixels, drawWidth, drawHeight, rowBytes);
            });
    }

    void renderGdiFallback(HDC hdc, const PAINTSTRUCT& ps, int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!renderFallbackPixels(width, height)) {
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = fallbackWidth_;
        bmi.bmiHeader.biHeight = -fallbackHeight_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        const int paintX = std::max<LONG>(0, ps.rcPaint.left);
        const int paintY = std::max<LONG>(0, ps.rcPaint.top);
        const int paintRight = std::min<LONG>(fallbackWidth_, ps.rcPaint.right);
        const int paintBottom = std::min<LONG>(fallbackHeight_, ps.rcPaint.bottom);
        const int paintWidth = paintRight - paintX;
        const int paintHeight = paintBottom - paintY;
        if (paintWidth > 0 && paintHeight > 0) {
            StretchDIBits(hdc,
                          paintX,
                          paintY,
                          paintWidth,
                          paintHeight,
                          paintX,
                          paintY,
                          paintWidth,
                          paintHeight,
                          fallbackPixels_.data(),
                          &bmi,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        }
    }

    void paint(HWND hwnd) {
        if (paintActive_) {
            ValidateRect(hwnd, nullptr);
            return;
        }
        paintActive_ = true;

        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        dpi_ = getWindowDpi(hwnd);
        if (!renderD3D(hwnd, width, height)) {
            renderGdiFallback(hdc, ps, width, height);
        }

        EndPaint(hwnd, &ps);
        paintActive_ = false;
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i + 4 < argc; ++i) {
            if (wcscmp(argv[i], L"--bench-draw") == 0) {
                const int width = std::max(1, _wtoi(argv[i + 1]));
                const int height = std::max(1, _wtoi(argv[i + 2]));
                const float dpiScale = std::max(0.1f, static_cast<float>(_wtof(argv[i + 3])));
                const int frames = std::max(1, _wtoi(argv[i + 4]));
                RmlLayerRenderer renderer;
                RmlDrawTiming timing;
                RmlBenchCounters counters;
                const double averageMs = renderer.benchDraw(width, height, dpiScale, frames, &timing, &counters);
                const double cachedAverageMs = renderer.benchCachedDraw(width, height, dpiScale, frames);
                char message[1024]{};
                std::snprintf(message,
                              sizeof(message),
                              "RmlLayerDesk bench-draw: %dx%d dpi=%.2f frames=%d uncached=%.3fms cached=%.3fms "
                              "prepare=%.3fms chrome=%.3fms rml=%.3fms "
                              "chrome_bg=%.3fms chrome_sidebar=%.3fms chrome_panel=%.3fms "
                              "chrome_compass=%.3fms chrome_status=%.3fms chrome_frame=%.3fms "
                              "strings/frame=%.2f compile/frame=%.2f render/frame=%.2f textures/frame=%.2f texture_kb/frame=%.1f\n",
                              width,
                              height,
                              dpiScale,
                              frames,
                              averageMs,
                              cachedAverageMs,
                              timing.prepareMs / static_cast<double>(frames),
                              timing.chromeMs / static_cast<double>(frames),
                              timing.rmlMs / static_cast<double>(frames),
                              timing.chromeBackgroundMs / static_cast<double>(frames),
                              timing.chromeSidebarMs / static_cast<double>(frames),
                              timing.chromePanelMs / static_cast<double>(frames),
                              timing.chromeCompassMs / static_cast<double>(frames),
                              timing.chromeStatusMs / static_cast<double>(frames),
                              timing.chromeFrameMs / static_cast<double>(frames),
                              static_cast<double>(counters.generatedStrings) / static_cast<double>(frames),
                              static_cast<double>(counters.compiledGeometries) / static_cast<double>(frames),
                              static_cast<double>(counters.renderedGeometries) / static_cast<double>(frames),
                              static_cast<double>(counters.generatedTextures) / static_cast<double>(frames),
                              static_cast<double>(counters.generatedTextureBytes) /
                                  (1024.0 * static_cast<double>(frames)));
                OutputDebugStringA(message);
                if (i + 5 < argc) {
                    FILE* file = nullptr;
                    if (_wfopen_s(&file, argv[i + 5], L"wb") == 0 && file) {
                        std::fprintf(file, "%s", message);
                        std::fclose(file);
                    }
                }
                LocalFree(argv);
                return EXIT_SUCCESS;
            }
        }
        for (int i = 1; i + 4 < argc; ++i) {
            if (wcscmp(argv[i], L"--dump-size") == 0) {
                const int width = std::max(1, _wtoi(argv[i + 1]));
                const int height = std::max(1, _wtoi(argv[i + 2]));
                const float dpiScale = std::max(0.1f, static_cast<float>(_wtof(argv[i + 3])));
                RmlLayerRenderer renderer;
                const bool ok = renderer.savePng(argv[i + 4], width, height, dpiScale);
                LocalFree(argv);
                return ok ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
        for (int i = 1; i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--dump") == 0) {
                RmlLayerRenderer renderer;
                const bool ok = renderer.savePng(argv[i + 1]);
                LocalFree(argv);
                return ok ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
        LocalFree(argv);
    }

    AppWindow app;
    return app.run(instance, showCmd);
}
