#include "skui_internal.h"

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

    const std::string& value = !uiNode->value.empty() ? uiNode->value : uiNode->text;
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

void setOptional(YGNodeRef node, void (*setter)(YGNodeRef, float), const std::optional<float>& value) {
    if (value) {
        setter(node, *value);
    }
}

void setEdge(YGNodeRef node,
             void (*setter)(YGNodeRef, YGEdge, float),
             YGEdge edge,
             const std::optional<float>& value) {
    if (value) {
        setter(node, edge, *value);
    }
}

}  // namespace

void LayoutEngine::layout(Document& document, float width, float height) {
    if (!document.root) {
        return;
    }

    std::vector<std::unique_ptr<YogaNode>> owned;
    owned.reserve(256);

    YogaNode rootYoga;
    YGNodeStyleSetWidth(rootYoga.get(), std::max(1.0f, width));
    YGNodeStyleSetHeight(rootYoga.get(), std::max(1.0f, height));
    buildYoga(*document.root, rootYoga.get(), owned);
    YGNodeCalculateLayout(rootYoga.get(), std::max(1.0f, width), std::max(1.0f, height), YGDirectionLTR);
    readYoga(*document.root, rootYoga.get(), 0.0f, 0.0f);
}

void LayoutEngine::buildYoga(Node& node, YGNodeRef yogaNode, std::vector<std::unique_ptr<YogaNode>>& owned) {
    const Style& s = node.style;
    if (s.display == Display::None) {
        return;
    }

    YGNodeSetContext(yogaNode, &node);
    YGNodeStyleSetFlexDirection(yogaNode, s.flexDirection);
    YGNodeStyleSetAlignItems(yogaNode, s.alignItems);
    YGNodeStyleSetJustifyContent(yogaNode, s.justifyContent);
    if (s.alignSelf != YGAlignAuto) {
        YGNodeStyleSetAlignSelf(yogaNode, s.alignSelf);
    }
    YGNodeStyleSetFlexGrow(yogaNode, s.flexGrow);
    YGNodeStyleSetFlexShrink(yogaNode, s.flexShrink);
    YGNodeStyleSetPositionType(yogaNode, s.position == Position::Absolute ? YGPositionTypeAbsolute : YGPositionTypeRelative);

    setOptional(yogaNode, YGNodeStyleSetWidth, s.width);
    setOptional(yogaNode, YGNodeStyleSetHeight, s.height);
    setOptional(yogaNode, YGNodeStyleSetMinWidth, s.minWidth);
    setOptional(yogaNode, YGNodeStyleSetMinHeight, s.minHeight);
    setOptional(yogaNode, YGNodeStyleSetMaxWidth, s.maxWidth);
    setOptional(yogaNode, YGNodeStyleSetMaxHeight, s.maxHeight);

    setEdge(yogaNode, YGNodeStyleSetMargin, YGEdgeLeft, s.margin.left);
    setEdge(yogaNode, YGNodeStyleSetMargin, YGEdgeTop, s.margin.top);
    setEdge(yogaNode, YGNodeStyleSetMargin, YGEdgeRight, s.margin.right);
    setEdge(yogaNode, YGNodeStyleSetMargin, YGEdgeBottom, s.margin.bottom);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGEdgeLeft, s.padding.left);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGEdgeTop, s.padding.top);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGEdgeRight, s.padding.right);
    setEdge(yogaNode, YGNodeStyleSetPadding, YGEdgeBottom, s.padding.bottom);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGEdgeLeft, s.inset.left);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGEdgeTop, s.inset.top);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGEdgeRight, s.inset.right);
    setEdge(yogaNode, YGNodeStyleSetPosition, YGEdgeBottom, s.inset.bottom);

    const bool hasText = !node.text.empty() || !node.value.empty();
    if (node.children.empty() && hasText && !s.width && !s.height) {
        YGNodeSetMeasureFunc(yogaNode, measureTextNode);
    }

    for (auto& child : node.children) {
        if (child->style.display == Display::None) {
            continue;
        }
        auto childYoga = std::make_unique<YogaNode>();
        YGNodeRef childRef = childYoga->get();
        buildYoga(*child, childRef, owned);
        YGNodeInsertChild(yogaNode, childRef, YGNodeGetChildCount(yogaNode));
        owned.push_back(std::move(childYoga));
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
