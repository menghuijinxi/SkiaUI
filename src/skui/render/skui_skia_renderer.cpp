#include "skui_internal.h"

#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkPoint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/codec/SkCodec.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkColorMatrix.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/ganesh/GrRecordingContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
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
#include <thread>
#include <unordered_set>

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

sk_sp<SkColorFilter> makeColorFilter(const Filter& filter) {
    sk_sp<SkColorFilter> result;
    for (const FilterOperation& operation : filter.operations) {
        if (operation.kind == FilterOperationKind::DropShadow) {
            continue;
        }
        SkColorMatrix matrix;
        if (operation.kind == FilterOperationKind::Grayscale) {
            matrix.setSaturation(1.0f - clampf(operation.amount, 0.0f, 1.0f));
        } else if (operation.kind == FilterOperationKind::Brightness) {
            const float amount = std::max(0.0f, operation.amount);
            matrix.setScale(amount, amount, amount);
        }
        result = SkColorFilters::Compose(
            SkColorFilters::Matrix(matrix),
            std::move(result));
    }
    return result;
}

SkColor shadowColor(const Shadow& shadow, const Style& style) {
    return shadow.usesCurrentColor ? style.color : shadow.color;
}

float resolvedBorderWidth(const BorderSide& side) {
    return side.style.value_or(BorderStyle::None) == BorderStyle::Solid
        ? std::max(0.0f, side.width.value_or(0.0f))
        : 0.0f;
}

SkColor resolvedBorderColor(const BorderSide& side, const Style& style) {
    return side.color.value_or(style.color);
}

bool hasVisibleBorder(const BorderSide& side, const Style& style) {
    return resolvedBorderWidth(side) > 0.0f &&
           SkColorGetA(resolvedBorderColor(side, style)) > 0;
}

bool bordersMatch(const BorderSide& lhs,
                  const BorderSide& rhs,
                  const Style& style) {
    return resolvedBorderWidth(lhs) == resolvedBorderWidth(rhs) &&
           resolvedBorderColor(lhs, style) == resolvedBorderColor(rhs, style);
}

float shadowSigma(float blurRadius) {
    return std::max(0.0f, blurRadius) * 0.5f;
}

sk_sp<SkImageFilter> makeImageFilter(const Filter& filter,
                                     const Style& style) {
    sk_sp<SkImageFilter> result;
    for (const FilterOperation& operation : filter.operations) {
        if (operation.kind != FilterOperationKind::DropShadow) {
            continue;
        }
        const float sigma = shadowSigma(operation.shadow.blurRadius);
        result = SkImageFilters::DropShadow(
            operation.shadow.offsetX,
            operation.shadow.offsetY,
            sigma,
            sigma,
            shadowColor(operation.shadow, style),
            std::move(result));
    }
    return result;
}

struct ResolvedBackgroundSize {
    float width = 0.0f;
    float height = 0.0f;
};

float resolveBackgroundDimension(const Length& length, float reference) {
    if (length.unit == LengthUnit::Percent) {
        return reference * length.value / 100.0f;
    }
    if (length.unit == LengthUnit::Px) {
        return length.value;
    }
    return 0.0f;
}

float resolveTransformLength(const Length& length, float reference) {
    if (length.unit == LengthUnit::Percent) {
        return reference * length.value / 100.0f;
    }
    if (length.unit == LengthUnit::Px) {
        return length.value;
    }
    return 0.0f;
}

std::optional<Rect> resolvePseudoElementRect(const Node& node,
                                             const Style& style) {
    if (style.position != Position::Absolute) {
        return std::nullopt;
    }

    const auto resolveEdge = [](const std::optional<Length>& value,
                                float reference) {
        return value ? resolveTransformLength(*value, reference) : 0.0f;
    };
    const float left = resolveEdge(style.inset.left, node.layout.w);
    const float top = resolveEdge(style.inset.top, node.layout.h);
    const float right = resolveEdge(style.inset.right, node.layout.w);
    const float bottom = resolveEdge(style.inset.bottom, node.layout.h);

    float width = style.width
                      ? resolveTransformLength(*style.width, node.layout.w)
                      : 0.0f;
    float height = style.height
                       ? resolveTransformLength(*style.height, node.layout.h)
                       : 0.0f;
    if (!style.width && style.flags.insetLeft && style.flags.insetRight) {
        width = std::max(0.0f, node.layout.w - left - right);
    }
    if (!style.height && style.flags.insetTop && style.flags.insetBottom) {
        height = std::max(0.0f, node.layout.h - top - bottom);
    }

    float x = node.layout.x;
    float y = node.layout.y;
    if (style.flags.insetLeft) {
        x += left;
    } else if (style.flags.insetRight) {
        x += node.layout.w - right - width;
    }
    if (style.flags.insetTop) {
        y += top;
    } else if (style.flags.insetBottom) {
        y += node.layout.h - bottom - height;
    }
    return Rect{x, y, width, height};
}

void applyTransformOperation(SkCanvas& canvas,
                             const TransformOperation& operation,
                             const Rect& layout) {
    if (operation.kind == TransformOperationKind::Translate) {
        canvas.translate(
            resolveTransformLength(operation.translateX, layout.w),
            resolveTransformLength(operation.translateY, layout.h));
    } else if (operation.kind == TransformOperationKind::Scale) {
        canvas.scale(operation.scaleX, operation.scaleY);
    } else if (operation.kind == TransformOperationKind::Rotate) {
        canvas.rotate(operation.rotateDeg);
    }
}

ResolvedBackgroundSize resolveBackgroundSize(const BackgroundSize& size,
                                             const Rect& rect,
                                             int imageWidth,
                                             int imageHeight) {
    const bool autoWidth = size.width.unit == LengthUnit::Auto;
    const bool autoHeight = size.height.unit == LengthUnit::Auto;
    if (autoWidth && autoHeight) {
        return {
            static_cast<float>(imageWidth),
            static_cast<float>(imageHeight),
        };
    }

    const float aspectRatio =
        imageHeight > 0 ? static_cast<float>(imageWidth) /
                              static_cast<float>(imageHeight)
                        : 1.0f;
    float width = autoWidth
                      ? 0.0f
                      : resolveBackgroundDimension(size.width, rect.w);
    float height = autoHeight
                       ? 0.0f
                       : resolveBackgroundDimension(size.height, rect.h);
    if (autoWidth) {
        width = height * aspectRatio;
    } else if (autoHeight) {
        height = aspectRatio > 0.0f ? width / aspectRatio : 0.0f;
    }
    return {width, height};
}

float resolveBackgroundOffset(const Length& position,
                              float containerSize,
                              float imageSize) {
    if (position.unit == LengthUnit::Percent) {
        return (containerSize - imageSize) * position.value / 100.0f;
    }
    if (position.unit == LengthUnit::Px) {
        return position.value;
    }
    return 0.0f;
}

bool repeatsBackgroundX(BackgroundRepeat repeat) {
    return repeat == BackgroundRepeat::Repeat ||
           repeat == BackgroundRepeat::RepeatX;
}

