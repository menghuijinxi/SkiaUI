#include "skui_internal.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkTypeface_win.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace skui {
namespace {

SkColor withAlpha(SkColor color, unsigned alpha) {
    return SkColorSetARGB(static_cast<uint8_t>(std::min(alpha, 255u)),
                          SkColorGetR(color),
                          SkColorGetG(color),
                          SkColorGetB(color));
}

std::string svgHex(SkColor color) {
    char buf[16];
    std::snprintf(buf,
                  sizeof(buf),
                  "#%02X%02X%02X",
                  SkColorGetR(color),
                  SkColorGetG(color),
                  SkColorGetB(color));
    return buf;
}

std::string svgOpacity(SkColor color) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<float>(SkColorGetA(color)) / 255.0f);
    return buf;
}

std::string strokeAttrs(SkColor color, float width) {
    char widthBuf[24];
    std::snprintf(widthBuf, sizeof(widthBuf), "%.2f", width);
    return "stroke=\"" + svgHex(color) + "\" stroke-opacity=\"" + svgOpacity(color) +
           "\" stroke-width=\"" + widthBuf + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
}

std::string fillAttrs(SkColor color) {
    return "fill=\"" + svgHex(color) + "\" fill-opacity=\"" + svgOpacity(color) + "\"";
}

std::string wrapSvg(std::string_view body, const std::string& attrs) {
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" ";
    svg += attrs;
    svg += ">";
    svg += body;
    svg += "</svg>";
    return svg;
}

std::vector<SkColor4f> gradientColors(const std::vector<SkColor>& colors) {
    std::vector<SkColor4f> stops;
    stops.reserve(colors.size());
    for (SkColor color : colors) {
        stops.push_back(SkColor4f::FromColor(color));
    }
    return stops;
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
        drawNode(canvas, *document.root);
    }
    canvas.restore();
}

