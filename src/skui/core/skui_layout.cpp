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
    const YGFlexDirection flexDirection = s.flexDirection;
    YGNodeStyleSetFlexDirection(yogaNode, flexDirection);
    YGNodeStyleSetFlexWrap(yogaNode, s.flexWrap);
    YGNodeStyleSetAlignItems(yogaNode, s.alignItems);
    YGNodeStyleSetJustifyContent(yogaNode, s.justifyContent);
    if (s.alignSelf != YGAlignAuto) {
        YGNodeStyleSetAlignSelf(yogaNode, s.alignSelf);
    }
    YGNodeStyleSetFlexGrow(yogaNode, s.flexGrow);
    YGNodeStyleSetFlexShrink(yogaNode, s.flexShrink);
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
    const float borderWidth =
        s.borderStyle == BorderStyle::Solid
            ? std::max(0.0f, s.borderWidth)
            : 0.0f;
    YGNodeStyleSetBorder(yogaNode, YGEdgeAll, borderWidth);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeLeft, s.inset.left);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeTop, s.inset.top);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeRight, s.inset.right);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeBottom, s.inset.bottom);

    const bool hasText = !node.text.empty() || !node.value.empty() || (node.tag == "input" && !node.placeholder.empty());
    if (node.children.empty() && hasText && !s.width && !s.height) {
        YGNodeSetMeasureFunc(yogaNode, measureTextNode);
    }

    bool hasPreviousLayoutChild = false;
    for (auto& child : node.children) {
        if (child->style.display == Display::None) {
            continue;
        }
        YGNodeRef childRef = YGNodeNew();
        buildYoga(*child, childRef, false);
        if (s.display == Display::Flex &&
            hasPreviousLayoutChild &&
            (s.rowGap || s.columnGap)) {
            const bool isRow =
                flexDirection == YGFlexDirectionRow ||
                flexDirection == YGFlexDirectionRowReverse;
            setGapMargin(childRef,
                         isRow ? YGEdgeLeft : YGEdgeTop,
                         isRow ? s.columnGap : s.rowGap,
                         isRow ? child->style.margin.left : child->style.margin.top);
        }
        YGNodeInsertChild(yogaNode, childRef, YGNodeGetChildCount(yogaNode));
        hasPreviousLayoutChild = true;
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

    uint32_t yogaIndex = 0;
    for (auto& child : node.children) {
        if (child->style.display == Display::None) {
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
