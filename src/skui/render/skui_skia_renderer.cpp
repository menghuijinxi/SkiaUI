#include "skui_internal.h"

#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkPoint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/codec/SkCodec.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkTypeface_win.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
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

std::vector<unsigned char> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int hexDigitValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string decodeUrlPath(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hexDigitValue(value[i + 1]);
            const int low = hexDigitValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
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

float lineHeightForNode(const Node& node) {
    return std::max(12.0f, node.style.fontSize * 1.38f);
}

struct TextLineRange {
    size_t first = 0;
    size_t last = 0;
};

TextLineRange visibleTextLineRange(const SkRect& content,
                                   const Rect& viewport,
                                   float scrollY,
                                   float lineHeight,
                                   size_t lineCount) {
    if (lineCount == 0) {
        return {};
    }

    const float safeLineHeight = std::max(1.0f, lineHeight);
    const float textTop = content.top() - scrollY;
    const float textBottom = textTop + static_cast<float>(lineCount) * safeLineHeight;
    const float visibleTop = viewport.y;
    const float visibleBottom = viewport.y + viewport.h;
    if (visibleBottom <= textTop || visibleTop >= textBottom) {
        return {};
    }

    const float firstValue = std::floor((visibleTop - textTop) / safeLineHeight);
    const float lastValue = std::ceil((visibleBottom - textTop) / safeLineHeight) + 1.0f;
    size_t first = 0;
    if (firstValue > 0.0f) {
        first = std::min(lineCount - 1, static_cast<size_t>(firstValue));
    }

    size_t last = lineCount;
    if (lastValue > 0.0f) {
        last = std::min(lineCount, static_cast<size_t>(lastValue));
    }
    if (last < first) {
        last = first;
    }
    return {first, last};
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
    float right = edgeOrZero(node.style.padding.right);
    float bottom = edgeOrZero(node.style.padding.bottom);
    if (isTextareaNode(node)) {
        if (shouldShowScrollbarY(node)) {
            right += kSkuiScrollbarGutter;
        }
        if (shouldShowScrollbarX(node)) {
            bottom += kSkuiScrollbarGutter;
        }
    }
    return SkRect::MakeLTRB(node.layout.x + left,
                            node.layout.y + top,
                            std::max(node.layout.x + left, node.layout.x + node.layout.w - right),
                            std::max(node.layout.y + top, node.layout.y + node.layout.h - bottom));
}

bool sameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool sameCornerRadii(const CornerRadii& a, const CornerRadii& b) {
    return a.topLeft == b.topLeft &&
           a.topRight == b.topRight &&
           a.bottomRight == b.bottomRight &&
           a.bottomLeft == b.bottomLeft;
}

bool sameGradient(const Gradient& a, const Gradient& b) {
    return a.kind == b.kind && a.colors == b.colors && a.positions == b.positions;
}

bool shouldCacheBox(const Node& node, const Rect& rect) {
    constexpr float kMinCachedArea = 64000.0f;
    constexpr float kMaxCachedSide = 4096.0f;
    if (rect.w <= 0.0f || rect.h <= 0.0f || rect.w > kMaxCachedSide || rect.h > kMaxCachedSide) {
        return false;
    }

    const bool hasGradient = node.style.backgroundGradient.kind != GradientKind::None;
    return hasGradient || rect.w * rect.h >= kMinCachedArea;
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

std::string lowerAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool imageBufferLayout(int width, int height, size_t& rowBytes, size_t& byteSize) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const size_t safeWidth = static_cast<size_t>(width);
    const size_t safeHeight = static_cast<size_t>(height);
    if (safeWidth > std::numeric_limits<size_t>::max() / 4u) {
        return false;
    }
    rowBytes = safeWidth * 4u;
    if (rowBytes == 0 || rowBytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (safeHeight > std::numeric_limits<size_t>::max() / rowBytes) {
        return false;
    }
    byteSize = rowBytes * safeHeight;
    return byteSize <= static_cast<size_t>(std::numeric_limits<uint32_t>::max());
}

}  // namespace

SkiaRenderer::SkiaRenderer(RuntimeOptions options)
    : assetRoot_(std::move(options.assetRoot)),
      clearColor_(options.clearColor),
      requestRedraw_(std::move(options.requestRedraw)) {
    fontMgr_ = SkFontMgr_New_DirectWrite();
    if (!fontMgr_) {
        fontMgr_ = SkFontMgr_New_GDI();
    }
    regular_ = pickTypeface(false);
    bold_ = pickTypeface(true);
}

SkiaRenderer::~SkiaRenderer() {
    shutdownCaches();
}

void SkiaRenderer::clearCaches() {
    stopBitmapLoads(bitmapState_);
    svgFileCache_.clear();
    svgDomCache_.clear();
    textCache_.clear();
    textLineCache_.clear();
    boxCache_.clear();
    bitmapState_.reset();
}

void SkiaRenderer::shutdownCaches() {
    stopBitmapLoads(bitmapState_);
    svgFileCache_.clear();
    svgDomCache_.clear();
    textCache_.clear();
    textLineCache_.clear();
    boxCache_.clear();
    bitmapState_.reset();
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

SkPaint SkiaRenderer::backgroundPaint(const Node& node, const Rect& rect) const {
    if (node.style.backgroundGradient.kind == GradientKind::None || node.style.backgroundGradient.colors.empty()) {
        return fill(node.style.backgroundColor);
    }

    SkPaint p;
    p.setAntiAlias(true);
    p.setStyle(SkPaint::kFill_Style);
    std::vector<SkColor4f> colors = gradientColors(node.style.backgroundGradient.colors);
    const SkGradient gradient(SkGradient::Colors(colors, {}, SkTileMode::kClamp), {});
    if (node.style.backgroundGradient.kind == GradientKind::Radial) {
        p.setShader(SkShaders::RadialGradient(SkPoint::Make(rect.x + rect.w * 0.5f,
                                                             rect.y + rect.h * 0.5f),
                                              std::max(rect.w, rect.h) * 0.62f,
                                              gradient));
    } else {
        const SkPoint points[2] = {
            SkPoint::Make(rect.x, rect.y),
            node.style.backgroundGradient.kind == GradientKind::LinearY
                ? SkPoint::Make(rect.x, rect.y + rect.h)
                : SkPoint::Make(rect.x + rect.w, rect.y),
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
    canvas.clear(clearColor_);
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
    const float stickyOffsetY = stickyVisualOffsetY(node);
    if (stickyOffsetY != 0.0f) {
        canvas.save();
        canvas.translate(0.0f, stickyOffsetY);
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
        if (stickyOffsetY != 0.0f) {
            canvas.restore();
        }
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
    if (stickyOffsetY != 0.0f) {
        canvas.restore();
    }
}

void SkiaRenderer::drawBox(SkCanvas& canvas, const Node& node) {
    const Rect r = node.layout;
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }

    const bool hasBackground = SkColorGetA(node.style.backgroundColor) > 0 ||
                               node.style.backgroundGradient.kind != GradientKind::None;
    const SkColor borderColor = node.style.flags.borderColor ? node.style.borderColor : node.style.color;
    const bool hasBorder = node.style.borderStyle == BorderStyle::Solid &&
                           node.style.borderWidth > 0.0f &&
                           SkColorGetA(borderColor) > 0;
    if (!hasBackground && !hasBorder) {
        return;
    }

    if (shouldCacheBox(node, r) && drawCachedBox(canvas, node, r, borderColor)) {
        return;
    }

    drawBoxDirect(canvas, node, r);
}

void SkiaRenderer::drawBoxDirect(SkCanvas& canvas, const Node& node, const Rect& rect) {
    const Rect r = rect;
    if (SkColorGetA(node.style.backgroundColor) > 0 || node.style.backgroundGradient.kind != GradientKind::None) {
        SkPaint paint = backgroundPaint(node, r);
        if (node.style.borderRadius.any()) {
            canvas.drawRRect(makeRRect(r, node.style.borderRadius), paint);
        } else {
            paint.setAntiAlias(false);
            canvas.drawRect(r.sk(), paint);
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

bool SkiaRenderer::drawCachedBox(SkCanvas& canvas,
                                 const Node& node,
                                 const Rect& rect,
                                 SkColor borderColor) {
    const int imageWidth = std::max(1, static_cast<int>(std::ceil(rect.w)));
    const int imageHeight = std::max(1, static_cast<int>(std::ceil(rect.h)));
    BoxCacheEntry& entry = boxCache_[&node];
    if (entry.image &&
        entry.imageWidth == imageWidth &&
        entry.imageHeight == imageHeight &&
        sameRect(entry.layout, rect) &&
        entry.backgroundColor == node.style.backgroundColor &&
        sameGradient(entry.backgroundGradient, node.style.backgroundGradient) &&
        sameCornerRadii(entry.borderRadius, node.style.borderRadius) &&
        entry.borderStyle == node.style.borderStyle &&
        entry.borderWidth == node.style.borderWidth &&
        entry.borderColor == borderColor) {
        canvas.drawImageRect(entry.image, rect.sk(), SkSamplingOptions());
        return true;
    }

    const SkImageInfo info = SkImageInfo::MakeN32Premul(imageWidth, imageHeight);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) {
        return false;
    }

    SkCanvas* layer = surface->getCanvas();
    layer->clear(SK_ColorTRANSPARENT);
    drawBoxDirect(*layer, node, {0.0f, 0.0f, rect.w, rect.h});

    entry.layout = rect;
    entry.imageWidth = imageWidth;
    entry.imageHeight = imageHeight;
    entry.backgroundColor = node.style.backgroundColor;
    entry.backgroundGradient = node.style.backgroundGradient;
    entry.borderRadius = node.style.borderRadius;
    entry.borderStyle = node.style.borderStyle;
    entry.borderWidth = node.style.borderWidth;
    entry.borderColor = borderColor;
    entry.image = surface->makeImageSnapshot();
    if (!entry.image) {
        return false;
    }

    canvas.drawImageRect(entry.image, rect.sk(), SkSamplingOptions());
    return true;
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

    SkPaint track = fill(SkColorSetARGB(112, 47, 65, 84));
    SkPaint thumb = fill(SkColorSetARGB(232, 72, 238, 226));
    const float radius = kSkuiScrollbarThickness * 0.5f;

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
            canvas.drawRRect(SkRRect::MakeRectXY(trackRect, radius, radius), track);
            canvas.drawRRect(SkRRect::MakeRectXY(thumbRect, radius, radius), thumb);
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
            canvas.drawRRect(SkRRect::MakeRectXY(trackRect, radius, radius), track);
            canvas.drawRRect(SkRRect::MakeRectXY(thumbRect, radius, radius), thumb);
        }
    }
}

void SkiaRenderer::drawImage(SkCanvas& canvas, const Document& document, const Node& node) {
    if (node.tag != "img" || node.src.empty()) {
        return;
    }

    if (!isSvgSource(node.src)) {
        const std::string path = resolveAssetPath(document, node.src);
        if (path.empty()) {
            return;
        }
        BitmapImageEntry entry = bitmapImageEntry(path);
        if (entry.state != ImageState::Ready || !entry.pixels || entry.pixels->empty()) {
            return;
        }
        const SkImageInfo info = SkImageInfo::Make(entry.width, entry.height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        SkPixmap pixmap(info, entry.pixels->data(), entry.rowBytes);
        sk_sp<SkImage> image = SkImages::RasterFromPixmapCopy(pixmap);
        if (!image) {
            return;
        }
        canvas.save();
        if (node.style.borderRadius.any()) {
            canvas.clipRRect(makeRRect(node.layout, node.style.borderRadius), SkClipOp::kIntersect, true);
        } else {
            canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
        }
        SkPaint paint;
        paint.setAntiAlias(true);
        const SkSamplingOptions sampling = entry.width < 2 || entry.height < 2
                                             ? SkSamplingOptions(SkFilterMode::kNearest)
                                             : SkSamplingOptions(SkFilterMode::kLinear);
        const SkRect srcRect = SkRect::MakeWH(static_cast<float>(entry.width), static_cast<float>(entry.height));
        canvas.drawImageRect(image.get(), srcRect, node.layout.sk(), sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
        canvas.restore();
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

SkiaRenderer::BitmapImageEntry SkiaRenderer::bitmapImageEntry(const std::string& path) {
    if (!bitmapState_) {
        bitmapState_ = std::make_shared<BitmapImageState>();
    }
    std::shared_ptr<BitmapImageState> state = bitmapState_;
    {
        std::lock_guard lock(state->mutex);
        auto it = state->cache.find(path);
        if (it != state->cache.end()) {
            return it->second;
        }
        state->cache.emplace(path, BitmapImageEntry{});
    }
    enqueueBitmapLoad(state, path);
    return {};
}

void SkiaRenderer::enqueueBitmapLoad(const std::shared_ptr<BitmapImageState>& state, const std::string& path) const {
    if (!state) {
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        state->queue.push_back(path);
    }
    ensureBitmapWorker(state);
    state->cv.notify_one();
}

void SkiaRenderer::ensureBitmapWorker(const std::shared_ptr<BitmapImageState>& state) const {
    if (!state) {
        return;
    }
    std::lock_guard lock(state->mutex);
    if (state->workerStarted) {
        return;
    }
    state->workerStarted = true;
    state->worker = std::thread(bitmapWorkerLoop, state, requestRedraw_);
}

void SkiaRenderer::stopBitmapLoads(const std::shared_ptr<BitmapImageState>& state) {
    if (!state) {
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        state->queue.clear();
        state->cache.clear();
        state->dirty = false;
        state->stop = true;
    }
    state->cv.notify_all();
    if (state->worker.joinable()) {
        state->worker.join();
    }
}

void SkiaRenderer::bitmapWorkerLoop(std::shared_ptr<BitmapImageState> state, RequestRedrawCallback requestRedraw) {
    if (!state) {
        return;
    }

    for (;;) {
        std::string path;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] {
                return state->stop || !state->queue.empty();
            });
            if (state->stop) {
                return;
            }
            path = std::move(state->queue.front());
            state->queue.pop_front();
        }

        BitmapImageEntry loaded = loadBitmapImage(path);
        bool updated = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->stop) {
                return;
            }
            auto it = state->cache.find(path);
            if (it != state->cache.end()) {
                it->second = std::move(loaded);
                state->dirty = true;
                updated = true;
            }
        }
        if (updated && requestRedraw) {
            requestRedraw();
        }
    }
}

SkiaRenderer::BitmapImageEntry SkiaRenderer::loadBitmapImage(const std::string& path) {
    BitmapImageEntry entry;
    entry.state = ImageState::Failed;

    const std::vector<unsigned char> data = readBinaryFile(pathFromUtf8(path));
    if (data.empty()) {
        return entry;
    }

    sk_sp<SkData> encoded = SkData::MakeWithCopy(data.data(), data.size());
    if (!encoded) {
        return entry;
    }

    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(encoded));
    if (!codec) {
        return entry;
    }

    const SkImageInfo sourceInfo = codec->getInfo();
    const int width = sourceInfo.width();
    const int height = sourceInfo.height();
    if (width <= 0 || height <= 0) {
        return entry;
    }

    size_t rowBytes = 0;
    size_t byteSize = 0;
    if (!imageBufferLayout(width, height, rowBytes, byteSize)) {
        return entry;
    }

    auto pixels = std::make_shared<std::vector<unsigned char>>(byteSize);
    if (pixels->empty()) {
        return entry;
    }

    const SkImageInfo targetInfo = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    const SkCodec::Result result = codec->getPixels(targetInfo, pixels->data(), rowBytes);
    if (result != SkCodec::kSuccess && result != SkCodec::kIncompleteInput) {
        return entry;
    }

    entry.state = ImageState::Ready;
    entry.width = width;
    entry.height = height;
    entry.rowBytes = rowBytes;
    entry.pixels = std::move(pixels);
    return entry;
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
    p.setAntiAlias(false);
    const SkRect content = contentRectForText(node);
    canvas.save();
    canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    if (isTextareaNode(node)) {
        const float lineHeight = lineHeightForNode(node);
        const float selectionHeight = std::max(12.0f, node.style.fontSize * 1.22f);
        const std::vector<TextLine>& lines = textLines(node, node.value);
        const TextLineRange lineRange = visibleTextLineRange(content,
                                                             node.layout,
                                                             node.scrollY,
                                                             lineHeight,
                                                             lines.size());
        for (size_t lineIndex = lineRange.first; lineIndex < lineRange.last; ++lineIndex) {
            const TextLine line = lines[lineIndex];
            const size_t lineSelectionStart = std::max(start, line.start);
            const size_t lineSelectionEnd = std::min(end, line.end);
            if (lineSelectionStart > lineSelectionEnd || (lineSelectionStart == lineSelectionEnd && start != end)) {
                continue;
            }
            if (lineSelectionStart == line.end && lineSelectionEnd == line.end && lineSelectionStart != start) {
                continue;
            }
            const bool startsAtLineStart = lineSelectionStart <= line.start;
            const bool endsAtLineEnd = lineSelectionEnd >= line.end;
            const bool selectsWholeLine = startsAtLineStart &&
                                          endsAtLineEnd &&
                                          lineSelectionEnd > lineSelectionStart;
            const float before = startsAtLineStart
                ? 0.0f
                : textWidth(std::string_view(node.value.data() + line.start,
                                             lineSelectionStart - line.start),
                            node.style.fontSize,
                            node.style.fontBold);
            float selected = 0.0f;
            if (selectsWholeLine) {
                selected = content.width() + node.scrollX;
            } else if (lineSelectionEnd > lineSelectionStart) {
                selected = textWidth(std::string_view(node.value.data() + lineSelectionStart,
                                                      lineSelectionEnd - lineSelectionStart),
                                     node.style.fontSize,
                                     node.style.fontBold);
            } else if (line.end < end) {
                selected = std::max(4.0f, node.style.fontSize * 0.45f);
            }
            const float x = content.left() - node.scrollX + before;
            const float right = std::min(content.right(), x + selected);
            if (right > x) {
                const float y = content.top() - node.scrollY +
                                static_cast<float>(lineIndex) * lineHeight +
                                (lineHeight - selectionHeight) * 0.5f;
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
    std::string compositionValue;
    const std::string* value = nullptr;
    if (inputPlaceholder) {
        value = &node.placeholder;
    } else if (hasComposition) {
        const size_t cursor = std::min(node.cursorIndex, node.value.size());
        compositionValue = node.value.substr(0, cursor);
        compositionValue += node.compositionText;
        compositionValue += node.value.substr(cursor);
        value = &compositionValue;
    } else {
        value = !node.value.empty() ? &node.value : &node.text;
    }
    if (!value || value->empty()) {
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
        const std::vector<TextLine>& lines = textLines(node, *value);
        const TextLineRange lineRange = visibleTextLineRange(content,
                                                             node.layout,
                                                             node.scrollY,
                                                             lineHeight,
                                                             lines.size());
        canvas.save();
        canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
        for (size_t i = lineRange.first; i < lineRange.last; ++i) {
            const TextLine line = lines[i];
            if (line.end <= line.start && !inputPlaceholder) {
                continue;
            }
            const std::string_view lineText(value->data() + line.start, line.end - line.start);
            if (lineText.empty()) {
                continue;
            }
            const TextEntry& entry = textEntry(lineText, node.style.fontSize, node.style.fontBold);
            const float baseline = content.top() - node.scrollY +
                                   static_cast<float>(i) * lineHeight +
                                   lineHeight * 0.5f -
                                   (entry.metrics.fAscent + entry.metrics.fDescent) * 0.5f;
            canvas.drawTextBlob(entry.blob, content.left() - node.scrollX, baseline, fill(textColor));
        }
        canvas.restore();
        return;
    }

    const TextEntry& entry = textEntry(*value, node.style.fontSize, node.style.fontBold);
    const SkRect content = contentRectForText(node);
    const float availableWidth = std::max(0.0f, content.width());
    const float x = textStartX(node, *value);
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
        const std::vector<TextLine>& lines = textLines(node, node.value);
        lineHeight = lineHeightForNode(node);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (cursor <= lines[i].end || i + 1 == lines.size()) {
                lineStart = lines[i].start;
                lineTop = content.top() - node.scrollY + static_cast<float>(i) * lineHeight;
                break;
            }
        }
    }
    const float before = cursor == lineStart ? 0.0f : textWidth(std::string_view(node.value.data() + lineStart, cursor - lineStart), node.style.fontSize, node.style.fontBold);
    const float width = textWidth(node.compositionText, node.style.fontSize, node.style.fontBold);
    const float x = content.left() - node.scrollX + before;
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
        const std::vector<TextLine>& lines = textLines(node, node.value);
        lineHeight = lineHeightForNode(node);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (cursor <= lines[i].end || i + 1 == lines.size()) {
                lineStart = lines[i].start;
                lineTop = content.top() - node.scrollY + static_cast<float>(i) * lineHeight;
                break;
            }
        }
    }
    std::string_view before(node.value.data() + lineStart, cursor - lineStart);
    float caretOffset = before.empty() ? 0.0f : textWidth(before, node.style.fontSize, node.style.fontBold);
    if (!node.compositionText.empty()) {
        caretOffset += textWidth(node.compositionText, node.style.fontSize, node.style.fontBold);
    }
    const float x = std::min(content.right() - 1.0f, content.left() - node.scrollX + caretOffset);
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

    std::string svg = readTextFile(pathFromUtf8(path));
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

    const std::string decodedSrc = decodeUrlPath(src);
    fs::path path = pathFromUtf8(decodedSrc);
    if (path.is_absolute()) {
        return pathToUtf8(path);
    }

    if (!document.basePath.empty()) {
        fs::path candidate = pathFromUtf8(document.basePath) / path;
        if (fs::exists(candidate)) {
            return pathToUtf8(candidate);
        }
    }

    if (!assetRoot_.empty()) {
        fs::path candidate = pathFromUtf8(assetRoot_) / path;
        if (fs::exists(candidate)) {
            return pathToUtf8(candidate);
        }
    }

    return pathToUtf8(path);
}

bool SkiaRenderer::isSvgSource(std::string_view src) {
    const std::string value = lowerAscii(trim(src));
    constexpr std::string_view suffix = ".svg";
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool SkiaRenderer::consumeImageDirty() {
    std::shared_ptr<BitmapImageState> state = bitmapState_;
    if (!state) {
        return false;
    }
    std::lock_guard lock(state->mutex);
    const bool dirty = state->dirty;
    state->dirty = false;
    return dirty;
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

const std::vector<SkiaRenderer::TextLine>& SkiaRenderer::textLines(const Node& node,
                                                                   const std::string& value) {
    const bool cacheableValue = &value == &node.text ||
                                &value == &node.value ||
                                &value == &node.placeholder;
    TextLineCacheEntry& entry = textLineCache_[&node];
    if (cacheableValue &&
        entry.revision == node.textRevision &&
        entry.value == &value &&
        entry.size == value.size()) {
        return entry.lines;
    }

    entry.revision = node.textRevision;
    entry.value = cacheableValue ? &value : nullptr;
    entry.size = value.size();
    entry.lines.clear();

    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n') {
            entry.lines.push_back({start, i});
            start = i + 1;
        }
    }
    entry.lines.push_back({start, value.size()});
    return entry.lines;
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

    if (offset >= textWidth(value, size, bold)) {
        return value.size();
    }

    std::vector<size_t> boundaries;
    boundaries.reserve(value.size() + 1);
    boundaries.push_back(0);
    for (size_t index = nextUtf8Boundary(value, 0); index <= value.size(); index = nextUtf8Boundary(value, index)) {
        boundaries.push_back(index);
        if (index == value.size()) {
            break;
        }
    }

    size_t low = 1;
    size_t high = boundaries.size() - 1;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (textWidth(value.substr(0, boundaries[middle]), size, bold) < offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    const size_t current = boundaries[low];
    const size_t previous = boundaries[low - 1];
    const float previousWidth = textWidth(value.substr(0, previous), size, bold);
    const float currentWidth = textWidth(value.substr(0, current), size, bold);
    return offset < (previousWidth + currentWidth) * 0.5f ? previous : current;
}

}  // namespace skui
