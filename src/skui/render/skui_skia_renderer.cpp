#include "skui_internal.h"

#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkTypeface_win.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

SkRRect makeRRect(const Rect& rect, const CornerRadii& radii) {
    SkVector corners[4] = {
        {radii.topLeft, radii.topLeft},
        {radii.topRight, radii.topRight},
        {radii.bottomRight, radii.bottomRight},
        {radii.bottomLeft, radii.bottomLeft},
    };
    SkRRect rrect;
    rrect.setRectRadii(rect.sk(), corners);
    return rrect;
}

SkRRect makeInsetRRect(const Rect& rect, const CornerRadii& radii, float inset) {
    CornerRadii inner{
        std::max(0.0f, radii.topLeft - inset),
        std::max(0.0f, radii.topRight - inset),
        std::max(0.0f, radii.bottomRight - inset),
        std::max(0.0f, radii.bottomLeft - inset),
    };
    return makeRRect(rect, inner);
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

std::string colorToSvgHex(SkColor color) {
    std::ostringstream stream;
    stream << '#'
           << std::uppercase
           << std::hex
           << std::setfill('0')
           << std::setw(2) << static_cast<int>(SkColorGetR(color))
           << std::setw(2) << static_cast<int>(SkColorGetG(color))
           << std::setw(2) << static_cast<int>(SkColorGetB(color));
    return stream.str();
}

bool isUtf8ContinuationByte(unsigned char ch) {
    return (ch & 0xC0) == 0x80;
}

bool isEditableNode(const Node& node) {
    return node.tag == "input" || node.tag == "textarea";
}

bool isTextareaNode(const Node& node) {
    return node.tag == "textarea";
}

struct TextLine {
    size_t start = 0;
    size_t end = 0;
};

std::vector<TextLine> textLines(std::string_view value) {
    std::vector<TextLine> lines;
    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n') {
            lines.push_back({start, i});
            start = i + 1;
        }
    }
    lines.push_back({start, value.size()});
    return lines;
}

float lineHeightForNode(const Node& node) {
    return std::max(12.0f, node.style.fontSize * 1.38f);
}

float edgeOrZero(const std::optional<Length>& value) {
    if (!value || value->unit != LengthUnit::Px) {
        return 0.0f;
    }
    return value->value;
}