bool repeatsBackgroundY(BackgroundRepeat repeat) {
    return repeat == BackgroundRepeat::Repeat ||
           repeat == BackgroundRepeat::RepeatY;
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

size_t nextUtf8Boundary(std::string_view value, size_t index);

bool isEditableNode(const Node& node) {
    return isTextEditingNode(node);
}

bool isTextareaNode(const Node& node) {
    return node.tag == "textarea";
}

float lineHeightForNode(const Node& node) {
    return std::max(12.0f, node.style.fontSize * 1.38f);
}

bool usesFlexTextAlignment(const Node& node) {
    return node.style.display == Display::Flex && node.style.displayFlex;
}

float selectableLineTop(const SkRect& content,
                        float lineHeight,
                        size_t lineCount,
                        size_t lineIndex,
                        bool useFlexAlignment) {
    if (useFlexAlignment && lineCount <= 1) {
        return content.top() + (content.height() - lineHeight) * 0.5f;
    }
    return content.top() + static_cast<float>(lineIndex) * lineHeight;
}

bool clipsTextOverflow(const Node& node) {
    return node.style.overflowX != Overflow::Visible ||
           node.style.overflowY != Overflow::Visible;
}

SkRect lineSelectionRect(float x, float y, float width, float height) {
    const float left = std::floor(x);
    const float top = std::round(y);
    const float right = std::ceil(x + width);
    const float bottom = std::round(y + height);
    return SkRect::MakeLTRB(left, top, right, bottom);
}

void addLineSelectionRect(SkPathBuilder& path, float x, float y, float width, float height) {
    path.addRect(lineSelectionRect(x, y, width, height));
}

bool rangesIntersect(size_t leftStart, size_t leftEnd, size_t rightStart, size_t rightEnd) {
    return leftStart < rightEnd && rightStart < leftEnd;
}

struct TextLineRange {
    size_t first = 0;
    size_t last = 0;
};

template <typename MeasureText>
size_t findSelectableLineEnd(std::string_view value,
                             size_t start,
                             size_t hardEnd,
                             float maxWidth,
                             MeasureText measureText) {
    if (maxWidth <= 0.0f || start >= hardEnd) {
        return hardEnd;
    }

    size_t lineEnd = start;
    size_t lastBreak = std::string_view::npos;
    while (lineEnd < hardEnd) {
        const size_t next = nextUtf8Boundary(value, lineEnd);
        const std::string_view candidate(value.data() + start, next - start);
        if (measureText(candidate) > maxWidth) {
            break;
        }
        if (std::isspace(static_cast<unsigned char>(value[lineEnd])) != 0 ||
            value[lineEnd] == '/' ||
            value[lineEnd] == '-' ||
            value[lineEnd] == '_' ||
            value[lineEnd] == '?' ||
            value[lineEnd] == '&' ||
            value[lineEnd] == '=') {
            lastBreak = next;
        }
        lineEnd = next;
    }

    if (lineEnd == hardEnd || lineEnd > start) {
        if (lineEnd < hardEnd && lastBreak != std::string_view::npos &&
            lastBreak > start) {
            return lastBreak;
        }
        return lineEnd;
    }

    return nextUtf8Boundary(value, start);
}

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

SkRect contentRectForText(const Node& node) {
    const float left = node.resolvedPadding.left;
    const float top = node.resolvedPadding.top;
    float right = node.resolvedPadding.right;
    float bottom = node.resolvedPadding.bottom;
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

size_t normalizeBitmapWorkerCount(size_t requested) {
    constexpr size_t kMaxBitmapWorkers = 4;
    if (requested == 0) {
        requested = 1;
    }
    const unsigned hardwareCount = std::thread::hardware_concurrency();
    const size_t hardwareLimit = hardwareCount == 0
                                     ? kMaxBitmapWorkers
                                     : std::max<size_t>(1, std::min<size_t>(kMaxBitmapWorkers, hardwareCount));
    return std::max<size_t>(1, std::min(requested, hardwareLimit));
}

}  // namespace

SkiaRenderer::SkiaRenderer(RuntimeOptions options)
    : assetRoot_(std::move(options.assetRoot)),
      clearColor_(options.clearColor),
      bitmapCacheBudgetBytes_(options.bitmapCacheBudgetBytes),
      bitmapLoadWorkerCount_(normalizeBitmapWorkerCount(options.bitmapLoadWorkerCount)),
      lazyImagePreloadMarginViewports_(
          std::max(0.0f, options.lazyImagePreloadMarginViewports)),
      requestRedraw_(std::move(options.requestRedraw)) {}

SkiaRenderer::~SkiaRenderer() {
    shutdownCaches();
}

void SkiaRenderer::clearCaches() {
    stopBitmapLoads(bitmapState_);
    svgFileCache_.clear();
    svgDomCache_.clear();
    textCache_.clear();
    clearNodeCaches();
    displayedBitmapImages_.clear();
    residentBitmapImages_.clear();
    lazyPreloadPathsPreviousFrame_.clear();
    lazyPreloadPathsCurrentFrame_.clear();
    bitmapState_.reset();
}

void SkiaRenderer::clearNodeCaches() {
    textLineCache_.clear();
}

MemoryStats SkiaRenderer::memoryStats() const {
    MemoryStats stats;
    BitmapMemoryStats& bitmapStats = stats.bitmapImages;
    bitmapStats.budgetBytes = bitmapCacheBudgetBytes_;
    bitmapStats.displayedImageCount = displayedBitmapImages_.size();

    std::unordered_set<std::string> displayedPaths;
    for (const auto& [node, displayed] : displayedBitmapImages_) {
        (void)node;
        if (!displayedPaths.insert(displayed.path).second) {
            continue;
        }
        size_t rowBytes = 0;
        size_t byteSize = 0;
        if (imageBufferLayout(displayed.width,
                              displayed.height,
                              rowBytes,
                              byteSize)) {
            bitmapStats.displayedBytes += byteSize;
        }
    }

    const std::shared_ptr<BitmapImageState> state = bitmapState_;
    if (state) {
        std::lock_guard lock(state->mutex);
        bitmapStats.budgetBytes = state->budgetBytes;
        bitmapStats.cacheBytes = state->cacheBytes;
        bitmapStats.peakCacheBytes = state->peakCacheBytes;
        bitmapStats.cacheEntryCount = state->cache.size();
        bitmapStats.cacheHitCount = state->cacheHitCount;
        bitmapStats.cacheMissCount = state->cacheMissCount;
        bitmapStats.decodeCount = state->decodeCount;
        bitmapStats.evictionCount = state->evictionCount;
        for (const auto& [path, entry] : state->cache) {
            (void)path;
            switch (entry.state) {
            case ImageState::Loading:
                ++bitmapStats.loadingImageCount;
                break;
            case ImageState::Ready:
                ++bitmapStats.readyImageCount;
                break;
            case ImageState::Failed:
                ++bitmapStats.failedImageCount;
                break;
            }
        }
    }

    stats.textCacheEntryCount = textCache_.size();
    stats.textLineCacheEntryCount = textLineCache_.size();
    stats.svgFileCacheEntryCount = svgFileCache_.size();
    stats.svgDomCacheEntryCount = svgDomCache_.size();
    return stats;
}

void SkiaRenderer::shutdownCaches() {
    stopBitmapLoads(bitmapState_);
    svgFileCache_.clear();
    svgDomCache_.clear();
    textCache_.clear();
    textLineCache_.clear();
    displayedBitmapImages_.clear();
    residentBitmapImages_.clear();
    lazyPreloadPathsPreviousFrame_.clear();
    lazyPreloadPathsCurrentFrame_.clear();
    bitmapState_.reset();
}

SkFont SkiaRenderer::font(float size, bool bold) const {
    return makeUiFont(size, bold);
}

SkPaint SkiaRenderer::fill(SkColor color) const {
    SkPaint p;
    p.setAntiAlias(true);
    p.setDither(true);
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

SkPaint SkiaRenderer::gradientPaint(const Gradient& gradient,
                                    const Style& style,
                                    const Rect& rect) const {
    SkPaint p;
    p.setAntiAlias(true);
    p.setDither(true);
    p.setStyle(SkPaint::kFill_Style);

    Rect gradientRect = rect;
    const bool hasExplicitWidth =
        style.backgroundSize.width.unit != LengthUnit::Auto;
    const bool hasExplicitHeight =
        style.backgroundSize.height.unit != LengthUnit::Auto;
    if (hasExplicitWidth) {
        gradientRect.w = resolveBackgroundDimension(
            style.backgroundSize.width,
            rect.w);
    }
    if (hasExplicitHeight) {
        gradientRect.h = resolveBackgroundDimension(
            style.backgroundSize.height,
            rect.h);
    }
    gradientRect.w = std::max(1.0f, gradientRect.w);
    gradientRect.h = std::max(1.0f, gradientRect.h);
    gradientRect.x += resolveBackgroundOffset(
        style.backgroundPosition.x,
        rect.w,
        gradientRect.w);
    gradientRect.y += resolveBackgroundOffset(
        style.backgroundPosition.y,
        rect.h,
        gradientRect.h);

    std::vector<SkColor4f> colors = gradientColors(gradient.colors);
    const auto makeColorStopPositions = [&](float extent) {
        std::vector<std::optional<float>> parsedPositions;
        parsedPositions.reserve(gradient.stopPositions.size());
        for (const std::optional<Length>& position :
             gradient.stopPositions) {
            if (!position) {
                parsedPositions.push_back(std::nullopt);
            } else if (position->unit == LengthUnit::Percent) {
                parsedPositions.push_back(clampf(
                    position->value / 100.0f,
                    0.0f,
                    1.0f));
            } else {
                parsedPositions.push_back(clampf(
                    position->value / std::max(1.0f, extent),
                    0.0f,
                    1.0f));
            }
        }
        if (parsedPositions.empty()) {
            parsedPositions.resize(colors.size());
        }
        parsedPositions.front() = parsedPositions.front().value_or(0.0f);
        parsedPositions.back() = parsedPositions.back().value_or(1.0f);
        size_t previousKnown = 0;
        for (size_t index = 1; index < parsedPositions.size(); ++index) {
            if (!parsedPositions[index]) {
                continue;
            }
            const float start = *parsedPositions[previousKnown];
            const float end = std::max(start, *parsedPositions[index]);
            parsedPositions[index] = end;
            const size_t interval = index - previousKnown;
            for (size_t fill = previousKnown + 1;
                 fill < index;
                 ++fill) {
                const float t =
                    static_cast<float>(fill - previousKnown) /
                    static_cast<float>(interval);
                parsedPositions[fill] = start + (end - start) * t;
            }
            previousKnown = index;
        }

        std::vector<float> positions;
        positions.reserve(parsedPositions.size());
        for (const std::optional<float>& position : parsedPositions) {
            positions.push_back(position.value_or(0.0f));
        }
        return positions;
    };
    SkGradient::Interpolation interpolation;
    interpolation.fColorSpace =
        SkGradient::Interpolation::ColorSpace::kSRGB;
    if (gradient.kind == GradientKind::Radial) {
        const float centerX = gradientRect.x +
            resolveTransformLength(gradient.centerX, gradientRect.w);
        const float centerY = gradientRect.y +
            resolveTransformLength(gradient.centerY, gradientRect.h);
        const float radiusX = std::max(
            std::abs(centerX - gradientRect.x),
            std::abs(gradientRect.x + gradientRect.w - centerX));
        const float radiusY = std::max(
            std::abs(centerY - gradientRect.y),
            std::abs(gradientRect.y + gradientRect.h - centerY));
        const float radius = std::sqrt(
            radiusX * radiusX + radiusY * radiusY);
        const std::vector<float> positions = makeColorStopPositions(radius);
        const SkGradient skGradient(
            SkGradient::Colors(colors, positions, SkTileMode::kClamp),
            interpolation);
        p.setShader(SkShaders::RadialGradient(
            SkPoint::Make(centerX, centerY),
            radius,
            skGradient));
    } else {
        float angleDegrees = gradient.angleDegrees;
        if (gradient.kind == GradientKind::LinearX) {
            angleDegrees = 90.0f;
        } else if (gradient.kind == GradientKind::LinearY) {
            angleDegrees = 180.0f;
        }
        const float radians = angleDegrees *
            3.14159265358979323846f / 180.0f;
        const float directionX = std::sin(radians);
        const float directionY = -std::cos(radians);
        const float halfLength =
            std::abs(directionX) * gradientRect.w * 0.5f +
            std::abs(directionY) * gradientRect.h * 0.5f;
        const float centerX = gradientRect.x + gradientRect.w * 0.5f;
        const float centerY = gradientRect.y + gradientRect.h * 0.5f;
        const SkPoint points[2] = {
            SkPoint::Make(
                centerX - directionX * halfLength,
                centerY - directionY * halfLength),
            SkPoint::Make(
                centerX + directionX * halfLength,
                centerY + directionY * halfLength),
        };
        const bool repeats =
            (hasExplicitWidth || hasExplicitHeight) &&
            style.backgroundRepeat != BackgroundRepeat::NoRepeat;
        const SkTileMode tileMode =
            repeats ? SkTileMode::kRepeat : SkTileMode::kClamp;
        const std::vector<float> positions =
            makeColorStopPositions(halfLength * 2.0f);
        const SkGradient skGradient(
            SkGradient::Colors(colors, positions, tileMode),
            interpolation);
        p.setShader(SkShaders::LinearGradient(
            points,
            skGradient));
    }
    return p;
}

void SkiaRenderer::draw(Document& document,
                        SkCanvas& canvas,
                        int width,
                        int height,
                        float dpiScale,
                        bool clearCanvas) {
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
    const float scale = std::max(0.1f, dpiScale);
    lazyImagePreloadMarginX_ =
        static_cast<float>(width) / scale * lazyImagePreloadMarginViewports_;
    lazyImagePreloadMarginY_ =
        static_cast<float>(height) / scale * lazyImagePreloadMarginViewports_;
    beginBitmapFrame();
    if (clearCanvas) {
        canvas.clear(clearColor_);
    }
    canvas.save();
    canvas.scale(scale, scale);
    if (document.root) {
        drawNode(canvas, document, *document.root);
    }
    canvas.restore();
    endBitmapFrame();
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
    const int saveCount = canvas.getSaveCount();
    const float stickyOffsetY = stickyVisualOffsetY(node);
    if (stickyOffsetY != 0.0f) {
        canvas.save();
        canvas.translate(0.0f, stickyOffsetY);
    }
    const bool hasTransform = !node.style.transform.isIdentity();
    if (hasTransform) {
        const float originX =
            node.layout.x +
            resolveTransformLength(node.style.transformOrigin.x, node.layout.w);
        const float originY =
            node.layout.y +
            resolveTransformLength(node.style.transformOrigin.y, node.layout.h);
        canvas.save();
        canvas.translate(originX, originY);
        for (const TransformOperation& operation : node.style.transform.operations) {
            applyTransformOperation(canvas, operation, node.layout);
        }
        canvas.translate(-originX, -originY);
    }
    requestLazyBitmapImageIfNeeded(canvas, document, node);
    const float opacity = clampf(node.style.opacity, 0.0f, 1.0f);
    if (opacity <= 0.0f) {
        canvas.restoreToCount(saveCount);
        return;
    }
    const bool hasFilter = !node.style.filter.isIdentity();
    if (opacity < 1.0f || hasFilter) {
        SkPaint paint;
        paint.setAlphaf(opacity);
        if (hasFilter) {
            paint.setColorFilter(makeColorFilter(node.style.filter));
            paint.setImageFilter(makeImageFilter(
                node.style.filter,
                node.style));
        }
        canvas.saveLayer(nullptr, &paint);
    }
    if (traceRender_) {
        ++traceNodeCount_;
    }

    const bool clipsChildren = node.style.overflowX != Overflow::Visible ||
                               node.style.overflowY != Overflow::Visible ||
                               node.scrollX > 0.0f ||
                               node.scrollY > 0.0f;
    const bool rejected = !hasTransform &&
                          node.layout.w > 0.0f &&
                          node.layout.h > 0.0f &&
                          canvas.quickReject(node.layout.sk());
    if (rejected && (node.children.empty() || clipsChildren)) {
        canvas.restoreToCount(saveCount);
        return;
    }

    const bool hasMask = node.style.maskGradient.has_value();
    if (hasMask) {
        canvas.saveLayer(node.layout.sk(), nullptr);
    }

    const bool drawsSelf = node.style.visibility != Visibility::Hidden;
    if (!rejected && drawsSelf) {
        auto start = traceRender_ ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        drawBox(canvas, document, node);
        if (node.hasBeforeStyle) {
            drawPseudoElement(
                canvas,
                document,
                node,
                node.beforeStyle);
        }
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
    if (requiresZIndexOrdering(node)) {
        const std::vector<const Node*> orderedChildren = childrenInPaintOrder(node);
        for (const Node* child : orderedChildren) {
            drawNode(canvas, document, *child);
        }
    } else {
        for (const auto& child : node.children) {
            drawNode(canvas, document, *child);
        }
    }
    if (drawsSelf && node.hasAfterStyle) {
        drawPseudoElement(canvas, document, node, node.afterStyle);
    }
    if (clipsChildren) {
        canvas.restore();
    }
    const auto scrollbarStart = traceRender_ ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    drawScrollbars(canvas, node);
    if (traceRender_) {
        traceScrollbarMs_ += perf::Trace::elapsedMs(scrollbarStart);
    }
    if (hasMask) {
        Style maskStyle;
        SkPaint maskPaint = gradientPaint(
            *node.style.maskGradient,
            maskStyle,
            node.layout);
        maskPaint.setBlendMode(SkBlendMode::kDstIn);
        canvas.drawRect(node.layout.sk(), maskPaint);
        canvas.restore();
    }
    canvas.restoreToCount(saveCount);
}

void SkiaRenderer::drawBox(SkCanvas& canvas,
                           const Document& document,
                           const Node& node) {
    const Rect r = node.layout;
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }

    const bool hasBackground = SkColorGetA(node.style.backgroundColor) > 0 ||
                               !node.style.backgroundGradients.empty() ||
                               !node.style.backgroundImage.empty();
    const bool hasShadow = !node.style.boxShadows.empty();
    const bool hasBorder =
        hasVisibleBorder(node.style.borders.left, node.style) ||
        hasVisibleBorder(node.style.borders.top, node.style) ||
        hasVisibleBorder(node.style.borders.right, node.style) ||
        hasVisibleBorder(node.style.borders.bottom, node.style);
    if (!hasBackground && !hasBorder && !hasShadow) {
        return;
    }

    drawBoxDirect(canvas, document, node, r);
}

void SkiaRenderer::drawBoxDirect(SkCanvas& canvas,
                                 const Document& document,
                                 const Node& node,
                                 const Rect& rect) {
    const Rect r = rect;
    drawBoxShadows(canvas, node, r, false);
    const auto drawBackgroundPaint = [&](const SkPaint& paint) {
        if (node.style.borderRadius.any()) {
            canvas.drawRRect(makeRRect(r, node.style.borderRadius), paint);
        } else {
            canvas.drawRect(r.sk(), paint);
        }
    };
    if (SkColorGetA(node.style.backgroundColor) > 0) {
        SkPaint paint = fill(node.style.backgroundColor);
        paint.setAntiAlias(node.style.borderRadius.any());
        drawBackgroundPaint(paint);
    }
    for (auto gradient = node.style.backgroundGradients.rbegin();
         gradient != node.style.backgroundGradients.rend();
         ++gradient) {
        drawBackgroundPaint(gradientPaint(*gradient, node.style, r));
    }

    drawBackgroundImage(canvas, document, node, r);
    drawBoxShadows(canvas, node, r, true);

    const BorderSide& left = node.style.borders.left;
    const BorderSide& top = node.style.borders.top;
    const BorderSide& right = node.style.borders.right;
    const BorderSide& bottom = node.style.borders.bottom;
    const bool uniformBorder = bordersMatch(left, top, node.style) &&
                               bordersMatch(top, right, node.style) &&
                               bordersMatch(right, bottom, node.style);
    if (uniformBorder && hasVisibleBorder(top, node.style) &&
        node.style.borderRadius.any()) {
        const float width = resolvedBorderWidth(top);
        const float half = width * 0.5f;
        const SkRect border = SkRect::MakeXYWH(
            r.x + half,
            r.y + half,
            std::max(0.0f, r.w - width),
            std::max(0.0f, r.h - width));
        canvas.drawRRect(
            makeInsetRRect(
                {border.x(), border.y(), border.width(), border.height()},
                node.style.borderRadius,
                half),
            stroke(resolvedBorderColor(top, node.style), width));
        return;
    }

    const auto drawHorizontalBorder = [&](const BorderSide& side,
                                          bool atBottom) {
        if (!hasVisibleBorder(side, node.style)) {
            return;
        }
        const float width = std::min(resolvedBorderWidth(side), r.h);
        const float y = atBottom ? r.y + r.h - width : r.y;
        SkPaint paint = fill(resolvedBorderColor(side, node.style));
        paint.setAntiAlias(false);
        canvas.drawRect(SkRect::MakeXYWH(r.x, y, r.w, width), paint);
    };
    const auto drawVerticalBorder = [&](const BorderSide& side,
                                        bool atRight) {
        if (!hasVisibleBorder(side, node.style)) {
            return;
        }
        const float width = std::min(resolvedBorderWidth(side), r.w);
        const float x = atRight ? r.x + r.w - width : r.x;
        SkPaint paint = fill(resolvedBorderColor(side, node.style));
        paint.setAntiAlias(false);
        canvas.drawRect(SkRect::MakeXYWH(x, r.y, width, r.h), paint);
    };

    if (node.style.borderRadius.any()) {
        canvas.save();
        canvas.clipRRect(
            makeRRect(r, node.style.borderRadius),
            SkClipOp::kIntersect,
            true);
    }
    drawHorizontalBorder(top, false);
    drawHorizontalBorder(bottom, true);
    drawVerticalBorder(left, false);
    drawVerticalBorder(right, true);
    if (node.style.borderRadius.any()) {
        canvas.restore();
    }
}

void SkiaRenderer::drawBoxShadows(SkCanvas& canvas,
                                  const Node& node,
                                  const Rect& rect,
                                  bool inset) {
    for (const Shadow& shadow : node.style.boxShadows) {
        if (shadow.inset != inset) {
            continue;
        }
        const SkColor color = shadowColor(shadow, node.style);
        if (SkColorGetA(color) == 0) {
            continue;
        }
        SkPaint paint = fill(color);
        const float sigma = shadowSigma(shadow.blurRadius);
        if (sigma > 0.0f) {
            paint.setMaskFilter(SkMaskFilter::MakeBlur(
                kNormal_SkBlurStyle,
                sigma));
        }

        Rect shadowRect = rect;
        shadowRect.x += shadow.offsetX;
        shadowRect.y += shadow.offsetY;
        if (!inset) {
            shadowRect.x -= shadow.spreadRadius;
            shadowRect.y -= shadow.spreadRadius;
            shadowRect.w += shadow.spreadRadius * 2.0f;
            shadowRect.h += shadow.spreadRadius * 2.0f;
            canvas.save();
            canvas.clipRRect(
                makeRRect(rect, node.style.borderRadius),
                SkClipOp::kDifference,
                true);
            canvas.drawRRect(
                makeInsetRRect(
                    shadowRect,
                    node.style.borderRadius,
                    -shadow.spreadRadius),
                paint);
            canvas.restore();
            continue;
        }

        canvas.save();
        canvas.clipRRect(
            makeRRect(rect, node.style.borderRadius),
            SkClipOp::kIntersect,
            true);
        const float margin = std::max(
            1.0f,
            shadow.blurRadius * 2.0f +
                std::abs(shadow.offsetX) +
                std::abs(shadow.offsetY) +
                std::abs(shadow.spreadRadius));
        const Rect outer = {
            rect.x - margin,
            rect.y - margin,
            rect.w + margin * 2.0f,
            rect.h + margin * 2.0f};
        Rect hole = {
            rect.x + shadow.offsetX + shadow.spreadRadius,
            rect.y + shadow.offsetY + shadow.spreadRadius,
            std::max(0.0f, rect.w - shadow.spreadRadius * 2.0f),
            std::max(0.0f, rect.h - shadow.spreadRadius * 2.0f)};
        SkPathBuilder shadowPath;
        shadowPath.setFillType(SkPathFillType::kEvenOdd);
        shadowPath.addRect(outer.sk());
        if (hole.w > 0.0f && hole.h > 0.0f) {
            shadowPath.addRRect(makeInsetRRect(
                hole,
                node.style.borderRadius,
                shadow.spreadRadius));
        }
        canvas.drawPath(shadowPath.detach(), paint);
        canvas.restore();
    }
}

void SkiaRenderer::drawPseudoElement(SkCanvas& canvas,
                                     const Document& document,
                                     const Node& node,
                                     const Style& style) {
    std::optional<Rect> rect = resolvePseudoElementRect(node, style);
    if (!rect || rect->w <= 0.0f || rect->h <= 0.0f) {
        return;
    }

    Node pseudo;
    pseudo.tag = "pseudo";
    pseudo.style = style;
    pseudo.layout = *rect;
    drawNode(canvas, document, pseudo);
}

void SkiaRenderer::drawBackgroundImage(SkCanvas& canvas,
                                       const Document& document,
                                       const Node& node,
                                       const Rect& rect) {
    if (node.style.backgroundImage.empty() ||
        isSvgSource(node.style.backgroundImage)) {
        return;
    }

    const std::string path =
        resolveAssetPath(document, node.style.backgroundImage);
    if (path.empty()) {
        return;
    }

    const BitmapImageEntry entry = bitmapImageEntry(path);
    sk_sp<SkImage> image = bitmapImageForEntry(canvas, path, entry);
    if (!image || entry.width <= 0 || entry.height <= 0) {
        return;
    }

    const ResolvedBackgroundSize size =
        resolveBackgroundSize(node.style.backgroundSize,
                              rect,
                              entry.width,
                              entry.height);
    if (size.width <= 0.0f || size.height <= 0.0f) {
        return;
    }

    float firstX =
        rect.x + resolveBackgroundOffset(node.style.backgroundPosition.x,
                                         rect.w,
                                         size.width);
    float firstY =
        rect.y + resolveBackgroundOffset(node.style.backgroundPosition.y,
                                         rect.h,
                                         size.height);
    const bool repeatX = repeatsBackgroundX(node.style.backgroundRepeat);
    const bool repeatY = repeatsBackgroundY(node.style.backgroundRepeat);
    if (repeatX) {
        while (firstX > rect.x) {
            firstX -= size.width;
        }
        while (firstX + size.width <= rect.x) {
            firstX += size.width;
        }
    }
    if (repeatY) {
        while (firstY > rect.y) {
            firstY -= size.height;
        }
        while (firstY + size.height <= rect.y) {
            firstY += size.height;
        }
    }

    canvas.save();
    if (node.style.borderRadius.any()) {
        canvas.clipRRect(makeRRect(rect, node.style.borderRadius),
                         SkClipOp::kIntersect,
                         true);
    } else {
        canvas.clipRect(rect.sk(), SkClipOp::kIntersect, true);
    }

    SkPaint paint;
    paint.setAntiAlias(true);
    const SkRect source =
        SkRect::MakeWH(static_cast<float>(entry.width),
                       static_cast<float>(entry.height));
    const SkSamplingOptions sampling(SkFilterMode::kLinear);
    constexpr int kMaxBackgroundTiles = 4096;
    int tileCount = 0;
    const float endX = repeatX ? rect.x + rect.w : firstX + size.width;
    const float endY = repeatY ? rect.y + rect.h : firstY + size.height;
    for (float y = firstY;
         y < endY && tileCount < kMaxBackgroundTiles;
         y += repeatY ? size.height : endY - firstY) {
        for (float x = firstX;
             x < endX && tileCount < kMaxBackgroundTiles;
             x += repeatX ? size.width : endX - firstX) {
            const SkRect destination =
                SkRect::MakeXYWH(x, y, size.width, size.height);
            canvas.drawImageRect(image.get(),
                                 source,
                                 destination,
                                 sampling,
                                 &paint,
                                 SkCanvas::kStrict_SrcRectConstraint);
            ++tileCount;
        }
    }
    canvas.restore();
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
    if (node.tag == "video") {
        if (node.videoFrame && node.videoFrameWidth > 0 && node.videoFrameHeight > 0) {
            drawBitmapImage(canvas,
                            node,
                            *node.videoFrame,
                            node.videoFrameWidth,
                            node.videoFrameHeight);
        }
        return;
    }

    if (node.tag != "img" || node.src.empty()) {
        return;
    }

    if (!isSvgSource(node.src)) {
        const std::string path = resolveAssetPath(document, node.src);
        if (path.empty()) {
            return;
        }
        BitmapImageEntry entry = bitmapImageEntry(path);
        int imageWidth = 0;
        int imageHeight = 0;
        sk_sp<SkImage> image = displayBitmapImageForNode(
            canvas,
            node,
            path,
            entry,
            imageWidth,
            imageHeight);
        if (!image) {
            return;
        }
        drawBitmapImage(canvas, node, *image, imageWidth, imageHeight);
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

sk_sp<SkImage> SkiaRenderer::displayBitmapImageForNode(SkCanvas& canvas,
                                                       const Node& node,
                                                       const std::string& path,
                                                       const BitmapImageEntry& entry,
                                                       int& imageWidth,
                                                       int& imageHeight) {
    sk_sp<SkImage> image = bitmapImageForEntry(canvas, path, entry);
    if (image) {
        DisplayedBitmapImage& displayed = displayedBitmapImages_[&node];
        displayed.path = path;
        displayed.image = image;
        displayed.lastUsedFrame = entry.lastUsedFrame;
        displayed.width = entry.width;
        displayed.height = entry.height;
        imageWidth = displayed.width;
        imageHeight = displayed.height;
        return image;
    }

    const auto displayedIt = displayedBitmapImages_.find(&node);
    if (displayedIt == displayedBitmapImages_.end() || !displayedIt->second.image) {
        return nullptr;
    }
    if (bitmapState_) {
        std::lock_guard lock(bitmapState_->mutex);
        displayedIt->second.lastUsedFrame = bitmapState_->frame;
    }
    imageWidth = displayedIt->second.width;
    imageHeight = displayedIt->second.height;
    return displayedIt->second.image;
}

void SkiaRenderer::drawBitmapImage(SkCanvas& canvas,
                                   const Node& node,
                                   SkImage& image,
                                   int imageWidth,
                                   int imageHeight) {
    if (imageWidth <= 0 || imageHeight <= 0) {
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
    const SkSamplingOptions sampling = imageWidth < 2 || imageHeight < 2
                                           ? SkSamplingOptions(SkFilterMode::kNearest)
                                           : SkSamplingOptions(SkFilterMode::kLinear);
    const SkRect srcRect = SkRect::MakeWH(static_cast<float>(imageWidth), static_cast<float>(imageHeight));
    canvas.drawImageRect(&image,
                         srcRect,
                         node.layout.sk(),
                         sampling,
                         &paint,
                         SkCanvas::kStrict_SrcRectConstraint);
    canvas.restore();
}

sk_sp<SkImage> SkiaRenderer::bitmapImageForEntry(SkCanvas& canvas,
                                                 const std::string& path,
                                                 const BitmapImageEntry& entry) {
    if (entry.state != ImageState::Ready) {
        return nullptr;
    }

    sk_sp<SkImage> rasterImage = entry.image;
    if (!rasterImage) {
        if (!entry.pixels || entry.pixels->empty()) {
            return nullptr;
        }

        const SkImageInfo info = SkImageInfo::Make(entry.width,
                                                   entry.height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType,
                                                   SkColorSpace::MakeSRGB());
        const SkPixmap pixmap(info, entry.pixels->data(), entry.rowBytes);
        rasterImage = SkImages::RasterFromPixmapCopy(pixmap);
        if (!rasterImage) {
            return nullptr;
        }

        if (bitmapState_) {
            std::lock_guard lock(bitmapState_->mutex);
            auto cacheIt = bitmapState_->cache.find(path);
            if (cacheIt != bitmapState_->cache.end() &&
                cacheIt->second.state == ImageState::Ready &&
                !cacheIt->second.image &&
                cacheIt->second.pixels == entry.pixels) {
                cacheIt->second.image = rasterImage;
                cacheIt->second.pixels.reset();
            }
        }
    }

    ResidentBitmapImage& resident = residentBitmapImages_[path];
    if (resident.rasterImage != rasterImage) {
        resident.rasterImage = rasterImage;
        resident.displayImage.reset();
    }
    resident.rowBytes = entry.rowBytes;
    resident.byteSize = entry.byteSize;
    resident.lastUsedFrame = entry.lastUsedFrame;
    resident.width = entry.width;
    resident.height = entry.height;

    GrDirectContext* directContext =
        GrAsDirectContext(canvas.recordingContext());
    if (!directContext) {
        return rasterImage;
    }

    // 当前绘制路径由 resident map 明确持有，离开当前帧工作集后统一释放 GPU 副本。
    // 这里不加入 Ganesh 的普通 LRU，避免可见纹理在帧间被预算驱逐并重复上传。
    if (resident.displayImage) {
        sk_sp<SkImage> compatibleImage = SkImages::TextureFromImage(
            directContext,
            resident.displayImage.get(),
            skgpu::Mipmapped::kNo,
            skgpu::Budgeted::kNo);
        if (compatibleImage) {
            resident.displayImage = compatibleImage;
            return compatibleImage;
        }
    }

    sk_sp<SkImage> textureImage = SkImages::TextureFromImage(
        directContext,
        rasterImage.get(),
        skgpu::Mipmapped::kNo,
        skgpu::Budgeted::kNo);
    if (!textureImage) {
        return rasterImage;
    }
    resident.displayImage = textureImage;
    return textureImage;
}

void SkiaRenderer::beginBitmapFrame() {
    lazyPreloadPathsCurrentFrame_.clear();
    std::shared_ptr<BitmapImageState> state = bitmapState_;
    if (!state) {
        return;
    }
    std::lock_guard lock(state->mutex);
    ++state->frame;
    state->activePaths.clear();
}

void SkiaRenderer::endBitmapFrame() {
    std::shared_ptr<BitmapImageState> state = bitmapState_;
    uint64_t frame = 0;
    if (state) {
        std::lock_guard lock(state->mutex);
        frame = state->frame;
        pruneBitmapCacheLocked(*state);
    }

    if (state) {
        for (auto it = residentBitmapImages_.begin();
             it != residentBitmapImages_.end();) {
            if (it->second.lastUsedFrame != frame) {
                it = residentBitmapImages_.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = displayedBitmapImages_.begin();
             it != displayedBitmapImages_.end();) {
            if (it->second.lastUsedFrame != frame) {
                it = displayedBitmapImages_.erase(it);
            } else {
                ++it;
            }
        }
    }

    lazyPreloadPathsPreviousFrame_ =
        std::move(lazyPreloadPathsCurrentFrame_);
    lazyPreloadPathsCurrentFrame_.clear();
}

void SkiaRenderer::requestBitmapImages(const Document& document) {
    if (!document.root) {
        return;
    }
    requestBitmapImagesForNode(document, *document.root);
}

namespace {

bool shouldRequestBitmapImageEagerly(const Node& node) {
    auto it = node.attributes.find("loading");
    if (it == node.attributes.end()) {
        return true;
    }
    return lowerAscii(trim(it->second)) != "lazy";
}

}  // namespace

void SkiaRenderer::requestLazyBitmapImageIfNeeded(SkCanvas& canvas,
                                                  const Document& document,
                                                  const Node& node) {
    if (node.tag != "img" ||
        node.src.empty() ||
        isSvgSource(node.src) ||
        shouldRequestBitmapImageEagerly(node)) {
        return;
    }

    SkRect preloadBounds;
    if (!canvas.getLocalClipBounds(&preloadBounds)) {
        return;
    }
    preloadBounds.outset(lazyImagePreloadMarginX_,
                         lazyImagePreloadMarginY_);
    if (!SkRect::Intersects(preloadBounds, node.layout.sk())) {
        return;
    }

    const std::string path = resolveAssetPath(document, node.src);
    if (path.empty()) {
        return;
    }

    const bool firstRequestThisFrame =
        lazyPreloadPathsCurrentFrame_.insert(path).second;
    const bool enteredPreloadMargin =
        lazyPreloadPathsPreviousFrame_.find(path) ==
        lazyPreloadPathsPreviousFrame_.end();
    // 只在进入边距时触发；否则超预算淘汰会被加载完成回调放大为解码循环。
    if (firstRequestThisFrame && enteredPreloadMargin) {
        requestBitmapImage(path);
    }
}

void SkiaRenderer::requestBitmapImagesForNode(const Document& document, const Node& node) {
    if (node.tag == "img" &&
        !node.src.empty() &&
        !isSvgSource(node.src) &&
        shouldRequestBitmapImageEagerly(node)) {
        const std::string path = resolveAssetPath(document, node.src);
        if (!path.empty()) {
            requestBitmapImage(path);
        }
    }

    if (!node.style.backgroundImage.empty() &&
        !isSvgSource(node.style.backgroundImage)) {
        const std::string path =
            resolveAssetPath(document, node.style.backgroundImage);
        if (!path.empty()) {
            requestBitmapImage(path);
        }
    }

    for (const auto& child : node.children) {
        requestBitmapImagesForNode(document, *child);
    }
}

void SkiaRenderer::requestBitmapImage(const std::string& path) {
    if (!bitmapState_) {
        bitmapState_ = std::make_shared<BitmapImageState>();
        bitmapState_->budgetBytes = bitmapCacheBudgetBytes_;
        bitmapState_->workerCount = bitmapLoadWorkerCount_;
    }
    std::shared_ptr<BitmapImageState> state = bitmapState_;
    {
        std::lock_guard lock(state->mutex);
        if (state->cache.find(path) != state->cache.end()) {
            ++state->cacheHitCount;
            return;
        }
        ++state->cacheMissCount;
        state->cache.emplace(path, BitmapImageEntry{});
    }
    enqueueBitmapLoad(state, path);
}

SkiaRenderer::BitmapImageEntry SkiaRenderer::bitmapImageEntry(const std::string& path) {
    if (!bitmapState_) {
        bitmapState_ = std::make_shared<BitmapImageState>();
        bitmapState_->budgetBytes = bitmapCacheBudgetBytes_;
        bitmapState_->workerCount = bitmapLoadWorkerCount_;
    }
    std::shared_ptr<BitmapImageState> state = bitmapState_;
    {
        std::lock_guard lock(state->mutex);
        state->activePaths.insert(path);
        auto it = state->cache.find(path);
        if (it != state->cache.end()) {
            ++state->cacheHitCount;
            it->second.lastUsedFrame = state->frame;
            return it->second;
        }

        ++state->cacheMissCount;
        const auto residentIt = residentBitmapImages_.find(path);
        if (residentIt != residentBitmapImages_.end() &&
            residentIt->second.rasterImage) {
            BitmapImageEntry retained;
            retained.state = ImageState::Ready;
            retained.image = residentIt->second.rasterImage;
            retained.rowBytes = residentIt->second.rowBytes;
            retained.byteSize = residentIt->second.byteSize;
            retained.lastUsedFrame = state->frame;
            retained.width = residentIt->second.width;
            retained.height = residentIt->second.height;
            state->cacheBytes += retained.byteSize;
            state->peakCacheBytes =
                std::max(state->peakCacheBytes, state->cacheBytes);
            state->cache.emplace(path, retained);
            return retained;
        }

        BitmapImageEntry pending;
        pending.lastUsedFrame = state->frame;
        state->cache.emplace(path, std::move(pending));
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
    ensureBitmapWorkers(state);
    state->cv.notify_one();
}

void SkiaRenderer::ensureBitmapWorkers(const std::shared_ptr<BitmapImageState>& state) const {
    if (!state) {
        return;
    }
    std::lock_guard lock(state->mutex);
    if (state->workersStarted) {
        return;
    }
    state->workersStarted = true;
    state->workers.reserve(state->workerCount);
    for (size_t i = 0; i < state->workerCount; ++i) {
        state->workers.emplace_back(bitmapWorkerLoop, state, requestRedraw_);
    }
}

void SkiaRenderer::stopBitmapLoads(const std::shared_ptr<BitmapImageState>& state) {
    if (!state) {
        return;
    }
    {
        std::lock_guard lock(state->mutex);
        state->queue.clear();
        state->cache.clear();
        state->activePaths.clear();
        state->cacheBytes = 0;
        state->dirty = false;
        state->stop = true;
    }
    state->cv.notify_all();
    for (std::thread& worker : state->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    state->workers.clear();
}

void SkiaRenderer::pruneBitmapCacheLocked(BitmapImageState& state) {
    if (state.cacheBytes <= state.budgetBytes) {
        return;
    }

    while (state.cacheBytes > state.budgetBytes) {
        auto candidate = state.cache.end();
        uint64_t oldestFrame = std::numeric_limits<uint64_t>::max();
        for (auto it = state.cache.begin(); it != state.cache.end(); ++it) {
            const BitmapImageEntry& entry = it->second;
            if (entry.state != ImageState::Ready || entry.byteSize == 0) {
                continue;
            }
            if (state.activePaths.find(it->first) != state.activePaths.end() ||
                (state.frame != 0 && entry.lastUsedFrame == state.frame)) {
                continue;
            }
            if (entry.lastUsedFrame < oldestFrame) {
                oldestFrame = entry.lastUsedFrame;
                candidate = it;
            }
        }
        if (candidate == state.cache.end()) {
            return;
        }
        const size_t byteSize = candidate->second.byteSize;
        state.cacheBytes = state.cacheBytes >= byteSize ? state.cacheBytes - byteSize : 0;
        state.cache.erase(candidate);
        ++state.evictionCount;
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
            if (it != state->cache.end() &&
                it->second.state == ImageState::Loading) {
                const size_t oldByteSize = it->second.byteSize;
                state->cacheBytes = state->cacheBytes >= oldByteSize ? state->cacheBytes - oldByteSize : 0;
                loaded.lastUsedFrame = it->second.lastUsedFrame;
                it->second = std::move(loaded);
                if (it->second.state == ImageState::Ready) {
                    state->cacheBytes += it->second.byteSize;
                    state->peakCacheBytes =
                        std::max(state->peakCacheBytes,
                                 state->cacheBytes);
                    ++state->decodeCount;
                }
                pruneBitmapCacheLocked(*state);
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

    const SkImageInfo targetInfo = SkImageInfo::Make(width,
                                                     height,
                                                     kBGRA_8888_SkColorType,
                                                     kPremul_SkAlphaType,
                                                     SkColorSpace::MakeSRGB());
    const SkCodec::Result result = codec->getPixels(targetInfo, pixels->data(), rowBytes);
    if (result != SkCodec::kSuccess && result != SkCodec::kIncompleteInput) {
        return entry;
    }

    entry.state = ImageState::Ready;
    entry.width = width;
    entry.height = height;
    entry.rowBytes = rowBytes;
    entry.byteSize = byteSize;
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
        entry.dom =
            SkSVGDOM::Builder().setFontManager(uiFontManager()).make(stream);
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
    if (!isEditableNode(node) || !node.editingFocused || node.selectionStart == node.selectionEnd || node.value.empty()) {
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
        const std::vector<TextLine>& lines = textLines(node, node.value);
        const TextLineRange lineRange = visibleTextLineRange(content,
                                                             node.layout,
                                                             node.scrollY,
                                                             lineHeight,
                                                             lines.size());
        SkPathBuilder selectionPath;
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
                                static_cast<float>(lineIndex) * lineHeight;
                addLineSelectionRect(selectionPath, x, y, right - x, lineHeight);
            }
        }
        if (!selectionPath.isEmpty()) {
            canvas.drawPath(selectionPath.detach(), p);
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

    const SkRect content = contentRectForText(node);
    const std::vector<TextLine>& lines = textLines(node, value);
    const float lineHeight = lineHeightForNode(node);
    const bool clipsOverflow = clipsTextOverflow(node);
    if (clipsOverflow) {
        canvas.save();
        canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
    }
    SkPathBuilder selectionPath;
    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const TextLine line = lines[lineIndex];
        const size_t lineStart = line.start;
        const size_t lineEnd = line.end;
        const size_t lineSelectionStart = std::max(start, lineStart);
        const size_t lineSelectionEnd = std::min(end, lineEnd);
        const bool selectsLineBreak = start <= lineEnd && end > lineEnd && lineEnd >= lineStart;
        if (lineSelectionStart >= lineSelectionEnd && !selectsLineBreak) {
            continue;
        }

        const std::string_view lineText(value.data() + lineStart, lineEnd - lineStart);
        const float lineX = textStartX(node, lineText);
        const float before = lineSelectionStart == lineStart
                                 ? 0.0f
                                 : textWidth(std::string_view(value.data() + lineStart,
                                                              lineSelectionStart - lineStart),
                                             node.style.fontSize,
                                             node.style.fontBold);
        float selected = 0.0f;
        if (lineSelectionEnd > lineSelectionStart) {
            selected = textWidth(std::string_view(value.data() + lineSelectionStart,
                                                  lineSelectionEnd - lineSelectionStart),
                                 node.style.fontSize,
                                 node.style.fontBold);
        }
        if (selectsLineBreak) {
            selected = std::max(selected, content.right() - lineX - before);
        }
        const float x = std::max(content.left(), lineX + before);
        const float right = std::min(content.right(), x + selected);
        if (right <= x) {
            continue;
        }

        const float y = selectableLineTop(content,
                                          lineHeight,
                                          lines.size(),
                                          lineIndex,
                                          usesFlexTextAlignment(node));
        addLineSelectionRect(selectionPath, x, y, right - x, lineHeight);
    }
    if (!selectionPath.isEmpty()) {
        canvas.drawPath(selectionPath.detach(), fill(SkColorSetARGB(96, 36, 232, 219)));
    }
    if (clipsOverflow) {
        canvas.restore();
    }
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
            drawStyledTextBlob(
                canvas,
                node,
                entry.blob,
                content.left() - node.scrollX,
                baseline,
                textColor);
        }
        canvas.restore();
        return;
    }

    const SkRect content = contentRectForText(node);
    SkColor textColor = node.style.color;
    if (inputPlaceholder) {
        textColor = SkColorSetA(textColor, static_cast<U8CPU>(std::min(150, static_cast<int>(SkColorGetA(textColor)))));
    }
    if (node.tag == "selectable") {
        const float lineHeight = lineHeightForNode(node);
        const std::vector<TextLine>& lines = textLines(node, *value);
        const bool clipsOverflow = clipsTextOverflow(node);
        if (clipsOverflow) {
            canvas.save();
            canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            const TextLine line = lines[i];
            if (line.end <= line.start) {
                continue;
            }
            const std::string_view lineText(value->data() + line.start, line.end - line.start);
            const TextEntry& entry = textEntry(lineText, node.style.fontSize, node.style.fontBold);
            const float baseline = selectableLineTop(content,
                                                     lineHeight,
                                                     lines.size(),
                                                     i,
                                                     usesFlexTextAlignment(node)) +
                                   lineHeight * 0.5f -
                                   (entry.metrics.fAscent + entry.metrics.fDescent) * 0.5f;
            const float lineX = textStartX(node, lineText);
            float segmentX = lineX;
            size_t segmentStart = line.start;
            for (const Node::TextLink& link : node.textLinks) {
                if (!rangesIntersect(line.start, line.end, link.start, link.end)) {
                    continue;
                }
                const size_t linkStart = std::max(line.start, link.start);
                const size_t linkEnd = std::min(line.end, link.end);
                if (segmentStart < linkStart) {
                    const std::string_view plain(value->data() + segmentStart, linkStart - segmentStart);
                    const TextEntry& plainEntry = textEntry(plain, node.style.fontSize, node.style.fontBold);
                    drawStyledTextBlob(
                        canvas,
                        node,
                        plainEntry.blob,
                        segmentX,
                        baseline,
                        textColor);
                    segmentX += plainEntry.width;
                }
                if (linkStart < linkEnd) {
                    const std::string_view linked(value->data() + linkStart, linkEnd - linkStart);
                    const TextEntry& linkedEntry = textEntry(linked, node.style.fontSize, node.style.fontBold);
                    const SkColor linkColor = SkColorSetRGB(11, 105, 183);
                    drawStyledTextBlob(
                        canvas,
                        node,
                        linkedEntry.blob,
                        segmentX,
                        baseline,
                        linkColor);
                    canvas.drawLine(segmentX,
                                    baseline + 2.0f,
                                    segmentX + linkedEntry.width,
                                    baseline + 2.0f,
                                    stroke(linkColor, 1.0f));
                    segmentX += linkedEntry.width;
                }
                segmentStart = linkEnd;
            }
            if (segmentStart < line.end) {
                const std::string_view plain(value->data() + segmentStart, line.end - segmentStart);
                const TextEntry& plainEntry = textEntry(plain, node.style.fontSize, node.style.fontBold);
                drawStyledTextBlob(
                    canvas,
                    node,
                    plainEntry.blob,
                    segmentX,
                    baseline,
                    textColor);
            }
        }
        if (clipsOverflow) {
            canvas.restore();
        }
        return;
    }

    const TextEntry& entry = textEntry(*value, node.style.fontSize, node.style.fontBold);
    const float x = textStartX(node, *value);
    const float y = content.top() + content.height() * 0.5f -
                    (entry.metrics.fAscent + entry.metrics.fDescent) * 0.5f;
    if (editable) {
        canvas.save();
        canvas.clipRect(node.layout.sk(), SkClipOp::kIntersect, true);
        drawStyledTextBlob(canvas, node, entry.blob, x, y, textColor);
        canvas.restore();
    } else {
        drawStyledTextBlob(canvas, node, entry.blob, x, y, textColor);
    }
}

void SkiaRenderer::drawStyledTextBlob(
    SkCanvas& canvas,
    const Node& node,
    const sk_sp<SkTextBlob>& blob,
    float x,
    float y,
    SkColor color) {
    for (const Shadow& shadow : node.style.textShadows) {
        const SkColor colorValue = shadowColor(shadow, node.style);
        if (SkColorGetA(colorValue) == 0) {
            continue;
        }
        SkPaint paint = fill(colorValue);
        const float sigma = shadowSigma(shadow.blurRadius);
        if (sigma > 0.0f) {
            paint.setMaskFilter(SkMaskFilter::MakeBlur(
                kNormal_SkBlurStyle,
                sigma));
        }
        canvas.drawTextBlob(
            blob,
            x + shadow.offsetX,
            y + shadow.offsetY,
            paint);
    }
    canvas.drawTextBlob(blob, x, y, fill(color));
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
    if (!isEditableNode(node) || !node.editingFocused || node.selectionStart != node.selectionEnd) {
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
    const SkRect content = contentRectForText(node);
    const float maxLineWidth = content.width();
    TextLineCacheEntry& entry = textLineCache_[&node];
    if (cacheableValue &&
        entry.revision == node.textRevision &&
        entry.value == &value &&
        entry.size == value.size() &&
        entry.maxWidth == maxLineWidth) {
        return entry.lines;
    }

    entry.revision = node.textRevision;
    entry.value = cacheableValue ? &value : nullptr;
    entry.size = value.size();
    entry.maxWidth = maxLineWidth;
    entry.lines.clear();

    const auto appendWrappedTextLines = [&](size_t start, size_t hardEnd) {
        size_t lineStart = start;
        while (lineStart < hardEnd) {
            const size_t lineEnd =
                findSelectableLineEnd(value,
                                      lineStart,
                                      hardEnd,
                                      maxLineWidth,
                                      [&](std::string_view text) {
                                          return textWidth(text,
                                                           node.style.fontSize,
                                                           node.style.fontBold);
                                      });
            entry.lines.push_back({lineStart, lineEnd});
            lineStart = lineEnd;
            while (lineStart < hardEnd &&
                   std::isspace(static_cast<unsigned char>(value[lineStart])) != 0) {
                ++lineStart;
            }
        }
        if (start == hardEnd) {
            entry.lines.push_back({hardEnd, hardEnd});
        }
    };

    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n') {
            if (node.tag == "selectable" || node.tag == "textarea") {
                appendWrappedTextLines(start, i);
            } else {
                entry.lines.push_back({start, i});
            }
            start = i + 1;
        }
    }
    if (node.tag == "selectable" || node.tag == "textarea") {
        appendWrappedTextLines(start, value.size());
    } else {
        entry.lines.push_back({start, value.size()});
    }
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
    if (usesFlexTextAlignment(node) && node.style.justifyContent == YGJustifyCenter) {
        x = content.left() + (availableWidth - width) * 0.5f;
    } else if (usesFlexTextAlignment(node) && node.style.justifyContent == YGJustifyFlexEnd) {
        x = content.right() - width;
    }
    return std::max(content.left(), x);
}

SkiaRenderer::TextHitResult SkiaRenderer::textHitAtPoint(const Node& node,
                                                         const std::string& value,
                                                         float x,
                                                         float y) {
    if (value.empty()) {
        return {};
    }

    const std::vector<TextLine>& lines = textLines(node, value);
    if (lines.empty()) {
        return {};
    }

    const SkRect content = contentRectForText(node);
    const float lineHeight = lineHeightForNode(node);
    float firstLineTop = content.top();
    if (usesFlexTextAlignment(node) && lines.size() <= 1) {
        firstLineTop = content.top() + (content.height() - lineHeight) * 0.5f;
    }
    const float relativeY = std::max(0.0f, y - firstLineTop);
    const size_t lineIndex = std::min(lines.size() - 1,
                                      static_cast<size_t>(relativeY / std::max(1.0f, lineHeight)));
    const TextLine line = lines[lineIndex];
    const std::string_view lineText(value.data() + line.start, line.end - line.start);
    const float lineX = textStartX(node, lineText);
    const float lineWidth = textWidth(lineText,
                                      node.style.fontSize,
                                      node.style.fontBold);
    TextHitResult result;
    result.index = line.start + textIndexAtOffset(lineText,
                                                  node.style.fontSize,
                                                  node.style.fontBold,
                                                  x - lineX);
    result.insideText = y >= firstLineTop &&
                        y < firstLineTop + lines.size() * lineHeight &&
                        x >= lineX &&
                        x <= lineX + lineWidth;
    return result;
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