void SkiaRenderer::drawNode(SkCanvas& canvas, const Node& node) {
    if (node.style.display == Display::None) {
        return;
    }

    drawBox(canvas, node);
    drawIcon(canvas, node);
    drawText(canvas, node);
    for (const auto& child : node.children) {
        drawNode(canvas, *child);
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

    if (node.style.borderWidth > 0.0f && SkColorGetA(node.style.borderColor) > 0) {
        const float half = node.style.borderWidth * 0.5f;
        const SkRect border = SkRect::MakeXYWH(r.x + half, r.y + half, std::max(0.0f, r.w - node.style.borderWidth), std::max(0.0f, r.h - node.style.borderWidth));
        if (node.style.borderRadius > 0.0f) {
            canvas.drawRRect(SkRRect::MakeRectXY(border, node.style.borderRadius, node.style.borderRadius),
                             stroke(node.style.borderColor, node.style.borderWidth));
        } else {
            canvas.drawRect(border, stroke(node.style.borderColor, node.style.borderWidth));
        }
    }
}

void SkiaRenderer::drawText(SkCanvas& canvas, const Node& node) {
    const std::string value = !node.value.empty() ? node.value : node.text;
    if (value.empty()) {
        return;
    }

    const TextEntry& entry = textEntry(value, node.style.fontSize, node.style.fontBold);
    const float iconOffset = node.style.icon.empty() ? 0.0f : node.style.iconSize + 8.0f;
    const bool drawOwnInlineContent = node.children.empty();
    const float contentOffset = drawOwnInlineContent ? iconOffset : 0.0f;
    const float availableWidth = std::max(0.0f, node.layout.w - contentOffset);
    float x = node.layout.x + contentOffset;
    if (node.style.justifyContent == YGJustifyCenter) {
        x = node.layout.x + contentOffset + (availableWidth - entry.width) * 0.5f;
    } else if (node.style.justifyContent == YGJustifyFlexEnd) {
        x = node.layout.x + contentOffset + availableWidth - entry.width;
    }
    x = std::max(node.layout.x, x);
    const float y = node.layout.y + node.layout.h * 0.5f - (entry.bounds.fTop + entry.bounds.fBottom) * 0.5f;
    canvas.drawTextBlob(entry.blob, x, y, fill(node.style.color));
}

void SkiaRenderer::drawIcon(SkCanvas& canvas, const Node& node) {
    const std::string icon = node.style.icon.empty() ? node.icon : node.style.icon;
    if (icon.empty()) {
        return;
    }

    const float size = std::max(1.0f, node.style.iconSize);
    float cx = node.layout.x + size * 0.5f;
    float cy = node.layout.y + node.layout.h * 0.5f;
    if (node.text.empty() && node.value.empty() && node.layout.w > size) {
        cx = node.layout.x + node.layout.w * 0.5f;
    }
    drawSvgIcon(canvas, iconSvg(node), cx, cy, size);
}

void SkiaRenderer::drawSvgIcon(SkCanvas& canvas, const std::string& svg, float cx, float cy, float size) {
    auto it = svgCache_.find(svg);
    if (it == svgCache_.end()) {
        SkMemoryStream stream(svg.data(), svg.size(), false);
        sk_sp<SkSVGDOM> dom = SkSVGDOM::MakeFromStream(stream);
        if (!dom) {
            return;
        }
        dom->setContainerSize(SkSize::Make(24.0f, 24.0f));
        it = svgCache_.emplace(svg, std::move(dom)).first;
    }

    canvas.save();
    canvas.translate(cx - size * 0.5f, cy - size * 0.5f);
    canvas.scale(size / 24.0f, size / 24.0f);
    it->second->render(&canvas);
    canvas.restore();
}

std::string SkiaRenderer::iconSvg(const Node& node) const {
    const std::string icon = node.style.icon.empty() ? node.icon : node.style.icon;
    const SkColor color = node.style.iconColor;
    if (icon == "layers") {
        std::string body;
        body += "<path d=\"M12 3.2 4.2 7.2 12 11.2l7.8-4-7.8-4Z\" ";
        body += fillAttrs(withAlpha(color, 235));
        body += "/><path d=\"M4.2 12 12 16l7.8-4\"/><path d=\"M4.2 16.6 12 20.6l7.8-4\"/>";
        return wrapSvg(body, strokeAttrs(color, 2.05f));
    }
    if (icon == "upload") {
        return wrapSvg("<path d=\"M12 16.5V5.4\"/><path d=\"M7.1 10.3 12 5.4l4.9 4.9\"/><path d=\"M5.2 15.6v3.2h13.6v-3.2\"/>",
                       strokeAttrs(color, 2.15f));
    }
    if (icon == "plus") {
        return wrapSvg("<path d=\"M12 5v14\"/><path d=\"M5 12h14\"/>", strokeAttrs(color, 1.9f));
    }
    if (icon == "search") {
        return wrapSvg("<circle cx=\"10.5\" cy=\"10.5\" r=\"6.3\"/><path d=\"M15.4 15.4 21 21\"/>",
                       strokeAttrs(color, 2.3f));
    }
    if (icon == "eye") {
        std::string body = "<path d=\"M3.2 12s3.3-5.2 8.8-5.2 8.8 5.2 8.8 5.2-3.3 5.2-8.8 5.2S3.2 12 3.2 12Z\"/>";
        body += "<circle cx=\"12\" cy=\"12\" r=\"2.25\" ";
        body += fillAttrs(color);
        body += " stroke=\"none\"/>";
        return wrapSvg(body, strokeAttrs(color, 1.9f));
    }
    if (icon == "polygon") {
        return wrapSvg("<path d=\"M12.2 2.9 20.1 8.8 17.2 21 5.7 20.2 3.7 9.1 12.2 2.9Z\"/>",
                       strokeAttrs(color, 2.1f));
    }
    if (icon == "line") {
        return wrapSvg("<path d=\"M3.8 5.4 9.1 9.6 13.6 13.2 20.2 18.3\"/><circle cx=\"3.8\" cy=\"5.4\" r=\"1.35\"/><circle cx=\"20.2\" cy=\"18.3\" r=\"1.35\"/>",
                       strokeAttrs(color, 2.1f));
    }
    if (icon == "point") {
        return wrapSvg("<circle cx=\"12\" cy=\"12\" r=\"6.2\"/>", strokeAttrs(color, 2.1f));
    }
    if (icon == "river") {
        return wrapSvg("<path d=\"M3.8 9.4c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/><path d=\"M3.8 14.8c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/>",
                       strokeAttrs(color, 2.1f));
    }
    if (icon == "square") {
        return wrapSvg("<path d=\"M5.1 5.1h13.8v13.8H5.1Z\"/>", strokeAttrs(color, 2.1f));
    }
    if (icon == "edit") {
        return wrapSvg("<path d=\"M12 3.6H5.7c-1.2 0-2.1.9-2.1 2.1v12.6c0 1.2.9 2.1 2.1 2.1h12.6c1.2 0 2.1-.9 2.1-2.1V12\"/><path d=\"M16.8 3.9c.8-.8 2.1-.8 2.9 0s.8 2.1 0 2.9l-8.9 8.9-4 1 1-4 9-8.8Z\"/><path d=\"M15.5 5.2l3.3 3.3\"/>",
                       strokeAttrs(color, 2.05f));
    }
    if (icon == "pencil") {
        return wrapSvg("<path d=\"M4.2 19.8 5 16.2 16.4 4.8l2.8 2.8L7.8 19l-3.6.8Z\"/><path d=\"M14.8 6.4 17.6 9.2\"/>",
                       strokeAttrs(color, 2.0f));
    }
    if (icon == "target") {
        std::string body = "<circle cx=\"12\" cy=\"12\" r=\"8.1\"/><circle cx=\"12\" cy=\"12\" r=\"3.8\"/>";
        body += "<circle cx=\"12\" cy=\"12\" r=\"1.25\" ";
        body += fillAttrs(color);
        body += " stroke=\"none\"/>";
        return wrapSvg(body, strokeAttrs(color, 1.85f));
    }
    if (icon == "doc") {
        return wrapSvg("<path d=\"M6.1 3.5h11.8v17H6.1Z\"/><path d=\"M9 7.2h6\"/><path d=\"M9 11.5h6\"/><path d=\"M9 15.8h6\"/>",
                       strokeAttrs(color, 1.95f));
    }
    if (icon == "switch") {
        return wrapSvg("<path d=\"M4 7.2h15\"/><path d=\"M15.1 3.9 19 7.2l-3.9 3.3\"/><path d=\"M20 16.8H5\"/><path d=\"M8.9 13.5 5 16.8l3.9 3.3\"/>",
                       strokeAttrs(color, 1.95f));
    }
    if (icon == "gear") {
        return wrapSvg("<path d=\"M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.09a2 2 0 0 1 1 1.74v.5a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.38a2 2 0 0 0-.73-2.73l-.15-.09a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2Z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>",
                       strokeAttrs(color, 2.0f));
    }
    if (icon == "chart") {
        return wrapSvg("<path d=\"M4.4 4.4h15.2v15.2H4.4Z\"/><path d=\"M8 15.7v-4.2M12 15.7V8.2M16 15.7v-6\"/>",
                       strokeAttrs(color, 1.8f));
    }
    if (icon == "menu") {
        return wrapSvg("<path d=\"M4.2 6.8h15.6\"/><path d=\"M4.2 12h15.6\"/><path d=\"M4.2 17.2h15.6\"/>",
                       strokeAttrs(color, 2.0f));
    }
    if (icon == "more") {
        std::string body;
        body += "<circle cx=\"12\" cy=\"5.4\" r=\"1.9\" ";
        body += fillAttrs(color);
        body += "/><circle cx=\"12\" cy=\"12\" r=\"1.9\" ";
        body += fillAttrs(color);
        body += "/><circle cx=\"12\" cy=\"18.6\" r=\"1.9\" ";
        body += fillAttrs(color);
        body += "/>";
        return wrapSvg(body, "stroke=\"none\"");
    }
    if (icon == "drag") {
        std::string body;
        for (int y : {5, 12, 19}) {
            body += "<circle cx=\"8\" cy=\"" + std::to_string(y) + "\" r=\"1.6\" " + fillAttrs(color) + "/>";
            body += "<circle cx=\"16\" cy=\"" + std::to_string(y) + "\" r=\"1.6\" " + fillAttrs(color) + "/>";
        }
        return wrapSvg(body, "stroke=\"none\"");
    }
    return wrapSvg("<path d=\"M5.1 5.1h13.8v13.8H5.1Z\"/>", strokeAttrs(color, 2.0f));
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

}  // namespace skui