SkRect contentRectForText(const Node& node) {
    const float left = edgeOrZero(node.style.padding.left);
    const float top = edgeOrZero(node.style.padding.top);
    const float right = edgeOrZero(node.style.padding.right);
    const float bottom = edgeOrZero(node.style.padding.bottom);
    return SkRect::MakeLTRB(node.layout.x + left,
                            node.layout.y + top,
                            std::max(node.layout.x + left, node.layout.x + node.layout.w - right),
                            std::max(node.layout.y + top, node.layout.y + node.layout.h - bottom));
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

void replaceAll(std::string& value, std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string svgMarkupForDom(std::string svg, SkColor currentColor) {
    svg = normalizeSvgMarkup(std::move(svg));
    const std::string color = colorToSvgHex(currentColor);
    replaceAll(svg, "currentColor", color);
    replaceAll(svg, "currentcolor", color);
    replaceAll(svg, "CURRENTCOLOR", color);
    return svg;
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
    svgDomCache_.clear();
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
    f.setEdging(SkFont::Edging::kAntiAlias);
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
    const bool traceEnabled = perf::Trace::enabled();
    const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    traceRender_ = traceEnabled;
    traceBoxMs_ = 0.0;
    traceProgressMs_ = 0.0;
    traceImageMs_ = 0.0;
    traceSvgMs_ = 0.0;
    traceSelectionMs_ = 0.0;
    traceTextMs_ = 0.0;
    traceCaretMs_ = 0.0;
    traceScrollbarMs_ = 0.0;
    traceTextCount_ = 0;
    traceSvgCount_ = 0;
    traceNodeCount_ = 0;
    canvas.clear(options_.clearColor);
    const float scale = std::max(0.1f, dpiScale);
    canvas.save();
    canvas.scale(scale, scale);
    if (document.root) {
        drawNode(canvas, document, *document.root);
    }
    canvas.restore();
    if (traceEnabled) {
        std::ostringstream detail;
        detail << "nodes=" << traceNodeCount_
               << ";texts=" << traceTextCount_
               << ";svgs=" << traceSvgCount_
               << ";box_ms=" << traceBoxMs_
               << ";progress_ms=" << traceProgressMs_
               << ";image_ms=" << traceImageMs_
               << ";svg_ms=" << traceSvgMs_
               << ";selection_ms=" << traceSelectionMs_
               << ";text_ms=" << traceTextMs_
               << ";caret_ms=" << traceCaretMs_
               << ";scrollbar_ms=" << traceScrollbarMs_;
        perf::Trace::write("skui_render", "draw_breakdown", width, height, perf::Trace::elapsedMs(traceStart), detail.str());
        perf::Trace::write("skui_render", "draw_total", width, height, perf::Trace::elapsedMs(traceStart));
    }
    traceRender_ = false;
}

void SkiaRenderer::drawNode(SkCanvas& canvas, const Document& document, const Node& node) {
    if (node.style.display == Display::None) {
        return;
    }
    if (traceRender_) {
        ++traceNodeCount_;
    }

    const bool clipsChildren = node.style.overflowX != Overflow::Visible ||
                               node.style.overflowY != Overflow::Visible ||
                               node.scrollX > 0.0f ||
                               node.scrollY > 0.0f;
    const bool rejected = node.layout.w > 0.0f &&
                          node.layout.h > 0.0f &&
                          canvas.quickReject(node.layout.sk());
    if (rejected && (node.children.empty() || clipsChildren)) {
        return;
    }

    if (!rejected) {
        auto start = traceRender_ ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        drawBox(canvas, node);
        if (traceRender_) {
            traceBoxMs_ += perf::Trace::elapsedMs(start);
            start = perf::Trace::now();
        }
        drawProgress(canvas, node);
        if (traceRender_) {
            traceProgressMs_ += perf::Trace::elapsedMs(start);
            start = perf::Trace::now();
        }
        drawImage(canvas, document, node);
        if (traceRender_) {
            traceImageMs_ += perf::Trace::elapsedMs(start);
            start = perf::Trace::now();
        }
        drawInlineSvg(canvas, node);
        if (traceRender_) {
            traceSvgMs_ += perf::Trace::elapsedMs(start);
            start = perf::Trace::now();
        }
        drawSelectableSelection(canvas, node);
        drawInputSelection(canvas, node);
        if (traceRender_) {
            traceSelectionMs_ += perf::Trace::elapsedMs(start);
            start = perf::Trace::now();
        }
        drawText(canvas, node);
        if (traceRender_) {
            traceTextMs_ += perf::Trace::elapsedMs(start);
            start = perf::Trace::now();
        }
        drawInputCompositionUnderline(canvas, node);
        drawInputCaret(canvas, node);
        if (traceRender_) {
            traceCaretMs_ += perf::Trace::elapsedMs(start);
        }
    }

    if (clipsChildren) {
        const Rect contentClip = scrollContentClipRect(node);
        canvas.save();
        if (node.style.borderRadius.any()) {
            canvas.clipRRect(makeRRect(contentClip, node.style.borderRadius), SkClipOp::kIntersect, true);
        } else {
            canvas.clipRect(contentClip.sk(), SkClipOp::kIntersect, true);
        }
        canvas.translate(-node.scrollX, -node.scrollY);
    }
    for (const auto& child : node.children) {
        drawNode(canvas, document, *child);
    }
    if (clipsChildren) {
        canvas.restore();
    }
    const auto scrollbarStart = traceRender_ ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    drawScrollbars(canvas, node);
    if (traceRender_) {
        traceScrollbarMs_ += perf::Trace::elapsedMs(scrollbarStart);
    }
}

void SkiaRenderer::drawBox(SkCanvas& canvas, const Node& node) {
    const Rect r = node.layout;
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }

    if (SkColorGetA(node.style.backgroundColor) > 0 || node.style.backgroundGradient.kind != GradientKind::None) {
        if (node.style.borderRadius.any()) {
            canvas.drawRRect(makeRRect(r, node.style.borderRadius), backgroundPaint(node));
        } else {
            canvas.drawRect(r.sk(), backgroundPaint(node));
        }
    }

    const SkColor borderColor = node.style.flags.borderColor ? node.style.borderColor : node.style.color;
    if (node.style.borderStyle == BorderStyle::Solid &&
        node.style.borderWidth > 0.0f &&
        SkColorGetA(borderColor) > 0) {
        if (node.style.borderRadius.any()) {
            const float half = node.style.borderWidth * 0.5f;
            const SkRect border = SkRect::MakeXYWH(r.x + half,
                                                   r.y + half,
                                                   std::max(0.0f, r.w - node.style.borderWidth),
                                                   std::max(0.0f, r.h - node.style.borderWidth));
            canvas.drawRRect(makeInsetRRect({border.x(), border.y(), border.width(), border.height()},
                                            node.style.borderRadius,
                                            half),
                             stroke(borderColor, node.style.borderWidth));
        } else {
            SkPaint borderPaint = fill(borderColor);
            borderPaint.setAntiAlias(false);
            const float bw = std::min(node.style.borderWidth, std::min(r.w, r.h) * 0.5f);
            if (bw > 0.0f) {
                canvas.drawRect(SkRect::MakeXYWH(r.x, r.y, r.w, bw), borderPaint);
                canvas.drawRect(SkRect::MakeXYWH(r.x, r.y + r.h - bw, r.w, bw), borderPaint);
                canvas.drawRect(SkRect::MakeXYWH(r.x, r.y + bw, bw, std::max(0.0f, r.h - bw * 2.0f)), borderPaint);
                canvas.drawRect(SkRect::MakeXYWH(r.x + r.w - bw,
                                                 r.y + bw,
                                                 bw,
                                                 std::max(0.0f, r.h - bw * 2.0f)),
                                borderPaint);
            }
        }
    }
}

void SkiaRenderer::drawProgress(SkCanvas& canvas, const Node& node) {
    if (node.tag != "progress" || node.layout.w <= 0.0f || node.layout.h <= 0.0f) {
        return;
    }

    const float ratio = clampf(node.numericMax <= 0.0f ? 0.0f : node.numericValue / node.numericMax, 0.0f, 1.0f);
    if (ratio <= 0.0f) {
        return;
    }
    Rect fillRect = node.layout;
    fillRect.w *= ratio;
    SkPaint p = fill(node.style.color);
    if (node.style.borderRadius.any()) {
        canvas.save();
        canvas.clipRRect(makeRRect(node.layout, node.style.borderRadius), SkClipOp::kIntersect, true);
        canvas.drawRRect(makeRRect(fillRect, node.style.borderRadius), p);
        canvas.restore();
    } else {
        canvas.drawRect(fillRect.sk(), p);
    }
}

void SkiaRenderer::drawScrollbars(SkCanvas& canvas, const Node& node) {
    if (node.layout.w <= 0.0f || node.layout.h <= 0.0f) {
        return;
    }

    const float maxX = scrollMaxX(node);
    const float maxY = scrollMaxY(node);
    const bool showX = shouldShowScrollbarX(node) && node.scrollContentWidth > scrollViewportWidth(node);
    const bool showY = shouldShowScrollbarY(node) && node.scrollContentHeight > scrollViewportHeight(node);
    if (!showX && !showY) {
        return;
    }

    SkPaint thumb = fill(SkColorSetRGB(184, 195, 208));

    if (showY) {
        const float trackTop = node.layout.y + kSkuiScrollbarInset;
        const float trackBottom = node.layout.y + node.layout.h - kSkuiScrollbarInset - (showX ? kSkuiScrollbarThickness + kSkuiScrollbarInset : 0.0f);
        const float trackHeight = std::max(0.0f, trackBottom - trackTop);
        if (trackHeight > 0.0f) {
            const float ratio = node.scrollContentHeight <= 0.0f ? 1.0f : scrollViewportHeight(node) / node.scrollContentHeight;
            const float thumbHeight = clampf(trackHeight * ratio, std::min(kSkuiScrollbarMinThumb, trackHeight), trackHeight);
            const float travel = std::max(0.0f, trackHeight - thumbHeight);
            const float thumbTop = trackTop + (maxY <= 0.0f ? 0.0f : node.scrollY / maxY * travel);
            const SkRect trackRect = SkRect::MakeXYWH(node.layout.x + node.layout.w - kSkuiScrollbarInset - kSkuiScrollbarThickness,
                                                      trackTop,
                                                      kSkuiScrollbarThickness,
                                                      trackHeight);
            const SkRect thumbRect = SkRect::MakeXYWH(trackRect.x(),
                                                      thumbTop,
                                                      kSkuiScrollbarThickness,
                                                      thumbHeight);
            canvas.drawRRect(SkRRect::MakeRectXY(thumbRect, kSkuiScrollbarThickness * 0.5f, kSkuiScrollbarThickness * 0.5f), thumb);
        }
    }

    if (showX) {
        const float trackLeft = node.layout.x + kSkuiScrollbarInset;
        const float trackRight = node.layout.x + node.layout.w - kSkuiScrollbarInset - (showY ? kSkuiScrollbarThickness + kSkuiScrollbarInset : 0.0f);
        const float trackWidth = std::max(0.0f, trackRight - trackLeft);
        if (trackWidth > 0.0f) {
            const float ratio = node.scrollContentWidth <= 0.0f ? 1.0f : scrollViewportWidth(node) / node.scrollContentWidth;
            const float thumbWidth = clampf(trackWidth * ratio, std::min(kSkuiScrollbarMinThumb, trackWidth), trackWidth);
            const float travel = std::max(0.0f, trackWidth - thumbWidth);
            const float thumbLeft = trackLeft + (maxX <= 0.0f ? 0.0f : node.scrollX / maxX * travel);
            const SkRect trackRect = SkRect::MakeXYWH(trackLeft,
                                                      node.layout.y + node.layout.h - kSkuiScrollbarInset - kSkuiScrollbarThickness,
                                                      trackWidth,
                                                      kSkuiScrollbarThickness);
            const SkRect thumbRect = SkRect::MakeXYWH(thumbLeft,
                                                      trackRect.y(),
                                                      thumbWidth,
                                                      kSkuiScrollbarThickness);
            canvas.drawRRect(SkRRect::MakeRectXY(thumbRect, kSkuiScrollbarThickness * 0.5f, kSkuiScrollbarThickness * 0.5f), thumb);
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
    if (traceRender_) {
        ++traceSvgCount_;
    }
    drawSvgMarkup(canvas, *svg, node.layout, node.style.color);
}

void SkiaRenderer::drawInlineSvg(SkCanvas& canvas, const Node& node) {
    if (node.tag != "svg" || node.svgMarkup.empty()) {
        return;
    }
    if (traceRender_) {
        ++traceSvgCount_;
    }
    drawSvgMarkup(canvas, node.svgMarkup, node.layout, node.style.color);
}

void SkiaRenderer::drawSvgMarkup(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor) {
    if (svg.empty() || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    drawSvgDom(canvas, svg, rect, currentColor);
}

bool SkiaRenderer::drawSvgDom(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor) {
    if (svg.empty() || rect.w <= 0.0f || rect.h <= 0.0f) {
        return false;
    }

    const std::string key = svgMarkupForDom(svg, currentColor);
    auto it = svgDomCache_.find(key);
    if (it == svgDomCache_.end()) {
        SkMemoryStream stream(key.data(), key.size(), false);
        SvgDomEntry entry;
        entry.dom = SkSVGDOM::Builder().setFontManager(fontMgr_).make(stream);
        it = svgDomCache_.emplace(key, std::move(entry)).first;
    }

    if (!it->second.dom) {
        return false;
    }

    canvas.save();
    canvas.clipRect(rect.sk(), SkClipOp::kIntersect, true);
    canvas.translate(rect.x, rect.y);
    it->second.dom->setContainerSize(SkSize::Make(rect.w, rect.h));
    it->second.dom->render(&canvas);
    canvas.restore();
    return true;
}

void SkiaRenderer::drawInputSelection(SkCanvas& canvas, const Node& node) {
    if (!isEditableNode(node) || !node.focused || node.selectionStart == node.selectionEnd || node.value.empty()) {
        return;
    }

    const size_t start = std::min(node.selectionStart, node.value.size());
    const size_t end = std::min(node.selectionEnd, node.value.size());
    if (start >= end) {
        return;
    }

    SkPaint p = fill(SkColorSetARGB(96, 36, 232, 219));
    const SkRect content = contentRectForText(node);
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    if (isTextareaNode(node)) {
        const float lineHeight = lineHeightForNode(node);
        const float selectionHeight = std::max(12.0f, node.style.fontSize * 1.22f);
        const std::vector<TextLine> lines = textLines(node.value);
        for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            const TextLine line = lines[lineIndex];
            const size_t lineSelectionStart = std::max(start, line.start);
            const size_t lineSelectionEnd = std::min(end, line.end);
            if (lineSelectionStart > lineSelectionEnd || (lineSelectionStart == lineSelectionEnd && start != end)) {
                continue;
            }
            if (lineSelectionStart == line.end && lineSelectionEnd == line.end && lineSelectionStart != start) {
                continue;
            }
            const float before = lineSelectionStart <= line.start ? 0.0f : textWidth(std::string_view(node.value.data() + line.start, lineSelectionStart - line.start), node.style.fontSize, node.style.fontBold);
            float selected = 0.0f;
            if (lineSelectionEnd > lineSelectionStart) {
                selected = textWidth(std::string_view(node.value.data() + lineSelectionStart, lineSelectionEnd - lineSelectionStart), node.style.fontSize, node.style.fontBold);
            } else if (line.end < end) {
                selected = std::max(4.0f, node.style.fontSize * 0.45f);
            }
            const float x = content.left() + before;
            const float right = std::min(content.right(), x + selected);
            if (right > x) {
                const float y = content.top() + static_cast<float>(lineIndex) * lineHeight + (lineHeight - selectionHeight) * 0.5f;
                canvas.drawRect(SkRect::MakeXYWH(x, y, right - x, selectionHeight), p);
            }
        }
    } else {
        const float before = start == 0 ? 0.0f : textWidth(std::string_view(node.value.data(), start), node.style.fontSize, node.style.fontBold);
        const float selected = textWidth(std::string_view(node.value.data() + start, end - start), node.style.fontSize, node.style.fontBold);
        const float x = std::max(content.left(), content.left() + before);
        const float right = std::min(content.right(), x + selected);
        if (right > x) {
            const float selectionHeight = std::max(12.0f, node.style.fontSize * 1.35f);
            const float y = content.top() + (content.height() - selectionHeight) * 0.5f;
            canvas.drawRect(SkRect::MakeXYWH(x, y, right - x, selectionHeight), p);
        }
    }
    canvas.restore();
}

void SkiaRenderer::drawSelectableSelection(SkCanvas& canvas, const Node& node) {
    if (node.tag != "selectable" || node.selectionStart == node.selectionEnd) {
        return;
    }

    const std::string& value = !node.value.empty() ? node.value : node.text;
    const size_t start = std::min(node.selectionStart, value.size());
    const size_t end = std::min(node.selectionEnd, value.size());
    if (start >= end) {
        return;
    }

    const float before = start == 0 ? 0.0f : textWidth(std::string_view(value.data(), start), node.style.fontSize, node.style.fontBold);
    const float selected = textWidth(std::string_view(value.data() + start, end - start), node.style.fontSize, node.style.fontBold);
    const SkRect content = contentRectForText(node);
    const float x = std::max(content.left(), textStartX(node, value) + before);
    const float right = std::min(content.right(), x + selected);
    if (right <= x) {
        return;
    }

    const TextEntry& entry = textEntry(value, node.style.fontSize, node.style.fontBold);
    const float selectionHeight = std::max(12.0f, node.style.fontSize * 1.35f);
    const float y = content.top() + content.height() * 0.5f - selectionHeight * 0.5f;
    (void)entry;
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    canvas.drawRect(SkRect::MakeXYWH(x, y, right - x, selectionHeight), fill(SkColorSetARGB(96, 36, 232, 219)));
    canvas.restore();
}

void SkiaRenderer::drawText(SkCanvas& canvas, const Node& node) {
    if (node.tag == "progress") {
        return;
    }

    const bool editable = isEditableNode(node);
    const bool textarea = isTextareaNode(node);
    const bool hasComposition = editable && !node.compositionText.empty();
    const bool inputPlaceholder = editable && node.value.empty() && !hasComposition && !node.placeholder.empty();
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
    if (traceRender_) {
        ++traceTextCount_;
    }

    if (textarea) {
        SkColor textColor = node.style.color;
        if (inputPlaceholder) {
            textColor = SkColorSetA(textColor, static_cast<U8CPU>(std::min(150, static_cast<int>(SkColorGetA(textColor)))));
        }
        const SkRect content = contentRectForText(node);
        const float lineHeight = lineHeightForNode(node);
        const std::vector<TextLine> lines = textLines(value);
        canvas.save();
        canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
        for (size_t i = 0; i < lines.size(); ++i) {
            const TextLine line = lines[i];
            if (line.end <= line.start && !inputPlaceholder) {
                continue;
            }
            const std::string_view lineText(value.data() + line.start, line.end - line.start);
            if (lineText.empty()) {
                continue;
            }
            const TextEntry& entry = textEntry(lineText, node.style.fontSize, node.style.fontBold);
            const float baseline = content.top() + static_cast<float>(i) * lineHeight + lineHeight * 0.5f - (entry.metrics.fAscent + entry.metrics.fDescent) * 0.5f;
            canvas.drawTextBlob(entry.blob, content.left(), baseline, fill(textColor));
        }
        canvas.restore();
        return;
    }

    const TextEntry& entry = textEntry(value, node.style.fontSize, node.style.fontBold);
    const SkRect content = contentRectForText(node);
    const float availableWidth = std::max(0.0f, content.width());
    const float x = textStartX(node, value);
    const float y = content.top() + content.height() * 0.5f - (entry.metrics.fAscent + entry.metrics.fDescent) * 0.5f;
    SkColor textColor = node.style.color;
    if (inputPlaceholder) {
        textColor = SkColorSetA(textColor, static_cast<U8CPU>(std::min(150, static_cast<int>(SkColorGetA(textColor)))));
    }
    if (editable) {
        canvas.save();
        canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
        canvas.drawTextBlob(entry.blob, x, y, fill(textColor));
        canvas.restore();
    } else {
        canvas.drawTextBlob(entry.blob, x, y, fill(textColor));
    }
}

void SkiaRenderer::drawInputCompositionUnderline(SkCanvas& canvas, const Node& node) {
    if (!isEditableNode(node) || node.compositionText.empty()) {
        return;
    }

    const size_t cursor = std::min(node.cursorIndex, node.value.size());
    const SkRect content = contentRectForText(node);
    size_t lineStart = 0;
    float lineTop = content.top();
    float lineHeight = content.height();
    if (isTextareaNode(node)) {
        const std::vector<TextLine> lines = textLines(node.value);
        lineHeight = lineHeightForNode(node);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (cursor <= lines[i].end || i + 1 == lines.size()) {
                lineStart = lines[i].start;
                lineTop = content.top() + static_cast<float>(i) * lineHeight;
                break;
            }
        }
    }
    const float before = cursor == lineStart ? 0.0f : textWidth(std::string_view(node.value.data() + lineStart, cursor - lineStart), node.style.fontSize, node.style.fontBold);
    const float width = textWidth(node.compositionText, node.style.fontSize, node.style.fontBold);
    const float x = content.left() + before;
    const float right = std::min(content.right(), x + width);
    if (right <= x) {
        return;
    }
    const float y = lineTop + lineHeight * 0.5f + node.style.fontSize * 0.58f;
    SkPaint p = stroke(node.style.color, 1.0f);
    p.setStrokeCap(SkPaint::kButt_Cap);
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    canvas.drawLine(x, y, right, y, p);
    canvas.restore();
}

void SkiaRenderer::drawInputCaret(SkCanvas& canvas, const Node& node) {
    if (!isEditableNode(node) || !node.focused || node.selectionStart != node.selectionEnd) {
        return;
    }

    const size_t cursor = std::min(node.cursorIndex, node.value.size());
    const SkRect content = contentRectForText(node);
    size_t lineStart = 0;
    float lineTop = content.top();
    float lineHeight = content.height();
    if (isTextareaNode(node)) {
        const std::vector<TextLine> lines = textLines(node.value);
        lineHeight = lineHeightForNode(node);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (cursor <= lines[i].end || i + 1 == lines.size()) {
                lineStart = lines[i].start;
                lineTop = content.top() + static_cast<float>(i) * lineHeight;
                break;
            }
        }
    }
    std::string_view before(node.value.data() + lineStart, cursor - lineStart);
    float caretOffset = before.empty() ? 0.0f : textWidth(before, node.style.fontSize, node.style.fontBold);
    if (!node.compositionText.empty()) {
        caretOffset += textWidth(node.compositionText, node.style.fontSize, node.style.fontBold);
    }
    const float x = std::min(content.right() - 1.0f, content.left() + caretOffset);
    const float caretHeight = std::max(12.0f, node.style.fontSize * 1.18f);
    const float y0 = lineTop + (lineHeight - caretHeight) * 0.5f;
    const float y1 = y0 + caretHeight;

    SkPaint p = stroke(node.style.color, 1.25f);
    p.setStrokeCap(SkPaint::kButt_Cap);
    canvas.drawLine(x, y0, x, y1, p);
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
    f.getMetrics(&entry.metrics);
    entry.blob = SkTextBlob::MakeFromText(value.data(), value.size(), f, SkTextEncoding::kUTF8);
    it = textCache_.emplace(std::move(key), std::move(entry)).first;
    return it->second;
}

float SkiaRenderer::textWidth(std::string_view value, float size, bool bold) {
    return textEntry(value, size, bold).width;
}

float SkiaRenderer::textStartX(const Node& node, std::string_view value) {
    const SkRect content = contentRectForText(node);
    const float availableWidth = std::max(0.0f, content.width());
    const float width = textWidth(value, node.style.fontSize, node.style.fontBold);
    float x = content.left();
    if (node.style.justifyContent == YGJustifyCenter) {
        x = content.left() + (availableWidth - width) * 0.5f;
    } else if (node.style.justifyContent == YGJustifyFlexEnd) {
        x = content.right() - width;
    }
    return std::max(content.left(), x);
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
