#include "skui_internal.h"

#include "perf_trace.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

namespace skui {
namespace {

size_t utf8Advance(unsigned char ch) {
    if (ch < 0x80) {
        return 1;
    }
    if ((ch & 0xE0) == 0xC0) {
        return 2;
    }
    if ((ch & 0xF0) == 0xE0) {
        return 3;
    }
    if ((ch & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

size_t nextUtf8Boundary(std::string_view value, size_t index) {
    if (index >= value.size()) {
        return value.size();
    }
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    return std::min(value.size(), index + utf8Advance(ch));
}

template <typename MeasureText>
size_t findWrappedLineEnd(std::string_view value,
                          size_t start,
                          size_t hardEnd,
                          float maxWidth,
                          MeasureText measureText) {
    if (maxWidth <= 0.0f || start >= hardEnd) {
        return hardEnd;
    }

    std::vector<size_t> boundaries;
    boundaries.reserve(hardEnd - start);
    for (size_t index = start; index < hardEnd;) {
        index = nextUtf8Boundary(value, index);
        boundaries.push_back(index);
    }

    size_t low = 0;
    size_t high = boundaries.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const size_t candidateEnd = boundaries[middle];
        const std::string_view candidate(
            value.data() + start,
            candidateEnd - start);
        if (measureText(candidate) <= maxWidth) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == 0) {
        return boundaries.front();
    }

    const size_t lineEnd = boundaries[low - 1];
    if (lineEnd == hardEnd) {
        return lineEnd;
    }

    size_t lastBreak = std::string_view::npos;
    for (size_t index = start; index < lineEnd;) {
        const size_t next = nextUtf8Boundary(value, index);
        const unsigned char ch = static_cast<unsigned char>(value[index]);
        if (std::isspace(ch) != 0 ||
            value[index] == '/' ||
            value[index] == '-' ||
            value[index] == '_' ||
            value[index] == '?' ||
            value[index] == '&' ||
            value[index] == '=') {
            lastBreak = next;
        }
        index = next;
    }
    return lastBreak != std::string_view::npos && lastBreak > start
        ? lastBreak
        : lineEnd;
}

template <typename MeasureText>
size_t wrappedTextLineCount(std::string_view value,
                            float maxWidth,
                            MeasureText measureText) {
    size_t lineCount = 0;
    const auto countSegment = [&](size_t start, size_t hardEnd) {
        size_t lineStart = start;
        while (lineStart < hardEnd) {
            const size_t lineEnd = findWrappedLineEnd(
                value,
                lineStart,
                hardEnd,
                maxWidth,
                measureText);
            ++lineCount;
            lineStart = lineEnd;
            while (lineStart < hardEnd &&
                   std::isspace(
                       static_cast<unsigned char>(value[lineStart])) != 0) {
                ++lineStart;
            }
        }
    };

    size_t start = 0;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\n') {
            countSegment(start, index);
            start = index + 1;
        }
    }
    countSegment(start, value.size());
    return std::max<size_t>(1, lineCount);
}

struct TextareaScrollMetrics {
    float width = 0.0f;
    float height = 0.0f;
};

YGSize measureTextNode(YGNodeConstRef node,
                       float width,
                       YGMeasureMode widthMode,
                       float height,
                       YGMeasureMode heightMode) {
    const auto* uiNode = static_cast<const Node*>(YGNodeGetContext(node));
    if (!uiNode) {
        return {0.0f, 0.0f};
    }

    const std::string& value = !uiNode->value.empty()
        ? uiNode->value
        : (!uiNode->text.empty() ? uiNode->text : uiNode->placeholder);
    const auto measureText = [&](std::string_view text) {
        return measureUiTextWidth(
            text,
            uiNode->style.fontSize,
            uiNode->style.fontBold);
    };
    float measuredWidth = measureText(value);
    float measuredHeight = uiNode->style.fontSize * 1.35f;
    if (widthMode == YGMeasureModeExactly) {
        measuredWidth = width;
    } else if (widthMode == YGMeasureModeAtMost) {
        measuredWidth = std::min(measuredWidth, width);
    }
    if (uiNode->tag == "selectable") {
        const size_t lineCount = wrappedTextLineCount(
            value,
            measuredWidth,
            measureText);
        measuredHeight =
            static_cast<float>(lineCount) *
            std::max(12.0f, uiNode->style.fontSize * 1.38f);
    }
    if (heightMode == YGMeasureModeExactly) {
        measuredHeight = height;
    } else if (heightMode == YGMeasureModeAtMost) {
        measuredHeight = std::min(measuredHeight, height);
    }
    return {std::max(0.0f, measuredWidth), std::max(0.0f, measuredHeight)};
}

YGSize measureVideoNode(YGNodeConstRef node,
                        float width,
                        YGMeasureMode widthMode,
                        float height,
                        YGMeasureMode heightMode) {
    const auto* video = static_cast<const Node*>(YGNodeGetContext(node));
    if (!video || video->videoFrameWidth <= 0 || video->videoFrameHeight <= 0) {
        return {0.0f, 0.0f};
    }

    const float aspectRatio = static_cast<float>(video->videoFrameWidth) /
                              static_cast<float>(video->videoFrameHeight);
    float measuredWidth = static_cast<float>(video->videoFrameWidth);
    float measuredHeight = static_cast<float>(video->videoFrameHeight);
    if (widthMode == YGMeasureModeExactly) {
        measuredWidth = width;
        if (heightMode != YGMeasureModeExactly) {
            measuredHeight = measuredWidth / aspectRatio;
        }
    }
    if (heightMode == YGMeasureModeExactly) {
        measuredHeight = height;
        if (widthMode != YGMeasureModeExactly) {
            measuredWidth = measuredHeight * aspectRatio;
        }
    }
    if (widthMode == YGMeasureModeAtMost && measuredWidth > width) {
        measuredWidth = width;
        measuredHeight = measuredWidth / aspectRatio;
    }
    if (heightMode == YGMeasureModeAtMost && measuredHeight > height) {
        measuredHeight = height;
        measuredWidth = measuredHeight * aspectRatio;
    }
    return {std::max(0.0f, measuredWidth), std::max(0.0f, measuredHeight)};
}

float resolvedEdgeOrZero(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

size_t textLineCount(std::string_view value) {
    size_t count = 1;
    for (char ch : value) {
        if (ch == '\n') {
            ++count;
        }
    }
    return count;
}

TextareaScrollMetrics textareaScrollMetrics(const Node& node) {
    const std::string& value = !node.value.empty() ? node.value : node.placeholder;
    const float lineHeight = std::max(12.0f, node.style.fontSize * 1.38f);
    const float paddingTop = node.resolvedPadding.top;
    const float paddingBottom = node.resolvedPadding.bottom;
    const float paddingLeft = node.resolvedPadding.left;
    const float paddingRight = node.resolvedPadding.right;

    float maxLineWidth = 0.0f;
    size_t lines = 1;
    size_t lineStart = 0;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\n') {
            maxLineWidth = std::max(
                maxLineWidth,
                measureUiTextWidth(
                    std::string_view(
                        value.data() + lineStart,
                        index - lineStart),
                    node.style.fontSize,
                    node.style.fontBold));
            ++lines;
            lineStart = index + 1;
        }
    }
    maxLineWidth = std::max(
        maxLineWidth,
        measureUiTextWidth(
            std::string_view(
                value.data() + lineStart,
                value.size() - lineStart),
            node.style.fontSize,
            node.style.fontBold));
    return {
        paddingLeft + paddingRight + maxLineWidth,
        paddingTop + paddingBottom + static_cast<float>(lines) * lineHeight,
    };
}

bool needsIntrinsicMeasure(const std::optional<Length>& dimension) {
    return !dimension || dimension->unit == LengthUnit::Auto;
}

void setOptional(YGNodeRef node,
                 void (*setter)(YGNodeRef, float),
                 void (*percentSetter)(YGNodeRef, float),
                 void (*autoSetter)(YGNodeRef),
                 const std::optional<Length>& value) {
    if (value) {
        if (value->unit == LengthUnit::Percent && percentSetter) {
            percentSetter(node, value->value);
        } else if (value->unit == LengthUnit::Auto && autoSetter) {
            autoSetter(node);
        } else if (value->unit == LengthUnit::Px) {
            setter(node, value->value);
        }
    }
}

void setEdge(YGNodeRef node,
             void (*setter)(YGNodeRef, YGEdge, float),
             void (*percentSetter)(YGNodeRef, YGEdge, float),
             void (*autoSetter)(YGNodeRef, YGEdge),
             YGEdge edge,
             const std::optional<Length>& value) {
    if (value) {
        if (value->unit == LengthUnit::Percent && percentSetter) {
            percentSetter(node, edge, value->value);
        } else if (value->unit == LengthUnit::Auto && autoSetter) {
            autoSetter(node, edge);
        } else if (value->unit == LengthUnit::Px) {
            setter(node, edge, value->value);
        }
    }
}

void setGapMargin(YGNodeRef node,
                  YGEdge edge,
                  const std::optional<Length>& gap,
                  const std::optional<Length>& existingMargin) {
    if (!gap || gap->unit == LengthUnit::Auto) {
        return;
    }
    const float existing =
        existingMargin && existingMargin->unit == gap->unit
            ? existingMargin->value
            : 0.0f;
    const float value = existing + gap->value;
    if (gap->unit == LengthUnit::Percent) {
        YGNodeStyleSetMarginPercent(node, edge, value);
    } else {
        YGNodeStyleSetMargin(node, edge, value);
    }
}

void setFlexGap(YGNodeRef node,
                YGGutter gutter,
                const std::optional<Length>& gap) {
    if (!gap || gap->unit == LengthUnit::Auto) {
        return;
    }
    if (gap->unit == LengthUnit::Percent) {
        YGNodeStyleSetGapPercent(node, gutter, gap->value);
    } else {
        YGNodeStyleSetGap(node, gutter, gap->value);
    }
}

void setFlexBasis(YGNodeRef node, const std::optional<Length>& basis) {
    if (!basis) {
        return;
    }
    if (basis->unit == LengthUnit::Percent) {
        YGNodeStyleSetFlexBasisPercent(node, basis->value);
    } else if (basis->unit == LengthUnit::Auto) {
        YGNodeStyleSetFlexBasisAuto(node);
    } else {
        YGNodeStyleSetFlexBasis(node, basis->value);
    }
}

bool allTracksAreFractional(const std::vector<GridTrack>& tracks) {
    return !tracks.empty() &&
           std::all_of(tracks.begin(), tracks.end(), [](const GridTrack& track) {
               return track.kind == GridTrackKind::Fraction;
           });
}

struct ResolvedGridColumn {
    std::optional<size_t> start;
    size_t span = 1;
};

struct PositionedGridItem {
    Node* node = nullptr;
    size_t row = 0;
    size_t column = 0;
    size_t columnSpan = 1;
};

struct GridPlacementPlan {
    std::vector<PositionedGridItem> items;
    size_t cellCount = 0;
};

std::optional<size_t> resolveGridLineIndex(const GridLine& line,
                                           size_t columnCount) {
    if (line.kind != GridLineKind::Index) {
        return std::nullopt;
    }

    const int lineCount = static_cast<int>(columnCount) + 1;
    const int index = line.value > 0
        ? line.value - 1
        : lineCount + line.value;
    if (index < 0 || index > static_cast<int>(columnCount)) {
        return std::nullopt;
    }
    return static_cast<size_t>(index);
}

ResolvedGridColumn resolveGridColumn(const Style& style,
                                     size_t columnCount) {
    ResolvedGridColumn result;
    size_t span = 1;
    if (style.gridColumnStart.kind == GridLineKind::Span) {
        span = static_cast<size_t>(style.gridColumnStart.value);
    }
    if (style.gridColumnEnd.kind == GridLineKind::Span) {
        span = static_cast<size_t>(style.gridColumnEnd.value);
    }
    span = std::clamp(span, size_t{1}, columnCount);

    const std::optional<size_t> start = resolveGridLineIndex(
        style.gridColumnStart,
        columnCount);
    const std::optional<size_t> end = resolveGridLineIndex(
        style.gridColumnEnd,
        columnCount);
    if (start && end && *end > *start) {
        result.start = std::min(*start, columnCount - 1);
        result.span = std::min(*end - *start, columnCount - *result.start);
        return result;
    }
    if (start) {
        result.start = std::min(*start, columnCount - 1);
        result.span = std::min(span, columnCount - *result.start);
        return result;
    }
    if (end) {
        result.span = span;
        result.start = *end >= span ? *end - span : 0;
        result.start = std::min(*result.start, columnCount - result.span);
        return result;
    }

    result.span = span;
    return result;
}

bool gridCellsAreFree(const std::vector<bool>& occupied,
                      size_t row,
                      size_t column,
                      size_t span,
                      size_t columnCount) {
    const size_t first = row * columnCount + column;
    for (size_t offset = 0; offset < span; ++offset) {
        const size_t index = first + offset;
        if (index < occupied.size() && occupied[index]) {
            return false;
        }
    }
    return true;
}

GridPlacementPlan positionGridItems(const std::vector<Node*>& children,
                                    size_t columnCount) {
    GridPlacementPlan plan;
    std::vector<bool> occupied;
    size_t autoCursor = 0;

    for (Node* child : children) {
        const ResolvedGridColumn requested = resolveGridColumn(
            child->style,
            columnCount);
        size_t row = 0;
        size_t column = 0;
        if (requested.start) {
            column = *requested.start;
            while (!gridCellsAreFree(
                occupied,
                row,
                column,
                requested.span,
                columnCount)) {
                ++row;
            }
        } else {
            size_t candidate = autoCursor;
            while (true) {
                row = candidate / columnCount;
                column = candidate % columnCount;
                if (column + requested.span <= columnCount &&
                    gridCellsAreFree(
                        occupied,
                        row,
                        column,
                        requested.span,
                        columnCount)) {
                    break;
                }
                ++candidate;
            }
            autoCursor = row * columnCount + column + requested.span;
        }

        const size_t first = row * columnCount + column;
        const size_t end = first + requested.span;
        occupied.resize(std::max(occupied.size(), end), false);
        std::fill(occupied.begin() + first, occupied.begin() + end, true);
        plan.items.push_back({child, row, column, requested.span});
        plan.cellCount = std::max(plan.cellCount, end);
    }
    return plan;
}

void applySingleColumnTrack(YGNodeRef cell, const GridTrack& track) {
    if (track.kind == GridTrackKind::Fixed) {
        if (track.length.unit == LengthUnit::Percent) {
            YGNodeStyleSetWidthPercent(cell, track.length.value);
        } else {
            YGNodeStyleSetWidth(cell, track.length.value);
        }
        YGNodeStyleSetFlexGrow(cell, 0.0f);
        YGNodeStyleSetFlexShrink(cell, 0.0f);
    } else if (track.kind == GridTrackKind::Fraction) {
        YGNodeStyleSetFlexBasis(cell, 0.0f);
        YGNodeStyleSetFlexGrow(cell, track.fraction);
        YGNodeStyleSetFlexShrink(cell, 1.0f);
    } else {
        YGNodeStyleSetFlexBasisAuto(cell);
        YGNodeStyleSetFlexGrow(cell, 0.0f);
        YGNodeStyleSetFlexShrink(cell, 0.0f);
    }
}

void applyGridColumnTrack(YGNodeRef cell,
                          const Style& gridStyle,
                          size_t column,
                          size_t span) {
    const std::vector<GridTrack>& tracks = gridStyle.gridTemplateColumns;
    if (tracks.empty() || column >= tracks.size()) {
        return;
    }
    span = std::min(span, tracks.size() - column);

    if (allTracksAreFractional(tracks)) {
        float totalFraction = 0.0f;
        float itemFraction = 0.0f;
        for (size_t index = 0; index < tracks.size(); ++index) {
            totalFraction += tracks[index].fraction;
            if (index >= column && index < column + span) {
                itemFraction += tracks[index].fraction;
            }
        }
        if (totalFraction > 0.0f) {
            YGNodeStyleSetWidthPercent(
                cell,
                itemFraction / totalFraction * 100.0f);
            YGNodeStyleSetFlexGrow(cell, 0.0f);
            YGNodeStyleSetFlexShrink(cell, 0.0f);
        }
        return;
    }
    if (span == 1) {
        applySingleColumnTrack(cell, tracks[column]);
        return;
    }

    const LengthUnit unit = tracks[column].length.unit;
    const bool fixedWithSameUnit = std::all_of(
        tracks.begin() + static_cast<std::ptrdiff_t>(column),
        tracks.begin() + static_cast<std::ptrdiff_t>(column + span),
        [unit](const GridTrack& track) {
            return track.kind == GridTrackKind::Fixed &&
                   track.length.unit == unit;
        });
    if (!fixedWithSameUnit) {
        applySingleColumnTrack(cell, tracks[column]);
        return;
    }

    float extent = 0.0f;
    for (size_t index = column; index < column + span; ++index) {
        extent += tracks[index].length.value;
    }
    if (gridStyle.columnGap && gridStyle.columnGap->unit == unit) {
        extent += gridStyle.columnGap->value * static_cast<float>(span - 1);
    }
    if (unit == LengthUnit::Percent) {
        YGNodeStyleSetWidthPercent(cell, extent);
    } else {
        YGNodeStyleSetWidth(cell, extent);
    }
    YGNodeStyleSetFlexGrow(cell, 0.0f);
    YGNodeStyleSetFlexShrink(cell, 0.0f);
}

void applyGridRowTrack(YGNodeRef cell,
                       const Style& gridStyle,
                       size_t row,
                       size_t rowCount) {
    const GridTrack* track = nullptr;
    if (row < gridStyle.gridTemplateRows.size()) {
        track = &gridStyle.gridTemplateRows[row];
    } else if (gridStyle.gridAutoRows) {
        track = &*gridStyle.gridAutoRows;
    }

    if (!track) {
        if (gridStyle.gridTemplateRows.empty() && rowCount == 1) {
            YGNodeStyleSetHeightPercent(cell, 100.0f);
        }
        return;
    }
    if (track->kind == GridTrackKind::Fixed) {
        if (track->length.unit == LengthUnit::Percent) {
            YGNodeStyleSetHeightPercent(cell, track->length.value);
        } else {
            YGNodeStyleSetHeight(cell, track->length.value);
        }
        return;
    }
    if (track->kind != GridTrackKind::Fraction) {
        return;
    }

    if (!gridStyle.gridTemplateRows.empty() &&
        allTracksAreFractional(gridStyle.gridTemplateRows)) {
        float totalFraction = 0.0f;
        for (const GridTrack& rowTrack : gridStyle.gridTemplateRows) {
            totalFraction += rowTrack.fraction;
        }
        if (totalFraction > 0.0f) {
            YGNodeStyleSetHeightPercent(
                cell,
                track->fraction / totalFraction * 100.0f);
        }
    } else if (gridStyle.gridTemplateRows.empty() && rowCount > 0) {
        YGNodeStyleSetHeightPercent(
            cell,
            100.0f / static_cast<float>(rowCount));
    }
}

YGJustify gridItemJustification(GridItemAlignment alignment) {
    if (alignment == GridItemAlignment::Center) {
        return YGJustifyCenter;
    }
    if (alignment == GridItemAlignment::End) {
        return YGJustifyFlexEnd;
    }
    return YGJustifyFlexStart;
}

GridItemAlignment resolvedGridItemAlignment(const Style& gridStyle,
                                            const Style& childStyle) {
    const GridItemAlignment alignment =
        childStyle.justifySelf == GridItemAlignment::Auto
            ? gridStyle.justifyItems
            : childStyle.justifySelf;
    return alignment == GridItemAlignment::Auto
        ? GridItemAlignment::Stretch
        : alignment;
}

}  // namespace

