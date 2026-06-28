#include "skui_internal.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkShader.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkTypeface_win.h"
#include "include/utils/SkParsePath.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace skui {
namespace {

std::vector<SkColor4f> gradientColors(const std::vector<SkColor>& colors) {
    std::vector<SkColor4f> stops;
    stops.reserve(colors.size());
    for (SkColor color : colors) {
        stops.push_back(SkColor4f::FromColor(color));
    }
    return stops;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::string normalizeSvgMarkup(std::string svg) {
    const size_t svgPos = svg.find("<svg");
    if (svgPos != std::string::npos && svg.find("xmlns=", svgPos) == std::string::npos) {
        svg.insert(svgPos + 4, " xmlns=\"http://www.w3.org/2000/svg\"");
    }
    return svg;
}

std::optional<float> parseSvgFloat(std::string_view raw) {
    std::string value = trim(raw);
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.ends_with("px")) {
        value.resize(value.size() - 2);
    }
    float out = 0.0f;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return out;
}

std::vector<float> parseFloatList(std::string_view raw) {
    std::vector<float> values;
    size_t start = 0;
    while (start < raw.size()) {
        while (start < raw.size() && (std::isspace(static_cast<unsigned char>(raw[start])) || raw[start] == ',')) {
            ++start;
        }
        size_t end = start;
        while (end < raw.size() && !std::isspace(static_cast<unsigned char>(raw[end])) && raw[end] != ',') {
            ++end;
        }
        if (end > start) {
            if (std::optional<float> value = parseSvgFloat(raw.substr(start, end - start))) {
                values.push_back(*value);
            }
        }
        start = end;
    }
    return values;
}

std::string attrValue(std::string_view tag, std::string_view name) {
    size_t pos = 0;
    while (pos < tag.size()) {
        pos = tag.find(name, pos);
        if (pos == std::string_view::npos) {
            return {};
        }
        const bool leftOk = pos == 0 || std::isspace(static_cast<unsigned char>(tag[pos - 1])) || tag[pos - 1] == '<';
        const size_t afterName = pos + name.size();
        if (!leftOk || afterName >= tag.size() || tag[afterName] != '=') {
            pos = afterName;
            continue;
        }
        size_t valueStart = afterName + 1;
        if (valueStart >= tag.size()) {
            return {};
        }
        const char quote = tag[valueStart];
        if (quote == '"' || quote == '\'') {
            ++valueStart;
            const size_t valueEnd = tag.find(quote, valueStart);
            if (valueEnd == std::string_view::npos) {
                return {};
            }
            return std::string(tag.substr(valueStart, valueEnd - valueStart));
        }
        size_t valueEnd = valueStart;
        while (valueEnd < tag.size() && !std::isspace(static_cast<unsigned char>(tag[valueEnd])) && tag[valueEnd] != '>') {
            ++valueEnd;
        }
        return std::string(tag.substr(valueStart, valueEnd - valueStart));
    }
    return {};
}

bool hasAttr(std::string_view tag, std::string_view name) {
    size_t pos = 0;
    while (pos < tag.size()) {
        pos = tag.find(name, pos);
        if (pos == std::string_view::npos) {
            return false;
        }
        const bool leftOk = pos == 0 || std::isspace(static_cast<unsigned char>(tag[pos - 1])) || tag[pos - 1] == '<';
        const size_t afterName = pos + name.size();
        if (leftOk && afterName < tag.size() && tag[afterName] == '=') {
            return true;
        }
        pos = afterName;
    }
    return false;
}

bool isCurrentColor(std::string_view raw) {
    std::string value = trim(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "currentcolor";
}

bool isUtf8ContinuationByte(unsigned char ch) {
    return (ch & 0xC0) == 0x80;
}

size_t nextUtf8Boundary(std::string_view value, size_t index) {
    if (index >= value.size()) {
        return value.size();
    }
    ++index;
    while (index < value.size() && isUtf8ContinuationByte(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

std::vector<std::string_view> tagsNamed(std::string_view svg, std::string_view name) {
    std::vector<std::string_view> tags;
    std::string open = "<" + std::string(name);
    size_t pos = 0;
    while ((pos = svg.find(open, pos)) != std::string_view::npos) {
        const size_t end = svg.find('>', pos + open.size());
        if (end == std::string_view::npos) {
            break;
        }
        tags.push_back(svg.substr(pos, end - pos + 1));
        pos = end + 1;
    }
    return tags;
}

SkColor colorWithOpacity(SkColor color, float opacity) {
    opacity = clampf(opacity, 0.0f, 1.0f);
    return SkColorSetA(color, static_cast<U8CPU>(static_cast<float>(SkColorGetA(color)) * opacity));
}

std::optional<SkPaint> svgPaint(std::string_view tag,
                                std::string_view name,
                                SkPaint::Style style,
                                float defaultStrokeWidth,
                                std::optional<SkColor> inheritedColor = std::nullopt,
                                float inheritedOpacity = 1.0f,
                                std::optional<SkColor> currentColor = std::nullopt,
                                bool defaultFill = true) {
    const std::string raw = attrValue(tag, name);
    if (raw == "none") {
        return std::nullopt;
    }
    SkColor color = SK_ColorTRANSPARENT;
    if (isCurrentColor(raw)) {
        if (!currentColor) {
            return std::nullopt;
        }
        color = *currentColor;
    } else if (!raw.empty()) {
        color = parseColor(raw, SK_ColorTRANSPARENT);
    } else if (inheritedColor) {
        color = *inheritedColor;
    } else if (style == SkPaint::kFill_Style && defaultFill) {
        color = SK_ColorBLACK;
    } else {
        return std::nullopt;
    }
    if (SkColorGetA(color) == 0) {
        return std::nullopt;
    }

    float opacity = inheritedOpacity;
    if (const std::string rawOpacity = attrValue(tag, std::string(name) + "-opacity"); !rawOpacity.empty()) {
        opacity = parseSvgFloat(rawOpacity).value_or(1.0f);
    }
    if (const std::string rawOpacity = attrValue(tag, "opacity"); !rawOpacity.empty()) {
        opacity *= parseSvgFloat(rawOpacity).value_or(1.0f);
    }

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(style);
    paint.setColor(colorWithOpacity(color, opacity));
    if (style == SkPaint::kStroke_Style) {
        paint.setStrokeWidth(parseSvgFloat(attrValue(tag, "stroke-width")).value_or(defaultStrokeWidth));
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeJoin(SkPaint::kRound_Join);
    }
    return paint;
}

std::optional<SkColor> svgColorAttr(std::string_view tag, std::string_view name) {
    const std::string raw = attrValue(tag, name);
    if (raw.empty() || raw == "none") {
        return std::nullopt;
    }
    const SkColor color = parseColor(raw, SK_ColorTRANSPARENT);
    if (SkColorGetA(color) == 0) {
        return std::nullopt;
    }
    return color;
}

float svgOpacityAttr(std::string_view tag, std::string_view name, float fallback) {
    const std::string raw = attrValue(tag, name);
    if (raw.empty()) {
        return fallback;
    }
    return parseSvgFloat(raw).value_or(fallback);
}

}  // namespace

SkiaRenderer::SkiaRenderer(RuntimeOptions options) : options_(std::move(options)) {
    fontMgr_ = SkFontMgr_New_DirectWrite();
    if (!fontMgr_) {
        fontMgr_ = SkFontMgr_New_GDI();
    }
    regular_ = pickTypeface(false);
    bold_ = pickTypeface(true);
}

SkiaRenderer::~SkiaRenderer() {
    clearCaches();
}

void SkiaRenderer::clearCaches() {
    svgFileCache_.clear();
    parsedSvgCache_.clear();
    textCache_.clear();
}

sk_sp<SkTypeface> SkiaRenderer::pickTypeface(bool bold) {
    const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
    const std::array<const char*, 5> families = {"Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", "Arial", nullptr};
    for (const char* family : families) {
        if (!fontMgr_) {
            continue;
        }
        sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(family, style);
        if (typeface) {
            return typeface;
        }
    }
    return nullptr;
}

SkFont SkiaRenderer::font(float size, bool bold) const {
    SkFont f(bold ? bold_ : regular_, size);
    f.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    f.setHinting(SkFontHinting::kNormal);
    f.setSubpixel(true);
    return f;
}

SkPaint SkiaRenderer::fill(SkColor color) const {
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kFill_Style);
    p.setColor(color);
    return p;
}

SkPaint SkiaRenderer::stroke(SkColor color, float width) const {
    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setStrokeJoin(SkPaint::kRound_Join);
    p.setColor(color);
    return p;
}

SkPaint SkiaRenderer::backgroundPaint(const Node& node) const {
    if (node.style.backgroundGradient.kind == GradientKind::None || node.style.backgroundGradient.colors.empty()) {
        return fill(node.style.backgroundColor);
    }

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kFill_Style);
    std::vector<SkColor4f> colors = gradientColors(node.style.backgroundGradient.colors);
    const SkGradient gradient(SkGradient::Colors(colors, {}, SkTileMode::kClamp), {});
    const Rect r = node.layout;
    if (node.style.backgroundGradient.kind == GradientKind::Radial) {
        p.setShader(SkShaders::RadialGradient(SkPoint::Make(r.x + r.w * 0.5f, r.y + r.h * 0.5f),
                                              std::max(r.w, r.h) * 0.62f,
                                              gradient));
    } else {
        const SkPoint points[2] = {
            SkPoint::Make(r.x, r.y),
            node.style.backgroundGradient.kind == GradientKind::LinearY
                ? SkPoint::Make(r.x, r.y + r.h)
                : SkPoint::Make(r.x + r.w, r.y),
        };
        p.setShader(SkShaders::LinearGradient(points, gradient));
    }
    return p;
}

void SkiaRenderer::draw(Document& document, SkCanvas& canvas, int width, int height, float dpiScale) {
    canvas.clear(options_.clearColor);
    const float scale = std::max(0.1f, dpiScale);
    canvas.save();
    canvas.scale(scale, scale);
    if (document.root) {
        drawNode(canvas, document, *document.root);
    }
    canvas.restore();
}

void SkiaRenderer::drawNode(SkCanvas& canvas, const Document& document, const Node& node) {
    if (node.style.display == Display::None) {
        return;
    }

    drawBox(canvas, node);
    drawImage(canvas, document, node);
    drawInlineSvg(canvas, node);
    drawInputSelection(canvas, node);
    drawText(canvas, node);
    drawInputCompositionUnderline(canvas, node);
    drawInputCaret(canvas, node);
    for (const auto& child : node.children) {
        drawNode(canvas, document, *child);
    }
}

void SkiaRenderer::drawBox(SkCanvas& canvas, const Node& node) {
    const Rect r = node.layout;
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }

    if (SkColorGetA(node.style.backgroundColor) > 0 || node.style.backgroundGradient.kind != GradientKind::None) {
        const float radius = node.style.borderRadius;
        if (radius > 0.0f) {
            canvas.drawRRect(SkRRect::MakeRectXY(r.sk(), radius, radius), backgroundPaint(node));
        } else {
            canvas.drawRect(r.sk(), backgroundPaint(node));
        }
    }

    const SkColor borderColor = node.style.flags.borderColor ? node.style.borderColor : node.style.color;
    if (node.style.borderStyle == BorderStyle::Solid &&
        node.style.borderWidth > 0.0f &&
        SkColorGetA(borderColor) > 0) {
        const float half = node.style.borderWidth * 0.5f;
        const SkRect border = SkRect::MakeXYWH(r.x + half, r.y + half, std::max(0.0f, r.w - node.style.borderWidth), std::max(0.0f, r.h - node.style.borderWidth));
        if (node.style.borderRadius > 0.0f) {
            canvas.drawRRect(SkRRect::MakeRectXY(border, node.style.borderRadius, node.style.borderRadius),
                             stroke(borderColor, node.style.borderWidth));
        } else {
            canvas.drawRect(border, stroke(borderColor, node.style.borderWidth));
        }
    }
}

void SkiaRenderer::drawImage(SkCanvas& canvas, const Document& document, const Node& node) {
    if (node.tag != "img" || node.src.empty()) {
        return;
    }

    const std::optional<std::string> svg = readSvgAsset(document, node.src);
    if (!svg || svg->empty()) {
        return;
    }
    drawSvgMarkup(canvas, *svg, node.layout, node.style.color);
}

void SkiaRenderer::drawInlineSvg(SkCanvas& canvas, const Node& node) {
    if (node.tag != "svg" || node.svgMarkup.empty()) {
        return;
    }
    drawSvgMarkup(canvas, node.svgMarkup, node.layout, node.style.color);
}

void SkiaRenderer::drawSvgMarkup(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor) {
    if (svg.empty() || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    const ParsedSvg& parsed = parsedSvg(svg, currentColor);
    if (parsed.shapes.empty() || parsed.viewWidth <= 0.0f || parsed.viewHeight <= 0.0f) {
        return;
    }

    const float scale = std::min(rect.w / parsed.viewWidth, rect.h / parsed.viewHeight);
    const float drawWidth = parsed.viewWidth * scale;
    const float drawHeight = parsed.viewHeight * scale;

    canvas.save();
    canvas.translate(rect.x + (rect.w - drawWidth) * 0.5f, rect.y + (rect.h - drawHeight) * 0.5f);
    canvas.scale(scale, scale);
    canvas.translate(-parsed.viewX, -parsed.viewY);
    for (const SvgShape& shape : parsed.shapes) {
        if (shape.fill) {
            canvas.drawPath(shape.path, *shape.fill);
        }
        if (shape.stroke) {
            canvas.drawPath(shape.path, *shape.stroke);
        }
    }
    canvas.restore();
}

void SkiaRenderer::drawInputSelection(SkCanvas& canvas, const Node& node) {
    if (node.tag != "input" || !node.focused || node.selectionStart == node.selectionEnd || node.value.empty()) {
        return;
    }

    const size_t start = std::min(node.selectionStart, node.value.size());
    const size_t end = std::min(node.selectionEnd, node.value.size());
    if (start >= end) {
        return;
    }

    const float before = start == 0 ? 0.0f : textWidth(std::string_view(node.value.data(), start), node.style.fontSize, node.style.fontBold);
    const float selected = textWidth(std::string_view(node.value.data() + start, end - start), node.style.fontSize, node.style.fontBold);
    const float x = std::max(node.layout.x, node.layout.x + before);
    const float right = std::min(node.layout.x + node.layout.w, x + selected);
    if (right <= x) {
        return;
    }

    const float selectionHeight = std::max(12.0f, node.style.fontSize * 1.35f);
    const float y = node.layout.y + (node.layout.h - selectionHeight) * 0.5f;
    SkPaint p = fill(SkColorSetARGB(96, 36, 232, 219));
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    canvas.drawRect(SkRect::MakeXYWH(x, y, right - x, selectionHeight), p);
    canvas.restore();
}

void SkiaRenderer::drawText(SkCanvas& canvas, const Node& node) {
    const bool hasComposition = node.tag == "input" && !node.compositionText.empty();
    const bool inputPlaceholder = node.tag == "input" && node.value.empty() && !hasComposition && !node.placeholder.empty();
    std::string value;
    if (inputPlaceholder) {
        value = node.placeholder;
    } else if (hasComposition) {
        const size_t cursor = std::min(node.cursorIndex, node.value.size());
        value = node.value.substr(0, cursor);
        value += node.compositionText;
        value += node.value.substr(cursor);
    } else {
        value = !node.value.empty() ? node.value : node.text;
    }
    if (value.empty()) {
        return;
    }

    const TextEntry& entry = textEntry(value, node.style.fontSize, node.style.fontBold);
    const float availableWidth = std::max(0.0f, node.layout.w);
    float x = node.layout.x;
    if (node.style.justifyContent == YGJustifyCenter) {
        x = node.layout.x + (availableWidth - entry.width) * 0.5f;
    } else if (node.style.justifyContent == YGJustifyFlexEnd) {
        x = node.layout.x + availableWidth - entry.width;
    }
    x = std::max(node.layout.x, x);
    const float y = node.layout.y + node.layout.h * 0.5f - (entry.bounds.fTop + entry.bounds.fBottom) * 0.5f;
    SkColor textColor = node.style.color;
    if (inputPlaceholder) {
        textColor = SkColorSetA(textColor, static_cast<U8CPU>(std::min(150, static_cast<int>(SkColorGetA(textColor)))));
    }
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    canvas.drawTextBlob(entry.blob, x, y, fill(textColor));
    canvas.restore();
}

void SkiaRenderer::drawInputCompositionUnderline(SkCanvas& canvas, const Node& node) {
    if (node.tag != "input" || node.compositionText.empty()) {
        return;
    }

    const size_t cursor = std::min(node.cursorIndex, node.value.size());
    const float before = cursor == 0 ? 0.0f : textWidth(std::string_view(node.value.data(), cursor), node.style.fontSize, node.style.fontBold);
    const float width = textWidth(node.compositionText, node.style.fontSize, node.style.fontBold);
    const float x = node.layout.x + before;
    const float right = std::min(node.layout.x + node.layout.w, x + width);
    if (right <= x) {
        return;
    }
    const float y = node.layout.y + node.layout.h * 0.5f + node.style.fontSize * 0.58f;
    SkPaint p = stroke(node.style.color, 1.0f);
    p.setStrokeCap(SkPaint::kButt_Cap);
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    canvas.drawLine(x, y, right, y, p);
    canvas.restore();
}

void SkiaRenderer::drawInputCaret(SkCanvas& canvas, const Node& node) {
    if (node.tag != "input" || !node.focused || node.selectionStart != node.selectionEnd) {
        return;
    }

    const size_t cursor = std::min(node.cursorIndex, node.value.size());
    std::string_view before(node.value.data(), cursor);
    float caretOffset = before.empty() ? 0.0f : textWidth(before, node.style.fontSize, node.style.fontBold);
    if (!node.compositionText.empty()) {
        caretOffset += textWidth(node.compositionText, node.style.fontSize, node.style.fontBold);
    }
    const float x = std::min(node.layout.x + node.layout.w - 1.0f, node.layout.x + caretOffset);
    const float caretHeight = std::max(12.0f, node.style.fontSize * 1.18f);
    const float y0 = node.layout.y + (node.layout.h - caretHeight) * 0.5f;
    const float y1 = y0 + caretHeight;

    SkPaint p = stroke(node.style.color, 1.25f);
    p.setStrokeCap(SkPaint::kButt_Cap);
    canvas.drawLine(x, y0, x, y1, p);
}

SkiaRenderer::ParsedSvg SkiaRenderer::parseSvg(std::string_view svg, SkColor currentColor) const {
    ParsedSvg parsed;
    std::optional<SkColor> inheritedFill;
    std::optional<SkColor> inheritedStroke;
    float inheritedFillOpacity = 1.0f;
    float inheritedStrokeOpacity = 1.0f;
    bool defaultFill = true;
    if (const size_t svgPos = svg.find("<svg"); svgPos != std::string_view::npos) {
        if (const size_t svgEnd = svg.find('>', svgPos); svgEnd != std::string_view::npos) {
            const std::string_view svgTag = svg.substr(svgPos, svgEnd - svgPos + 1);
            const std::string viewBox = attrValue(svgTag, "viewBox");
            const std::vector<float> values = parseFloatList(viewBox);
            if (values.size() == 4 && values[2] > 0.0f && values[3] > 0.0f) {
                parsed.viewX = values[0];
                parsed.viewY = values[1];
                parsed.viewWidth = values[2];
                parsed.viewHeight = values[3];
            }
            if (isCurrentColor(attrValue(svgTag, "fill"))) {
                inheritedFill = currentColor;
            } else {
                inheritedFill = svgColorAttr(svgTag, "fill");
            }
            if (isCurrentColor(attrValue(svgTag, "stroke"))) {
                inheritedStroke = currentColor;
            } else {
                inheritedStroke = svgColorAttr(svgTag, "stroke");
            }
            if (hasAttr(svgTag, "fill") && !inheritedFill) {
                defaultFill = false;
            }
            inheritedFillOpacity = svgOpacityAttr(svgTag, "fill-opacity", 1.0f) *
                                   svgOpacityAttr(svgTag, "opacity", 1.0f);
            inheritedStrokeOpacity = svgOpacityAttr(svgTag, "stroke-opacity", 1.0f) *
                                     svgOpacityAttr(svgTag, "opacity", 1.0f);
        }
    }

    for (std::string_view pathTag : tagsNamed(svg, "path")) {
        const std::string d = attrValue(pathTag, "d");
        if (d.empty()) {
            continue;
        }
        std::optional<SkPath> path = SkParsePath::FromSVGString(d.c_str());
        if (!path) {
            continue;
        }
        SvgShape shape;
        shape.path = *path;
        shape.fill = svgPaint(pathTag, "fill", SkPaint::kFill_Style, 1.0f, inheritedFill, inheritedFillOpacity, currentColor, defaultFill);
        shape.stroke = svgPaint(pathTag, "stroke", SkPaint::kStroke_Style, 1.0f, inheritedStroke, inheritedStrokeOpacity, currentColor);
        if (shape.fill || shape.stroke) {
            parsed.shapes.push_back(std::move(shape));
        }
    }

    for (std::string_view circleTag : tagsNamed(svg, "circle")) {
        const float cx = parseSvgFloat(attrValue(circleTag, "cx")).value_or(0.0f);
        const float cy = parseSvgFloat(attrValue(circleTag, "cy")).value_or(0.0f);
        const float r = parseSvgFloat(attrValue(circleTag, "r")).value_or(0.0f);
        if (r <= 0.0f) {
            continue;
        }
        SvgShape shape;
        shape.path = SkPathBuilder().addCircle(cx, cy, r).detach();
        shape.fill = svgPaint(circleTag, "fill", SkPaint::kFill_Style, 1.0f, inheritedFill, inheritedFillOpacity, currentColor, defaultFill);
        shape.stroke = svgPaint(circleTag, "stroke", SkPaint::kStroke_Style, 1.0f, inheritedStroke, inheritedStrokeOpacity, currentColor);
        if (shape.fill || shape.stroke) {
            parsed.shapes.push_back(std::move(shape));
        }
    }

    return parsed;
}

const SkiaRenderer::ParsedSvg& SkiaRenderer::parsedSvg(std::string_view svg, SkColor currentColor) {
    std::string key = normalizeSvgMarkup(std::string(svg));
    if (key.find("currentColor") != std::string::npos || key.find("currentcolor") != std::string::npos ||
        key.find("CURRENTCOLOR") != std::string::npos) {
        char colorKey[16];
        std::snprintf(colorKey, sizeof(colorKey), "|%08X", currentColor);
        key += colorKey;
    }
    auto it = parsedSvgCache_.find(key);
    if (it != parsedSvgCache_.end()) {
        return it->second;
    }
    ParsedSvg parsed = parseSvg(key, currentColor);
    it = parsedSvgCache_.emplace(std::move(key), std::move(parsed)).first;
    return it->second;
}

std::optional<std::string> SkiaRenderer::readSvgAsset(const Document& document, std::string_view src) {
    const std::string path = resolveAssetPath(document, src);
    if (path.empty()) {
        return std::nullopt;
    }

    auto it = svgFileCache_.find(path);
    if (it != svgFileCache_.end()) {
        return it->second;
    }

    std::string svg = readTextFile(std::filesystem::path(path));
    if (svg.empty()) {
        return std::nullopt;
    }
    svg = normalizeSvgMarkup(std::move(svg));
    it = svgFileCache_.emplace(path, std::move(svg)).first;
    return it->second;
}

std::string SkiaRenderer::resolveAssetPath(const Document& document, std::string_view src) const {
    namespace fs = std::filesystem;
    if (src.empty()) {
        return {};
    }

    fs::path path{std::string(src)};
    if (path.is_absolute()) {
        return path.string();
    }

    if (!document.basePath.empty()) {
        fs::path candidate = fs::path(document.basePath) / path;
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }

    if (!options_.assetRoot.empty()) {
        fs::path candidate = fs::path(options_.assetRoot) / path;
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }

    return path.string();
}

const SkiaRenderer::TextEntry& SkiaRenderer::textEntry(std::string_view value, float size, bool bold) {
    std::string key;
    key.reserve(value.size() + 32);
    key += bold ? "1|" : "0|";
    char sizeBuf[24];
    std::snprintf(sizeBuf, sizeof(sizeBuf), "%.2f|", size);
    key += sizeBuf;
    key.append(value.data(), value.size());

    auto it = textCache_.find(key);
    if (it != textCache_.end()) {
        return it->second;
    }

    const SkFont f = font(size, bold);
    TextEntry entry;
    entry.width = f.measureText(value.data(), value.size(), SkTextEncoding::kUTF8, &entry.bounds);
    entry.blob = SkTextBlob::MakeFromText(value.data(), value.size(), f, SkTextEncoding::kUTF8);
    it = textCache_.emplace(std::move(key), std::move(entry)).first;
    return it->second;
}

float SkiaRenderer::textWidth(std::string_view value, float size, bool bold) {
    return textEntry(value, size, bold).width;
}

size_t SkiaRenderer::textIndexAtOffset(std::string_view value, float size, bool bold, float offset) {
    if (value.empty() || offset <= 0.0f) {
        return 0;
    }

    size_t previousIndex = 0;
    float previousWidth = 0.0f;
    for (size_t index = nextUtf8Boundary(value, 0); index <= value.size(); index = nextUtf8Boundary(value, index)) {
        const float width = textWidth(value.substr(0, index), size, bold);
        const float midpoint = (previousWidth + width) * 0.5f;
        if (offset < midpoint) {
            return previousIndex;
        }
        previousIndex = index;
        previousWidth = width;
        if (index == value.size()) {
            break;
        }
    }
    return value.size();
}

}  // namespace skui
