#include "skui_internal.h"

#include "perf_trace.h"

#include <algorithm>
#include <cctype>

namespace skui {
namespace {

float estimateTextWidth(std::string_view value, float fontSize) {
    float width = 0.0f;
    for (size_t i = 0; i < value.size();) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch < 0x80) {
            width += fontSize * (std::isspace(ch) ? 0.32f : 0.56f);
            ++i;
        } else if ((ch & 0xE0) == 0xC0) {
            width += fontSize * 0.92f;
            i += 2;
        } else if ((ch & 0xF0) == 0xE0) {
            width += fontSize * 1.02f;
            i += 3;
        } else if ((ch & 0xF8) == 0xF0) {
            width += fontSize * 1.02f;
            i += 4;
        } else {
            width += fontSize * 0.56f;
            ++i;
        }
    }
    return width;
}

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
    float measuredWidth = estimateTextWidth(value, uiNode->style.fontSize);
    float measuredHeight = uiNode->style.fontSize * 1.35f;
    if (widthMode == YGMeasureModeExactly) {
        measuredWidth = width;
    } else if (widthMode == YGMeasureModeAtMost) {
        measuredWidth = std::min(measuredWidth, width);
    }
    if (heightMode == YGMeasureModeExactly) {
        measuredHeight = height;
    } else if (heightMode == YGMeasureModeAtMost) {
        measuredHeight = std::min(measuredHeight, height);
    }
    return {std::max(0.0f, measuredWidth), std::max(0.0f, measuredHeight)};
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

}  // namespace

void updateScrollMetrics(Node& node) {
    if (node.virtualContentWidth > 0.0f && node.virtualContentHeight > 0.0f) {
        node.scrollContentWidth = std::max(node.layout.w, node.virtualContentWidth);
        node.scrollContentHeight = std::max(node.layout.h, node.virtualContentHeight);
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
    buildYoga(*document.root, rootYoga.get());
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

void LayoutEngine::buildYoga(Node& node, YGNodeRef yogaNode) {
    const Style& s = node.style;
    if (s.display == Display::None) {
        return;
    }

    YGNodeSetContext(yogaNode, &node);
    YGNodeStyleSetFlexDirection(yogaNode, s.flexDirection);
    YGNodeStyleSetFlexWrap(yogaNode, s.flexWrap);
    YGNodeStyleSetAlignItems(yogaNode, s.alignItems);
    YGNodeStyleSetJustifyContent(yogaNode, s.justifyContent);
    if (s.alignSelf != YGAlignAuto) {
        YGNodeStyleSetAlignSelf(yogaNode, s.alignSelf);
    }
    YGNodeStyleSetFlexGrow(yogaNode, s.flexGrow);
    YGNodeStyleSetFlexShrink(yogaNode, s.flexShrink);
    YGNodeStyleSetPositionType(yogaNode, s.position == Position::Absolute ? YGPositionTypeAbsolute : YGPositionTypeRelative);

    setOptional(yogaNode, YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto, s.width);
    setOptional(yogaNode, YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto, s.height);
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
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeLeft, s.inset.left);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeTop, s.inset.top);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeRight, s.inset.right);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGNodeStyleSetPositionPercent, YGNodeStyleSetPositionAuto, YGEdgeBottom, s.inset.bottom);

    const bool hasText = !node.text.empty() || !node.value.empty() || (node.tag == "input" && !node.placeholder.empty());
    if (node.children.empty() && hasText && !s.width && !s.height) {
        YGNodeSetMeasureFunc(yogaNode, measureTextNode);
    }

    for (auto& child : node.children) {
        if (child->style.display == Display::None) {
            continue;
        }
        YGNodeRef childRef = YGNodeNew();
        buildYoga(*child, childRef);
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