void updateScrollMetrics(Node& node) {
    if (node.virtualContentWidth > 0.0f && node.virtualContentHeight > 0.0f) {
        node.scrollContentWidth = std::max(node.layout.w, node.virtualContentWidth);
        node.scrollContentHeight = std::max(node.layout.h, node.virtualContentHeight);
        node.scrollX = clampf(node.scrollX, 0.0f, scrollMaxX(node));
        node.scrollY = clampf(node.scrollY, 0.0f, scrollMaxY(node));
        return;
    }

    if (node.tag == "textarea") {
        const TextareaScrollMetrics metrics = textareaScrollMetrics(node);
        node.scrollContentWidth = std::max(node.layout.w, metrics.width);
        node.scrollContentHeight = std::max(node.layout.h, metrics.height);
        node.scrollX = clampf(node.scrollX, 0.0f, scrollMaxX(node));
        node.scrollY = clampf(node.scrollY, 0.0f, scrollMaxY(node));
        return;
    }

    float minLeft = 0.0f;
    float minTop = 0.0f;
    float maxRight = node.layout.w;
    float maxBottom = node.layout.h;
    bool hasChild = false;

    for (auto& child : node.children) {
        if (child->style.display == Display::None) {
            continue;
        }
        updateScrollMetrics(*child);
        const float childLeft = child->layout.x - node.layout.x;
        const float childTop = child->layout.y - node.layout.y;
        const float childRight = childLeft + child->layout.w;
        const float childBottom = childTop + child->layout.h;
        if (!hasChild) {
            minLeft = std::min(0.0f, childLeft);
            minTop = std::min(0.0f, childTop);
            maxRight = std::max(node.layout.w, childRight);
            maxBottom = std::max(node.layout.h, childBottom);
            hasChild = true;
        } else {
            minLeft = std::min(minLeft, childLeft);
            minTop = std::min(minTop, childTop);
            maxRight = std::max(maxRight, childRight);
            maxBottom = std::max(maxBottom, childBottom);
        }
    }

    const float measuredContentWidth = maxRight - minLeft;
    const float measuredContentHeight = maxBottom - minTop;
    node.scrollContentWidth = node.virtualContentWidth > 0.0f
        ? std::max(node.layout.w, node.virtualContentWidth)
        : std::max(node.layout.w, measuredContentWidth);
    node.scrollContentHeight = node.virtualContentHeight > 0.0f
        ? std::max(node.layout.h, node.virtualContentHeight)
        : std::max(node.layout.h, measuredContentHeight);
    node.scrollX = clampf(node.scrollX, 0.0f, scrollMaxX(node));
    node.scrollY = clampf(node.scrollY, 0.0f, scrollMaxY(node));
}

void LayoutEngine::layout(Document& document, float width, float height) {
    if (!document.root) {
        return;
    }

    const bool traceEnabled = perf::Trace::enabled();
    const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    YogaNode rootYoga;
    YGNodeStyleSetWidth(rootYoga.get(), std::max(1.0f, width));
    YGNodeStyleSetHeight(rootYoga.get(), std::max(1.0f, height));
    const auto buildStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    buildYoga(*document.root, rootYoga.get(), true);
    if (traceEnabled) {
        perf::Trace::write("skui_layout", "build_yoga", static_cast<int>(width), static_cast<int>(height), perf::Trace::elapsedMs(buildStart));
    }
    YGNodeStyleSetWidth(rootYoga.get(), std::max(1.0f, width));
    YGNodeStyleSetHeight(rootYoga.get(), std::max(1.0f, height));
    const auto calculateStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    YGNodeCalculateLayout(rootYoga.get(), std::max(1.0f, width), std::max(1.0f, height), YGDirectionLTR);
    if (traceEnabled) {
        perf::Trace::write("skui_layout", "calculate_yoga", static_cast<int>(width), static_cast<int>(height), perf::Trace::elapsedMs(calculateStart));
    }
    const auto readStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    readYoga(*document.root, rootYoga.get(), 0.0f, 0.0f);
    if (traceEnabled) {
        perf::Trace::write("skui_layout", "read_yoga", static_cast<int>(width), static_cast<int>(height), perf::Trace::elapsedMs(readStart));
    }
    const auto scrollStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
    updateScrollMetrics(*document.root);
    if (traceEnabled) {
        perf::Trace::write("skui_layout", "scroll_metrics", static_cast<int>(width), static_cast<int>(height), perf::Trace::elapsedMs(scrollStart));
        perf::Trace::write("skui_layout", "layout_total", static_cast<int>(width), static_cast<int>(height), perf::Trace::elapsedMs(traceStart));
    }
}

void LayoutEngine::buildYoga(Node& node, YGNodeRef yogaNode, bool isRoot) {
    const Style& s = node.style;
    if (s.display == Display::None) {
        return;
    }

    YGNodeSetContext(yogaNode, &node);
    const bool isGrid = s.display == Display::Grid;
    const bool usesGridCells = isGrid && !s.gridTemplateColumns.empty();
    const YGFlexDirection flexDirection = isGrid
        ? YGFlexDirectionRow
        : s.flexDirection;
    YGNodeStyleSetFlexDirection(yogaNode, flexDirection);
    YGNodeStyleSetFlexWrap(yogaNode, isGrid ? YGWrapWrap : s.flexWrap);
    if (!isGrid) {
        setFlexGap(yogaNode, YGGutterRow, s.rowGap);
        setFlexGap(yogaNode, YGGutterColumn, s.columnGap);
    }
    YGNodeStyleSetAlignItems(
        yogaNode,
        usesGridCells ? YGAlignStretch : s.alignItems);
    YGNodeStyleSetJustifyContent(yogaNode, s.justifyContent);
    if (s.alignSelf != YGAlignAuto) {
        YGNodeStyleSetAlignSelf(yogaNode, s.alignSelf);
    }
    YGNodeStyleSetFlexGrow(yogaNode, s.flexGrow);
    YGNodeStyleSetFlexShrink(yogaNode, s.flexShrink);
    setFlexBasis(yogaNode, s.flexBasis);
    const bool hasSpecifiedBoxDimension =
        s.width.has_value() ||
        s.height.has_value() ||
        s.minWidth.has_value() ||
        s.minHeight.has_value() ||
        s.maxWidth.has_value() ||
        s.maxHeight.has_value();
    if (s.flags.boxSizing || hasSpecifiedBoxDimension) {
        YGNodeStyleSetBoxSizing(yogaNode, s.boxSizing);
    }
    YGNodeStyleSetPositionType(yogaNode,
                               (s.position == Position::Absolute ||
                                s.position == Position::Sticky)
                                   ? YGPositionTypeAbsolute
                                   : YGPositionTypeRelative);

    if (!isRoot) {
        setOptional(yogaNode, YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto, s.width);
        setOptional(yogaNode, YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto, s.height);
    }
    setOptional(yogaNode, YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent, nullptr, s.minWidth);
    setOptional(yogaNode, YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent, nullptr, s.minHeight);
    setOptional(yogaNode, YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent, nullptr, s.maxWidth);
    setOptional(yogaNode, YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent, nullptr, s.maxHeight);

    setEdge(yogaNode, YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent, YGNodeStyleSetMarginAuto, YGEdgeLeft, s.margin.left);
    setEdge(yogaNode, YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent, YGNodeStyleSetMarginAuto, YGEdgeTop, s.margin.top);
    setEdge(yogaNode, YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent, YGNodeStyleSetMarginAuto, YGEdgeRight, s.margin.right);
    setEdge(yogaNode, YGNodeStyleSetMargin, YGNodeStyleSetMarginPercent, YGNodeStyleSetMarginAuto, YGEdgeBottom, s.margin.bottom);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGNodeStyleSetPaddingPercent, nullptr, YGEdgeLeft, s.padding.left);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGNodeStyleSetPaddingPercent, nullptr, YGEdgeTop, s.padding.top);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGNodeStyleSetPaddingPercent, nullptr, YGEdgeRight, s.padding.right);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGNodeStyleSetPaddingPercent, nullptr, YGEdgeBottom, s.padding.bottom);
    const auto borderWidth = [](const BorderSide& side) {
        return side.style.value_or(BorderStyle::None) == BorderStyle::Solid
            ? std::max(0.0f, side.width.value_or(0.0f))
            : 0.0f;
    };
    YGNodeStyleSetBorder(yogaNode, YGEdgeLeft, borderWidth(s.borders.left));
    YGNodeStyleSetBorder(yogaNode, YGEdgeTop, borderWidth(s.borders.top));
    YGNodeStyleSetBorder(yogaNode, YGEdgeRight, borderWidth(s.borders.right));
    YGNodeStyleSetBorder(yogaNode, YGEdgeBottom, borderWidth(s.borders.bottom));
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeLeft, s.inset.left);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeTop, s.inset.top);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeRight, s.inset.right);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeBottom, s.inset.bottom);

    const bool contentEditableTextNode = isContentEditableTextNode(node);
    const bool hasText = !node.text.empty() ||
                         !node.value.empty() ||
                         (node.tag == "input" && !node.placeholder.empty()) ||
                         contentEditableTextNode;
    const bool needsTextMeasure = needsIntrinsicMeasure(s.width) ||
                                  needsIntrinsicMeasure(s.height);
    const bool hasLayoutChildren = std::any_of(
        node.children.begin(),
        node.children.end(),
        [contentEditableTextNode](const std::unique_ptr<Node>& child) {
            return !(contentEditableTextNode && child->tag == "br") &&
                   child->style.display != Display::None;
        });
    if (!hasLayoutChildren && hasText && needsTextMeasure) {
        YGNodeSetMeasureFunc(yogaNode, measureTextNode);
    } else if (!hasLayoutChildren && node.tag == "video" &&
               node.videoFrameWidth > 0 &&
               node.videoFrameHeight > 0) {
        YGNodeSetMeasureFunc(yogaNode, measureVideoNode);
    }

    if (usesGridCells) {
        std::vector<Node*> layoutChildren;
        layoutChildren.reserve(node.children.size());
        for (auto& child : node.children) {
            if (child->style.display != Display::None &&
                !(contentEditableTextNode && child->tag == "br")) {
                layoutChildren.push_back(child.get());
            }
        }

        const size_t columnCount = s.gridTemplateColumns.size();
        const GridPlacementPlan plan = positionGridItems(
            layoutChildren,
            columnCount);
        const size_t rowCount = plan.cellCount == 0
            ? 0
            : (plan.cellCount + columnCount - 1) / columnCount;
        std::vector<const PositionedGridItem*> itemStarts(
            plan.cellCount,
            nullptr);
        for (const PositionedGridItem& item : plan.items) {
            itemStarts[item.row * columnCount + item.column] = &item;
        }

        for (size_t cellIndex = 0; cellIndex < plan.cellCount;) {
            const PositionedGridItem* item = itemStarts[cellIndex];
            const size_t row = cellIndex / columnCount;
            const size_t column = cellIndex % columnCount;
            const size_t span = item ? item->columnSpan : 1;
            YGNodeRef cellRef = YGNodeNew();
            YGNodeStyleSetFlexDirection(cellRef, YGFlexDirectionRow);
            YGNodeStyleSetAlignItems(cellRef, s.alignItems);
            applyGridColumnTrack(cellRef, s, column, span);
            applyGridRowTrack(cellRef, s, row, rowCount);
            if (column > 0) {
                setGapMargin(cellRef, YGEdgeLeft, s.columnGap, std::nullopt);
            }
            if (row > 0) {
                setGapMargin(cellRef, YGEdgeTop, s.rowGap, std::nullopt);
            }

            if (item) {
                YGNodeSetContext(cellRef, item->node);
                YGNodeRef childRef = YGNodeNew();
                buildYoga(*item->node, childRef, false);
                const GridItemAlignment alignment =
                    resolvedGridItemAlignment(s, item->node->style);
                YGNodeStyleSetJustifyContent(
                    cellRef,
                    gridItemJustification(alignment));

                // Grid item 的 flex 属性不参与单元格内对齐；stretch 仅在宽度为 auto 时填满轨道。
                YGNodeStyleSetFlexGrow(childRef, 0.0f);
                YGNodeStyleSetFlexShrink(childRef, 0.0f);
                if (alignment == GridItemAlignment::Stretch &&
                    needsIntrinsicMeasure(item->node->style.width)) {
                    YGNodeStyleSetFlexBasis(childRef, 0.0f);
                    YGNodeStyleSetFlexGrow(childRef, 1.0f);
                    YGNodeStyleSetFlexShrink(childRef, 1.0f);
                }
                YGNodeInsertChild(cellRef, childRef, 0);
            }

            YGNodeInsertChild(
                yogaNode,
                cellRef,
                YGNodeGetChildCount(yogaNode));
            cellIndex += span;
        }
        return;
    }

    for (auto& child : node.children) {
        if (child->style.display == Display::None ||
            (contentEditableTextNode && child->tag == "br")) {
            continue;
        }
        YGNodeRef childRef = YGNodeNew();
        buildYoga(*child, childRef, false);
        YGNodeInsertChild(yogaNode, childRef, YGNodeGetChildCount(yogaNode));
    }
}

void LayoutEngine::readYoga(Node& node, YGNodeRef yogaNode, float offsetX, float offsetY) {
    node.layout = {
        offsetX + YGNodeLayoutGetLeft(yogaNode),
        offsetY + YGNodeLayoutGetTop(yogaNode),
        YGNodeLayoutGetWidth(yogaNode),
        YGNodeLayoutGetHeight(yogaNode),
    };
    node.resolvedPadding = {
        resolvedEdgeOrZero(YGNodeLayoutGetPadding(yogaNode, YGEdgeLeft)),
        resolvedEdgeOrZero(YGNodeLayoutGetPadding(yogaNode, YGEdgeTop)),
        resolvedEdgeOrZero(YGNodeLayoutGetPadding(yogaNode, YGEdgeRight)),
        resolvedEdgeOrZero(YGNodeLayoutGetPadding(yogaNode, YGEdgeBottom)),
    };

    if (node.style.display == Display::Grid &&
        !node.style.gridTemplateColumns.empty()) {
        const uint32_t cellCount = YGNodeGetChildCount(yogaNode);
        for (uint32_t index = 0; index < cellCount; ++index) {
            YGNodeRef cellYoga = YGNodeGetChild(yogaNode, index);
            auto* child = static_cast<Node*>(YGNodeGetContext(cellYoga));
            if (!child || YGNodeGetChildCount(cellYoga) == 0) {
                continue;
            }
            YGNodeRef childYoga = YGNodeGetChild(cellYoga, 0);
            readYoga(
                *child,
                childYoga,
                node.layout.x + YGNodeLayoutGetLeft(cellYoga),
                node.layout.y + YGNodeLayoutGetTop(cellYoga));
        }
        return;
    }

    uint32_t yogaIndex = 0;
    const bool contentEditableTextNode = isContentEditableTextNode(node);
    for (auto& child : node.children) {
        if (child->style.display == Display::None ||
            (contentEditableTextNode && child->tag == "br")) {
            continue;
        }
        YGNodeRef childYoga = YGNodeGetChild(yogaNode, yogaIndex++);
        if (!childYoga) {
            continue;
        }
        readYoga(*child, childYoga, node.layout.x, node.layout.y);
    }
}

}  // namespace skui
