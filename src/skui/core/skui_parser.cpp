#include "skui_internal.h"

#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/parser.h>
#include <lexbor/html/serialize.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace skui {
namespace {

struct HtmlDocumentDeleter {
    void operator()(lxb_html_document_t* document) const {
        if (document) {
            lxb_html_document_destroy(document);
        }
    }
};

using HtmlDocumentPtr = std::unique_ptr<lxb_html_document_t, HtmlDocumentDeleter>;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parseFloat(std::string_view raw, float& out) {
    std::string value = trim(raw);
    if (value.ends_with("px")) {
        value.resize(value.size() - 2);
    }
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

std::optional<float> parseLength(std::string_view raw) {
    float value = 0.0f;
    if (!parseFloat(raw, value)) {
        return std::nullopt;
    }
    return value;
}

std::string attr(lxb_dom_element_t* element, const char* name) {
    if (!element || !name) {
        return {};
    }
    size_t len = 0;
    const lxb_char_t* value = lxb_dom_element_get_attribute(element,
                                                            reinterpret_cast<const lxb_char_t*>(name),
                                                            std::char_traits<char>::length(name),
                                                            &len);
    if (!value || len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(value), len);
}

std::string nodeName(lxb_dom_element_t* element) {
    size_t len = 0;
    const lxb_char_t* value = lxb_dom_element_local_name(element, &len);
    if (!value || len == 0) {
        return {};
    }
    return lower(std::string(reinterpret_cast<const char*>(value), len));
}

lxb_status_t appendSerialized(const lxb_char_t* data, size_t len, void* ctx) {
    auto* out = static_cast<std::string*>(ctx);
    out->append(reinterpret_cast<const char*>(data), len);
    return LXB_STATUS_OK;
}

std::string serializeTree(lxb_dom_node_t* node) {
    if (!node) {
        return {};
    }
    std::string out;
    if (lxb_html_serialize_tree_cb(node, appendSerialized, &out) != LXB_STATUS_OK) {
        return {};
    }
    return out;
}

bool isWhitespaceOnly(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

void appendText(Node& node, std::string_view text) {
    if (isWhitespaceOnly(text)) {
        return;
    }
    std::string compact;
    compact.reserve(text.size());
    bool previousSpace = false;
    for (char ch : text) {
        const bool space = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (space) {
            if (!previousSpace && !compact.empty()) {
                compact.push_back(' ');
            }
        } else {
            compact.push_back(ch);
        }
        previousSpace = space;
    }
    if (!compact.empty()) {
        if (!node.text.empty() && node.text.back() != ' ') {
            node.text.push_back(' ');
        }
        node.text += compact;
    }
}

void setEdges(EdgeValues& edges,
              Style::Flags& flags,
              bool Style::Flags::*leftFlag,
              bool Style::Flags::*topFlag,
              bool Style::Flags::*rightFlag,
              bool Style::Flags::*bottomFlag,
              float value) {
    edges.left = value;
    edges.top = value;
    edges.right = value;
    edges.bottom = value;
    flags.*leftFlag = true;
    flags.*topFlag = true;
    flags.*rightFlag = true;
    flags.*bottomFlag = true;
}

void setEdgeByName(EdgeValues& edges,
                   Style::Flags& flags,
                   bool Style::Flags::*flag,
                   std::optional<float> EdgeValues::*field,
                   float value) {
    edges.*field = value;
    flags.*flag = true;
}

std::vector<std::string> splitCssTokens(std::string_view raw) {
    std::vector<std::string> tokens;
    size_t start = std::string_view::npos;
    int parenDepth = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (ch == '(') {
            ++parenDepth;
        } else if (ch == ')' && parenDepth > 0) {
            --parenDepth;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) != 0 && parenDepth == 0) {
            if (start != std::string_view::npos) {
                tokens.push_back(trim(raw.substr(start, i - start)));
                start = std::string_view::npos;
            }
        } else if (start == std::string_view::npos) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        tokens.push_back(trim(raw.substr(start)));
    }
    return tokens;
}

std::optional<BorderStyle> parseBorderStyleValue(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "solid") {
        return BorderStyle::Solid;
    }
    if (value == "none" || value == "hidden") {
        return BorderStyle::None;
    }
    return std::nullopt;
}

void applyBorderShorthand(Style& style, std::string_view rawValue) {
    const SkColor colorSentinel = SkColorSetARGB(1, 2, 3, 4);
    for (const std::string& token : splitCssTokens(rawValue)) {
        if (std::optional<float> width = parseLength(token)) {
            style.borderWidth = *width;
            style.flags.borderWidth = true;
            continue;
        }
        if (std::optional<BorderStyle> borderStyle = parseBorderStyleValue(token)) {
            style.borderStyle = *borderStyle;
            style.flags.borderStyle = true;
            continue;
        }

        const SkColor color = parseColor(token, colorSentinel);
        if (color != colorSentinel) {
            style.borderColor = color;
            style.flags.borderColor = true;
        }
    }
}

void mergeStyle(Style& target, const Style& source) {
    const Style::Flags& f = source.flags;
    if (f.display) {
        target.display = source.display;
        target.flags.display = true;
    }
    if (f.position) {
        target.position = source.position;
        target.flags.position = true;
    }
    if (f.flexDirection) {
        target.flexDirection = source.flexDirection;
        target.flags.flexDirection = true;
    }
    if (f.alignItems) {
        target.alignItems = source.alignItems;
        target.flags.alignItems = true;
    }
    if (f.justifyContent) {
        target.justifyContent = source.justifyContent;
        target.flags.justifyContent = true;
    }
    if (f.alignSelf) {
        target.alignSelf = source.alignSelf;
        target.flags.alignSelf = true;
    }
    if (f.flexGrow) {
        target.flexGrow = source.flexGrow;
        target.flags.flexGrow = true;
    }
    if (f.flexShrink) {
        target.flexShrink = source.flexShrink;
        target.flags.flexShrink = true;
    }
    if (f.width) {
        target.width = source.width;
        target.flags.width = true;
    }
    if (f.height) {
        target.height = source.height;
        target.flags.height = true;
    }
    if (f.minWidth) {
        target.minWidth = source.minWidth;
        target.flags.minWidth = true;
    }
    if (f.minHeight) {
        target.minHeight = source.minHeight;
        target.flags.minHeight = true;
    }
    if (f.maxWidth) {
        target.maxWidth = source.maxWidth;
        target.flags.maxWidth = true;
    }
    if (f.maxHeight) {
        target.maxHeight = source.maxHeight;
        target.flags.maxHeight = true;
    }
    if (f.marginLeft) {
        target.margin.left = source.margin.left;
        target.flags.marginLeft = true;
    }
    if (f.marginTop) {
        target.margin.top = source.margin.top;
        target.flags.marginTop = true;
    }
    if (f.marginRight) {
        target.margin.right = source.margin.right;
        target.flags.marginRight = true;
    }
    if (f.marginBottom) {
        target.margin.bottom = source.margin.bottom;
        target.flags.marginBottom = true;
    }
    if (f.paddingLeft) {
        target.padding.left = source.padding.left;
        target.flags.paddingLeft = true;
    }
    if (f.paddingTop) {
        target.padding.top = source.padding.top;
        target.flags.paddingTop = true;
    }
    if (f.paddingRight) {
        target.padding.right = source.padding.right;
        target.flags.paddingRight = true;
    }
    if (f.paddingBottom) {
        target.padding.bottom = source.padding.bottom;
        target.flags.paddingBottom = true;
    }
    if (f.insetLeft) {
        target.inset.left = source.inset.left;
        target.flags.insetLeft = true;
    }
    if (f.insetTop) {
        target.inset.top = source.inset.top;
        target.flags.insetTop = true;
    }
    if (f.insetRight) {
        target.inset.right = source.inset.right;
        target.flags.insetRight = true;
    }
    if (f.insetBottom) {
        target.inset.bottom = source.inset.bottom;
        target.flags.insetBottom = true;
    }
    if (f.color) {
        target.color = source.color;
        target.flags.color = true;
    }
    if (f.backgroundColor) {
        target.backgroundColor = source.backgroundColor;
        target.flags.backgroundColor = true;
    }
    if (f.borderColor) {
        target.borderColor = source.borderColor;
        target.flags.borderColor = true;
    }
    if (f.borderWidth) {
        target.borderWidth = source.borderWidth;
        target.flags.borderWidth = true;
    }
    if (f.borderStyle) {
        target.borderStyle = source.borderStyle;
        target.flags.borderStyle = true;
    }
    if (f.borderRadius) {
        target.borderRadius = source.borderRadius;
        target.flags.borderRadius = true;
    }
    if (f.fontSize) {
        target.fontSize = source.fontSize;
        target.flags.fontSize = true;
    }
    if (f.fontBold) {
        target.fontBold = source.fontBold;
        target.flags.fontBold = true;
    }
    if (f.backgroundGradient) {
        target.backgroundGradient = source.backgroundGradient;
        target.flags.backgroundGradient = true;
    }
}

bool matchesRule(const Node& node, const StyleRule& rule) {
    if (rule.kind == StyleRule::Kind::Tag) {
        return node.tag == rule.selector;
    }
    if (rule.kind == StyleRule::Kind::Id) {
        return node.id == rule.selector;
    }
    return std::find(node.classes.begin(), node.classes.end(), rule.selector) != node.classes.end();
}

void applyRules(Node& node, const std::vector<StyleRule>& rules) {
    for (const StyleRule& rule : rules) {
        if (matchesRule(node, rule)) {
            mergeStyle(node.style, rule.style);
        }
    }
    for (auto& child : node.children) {
        applyRules(*child, rules);
    }
}

void applyInlineStyles(Node& node) {
    mergeStyle(node.style, node.inlineStyle);
    for (auto& child : node.children) {
        applyInlineStyles(*child);
    }
}

void applyInheritedStyle(Node& node, const RuntimeOptions& options) {
    if (!node.parent) {
        if (!node.style.flags.color) {
            node.style.color = options.theme.text;
        }
    } else {
        const Style& parent = node.parent->style;
        if (!node.style.flags.color) {
            node.style.color = parent.color;
        }
        if (!node.style.flags.fontSize) {
            node.style.fontSize = parent.fontSize;
        }
        if (!node.style.flags.fontBold) {
            node.style.fontBold = parent.fontBold;
        }
    }

    if (node.tag == "text" || node.tag == "span" || node.tag == "label" || node.tag == "button" ||
        node.tag == "img" || node.tag == "svg") {
        node.style.flexShrink = 0.0f;
    }
    for (auto& child : node.children) {
        applyInheritedStyle(*child, options);
    }
}

void parseGradient(std::string_view raw, Style& style) {
    std::string value = trim(raw);
    const std::string valueLower = lower(value);
    Gradient gradient;
    if (valueLower.rfind("linear-gradient-x(", 0) == 0 && value.back() == ')') {
        gradient.kind = GradientKind::LinearX;
        value = value.substr(18, value.size() - 19);
    } else if (valueLower.rfind("linear-gradient-y(", 0) == 0 && value.back() == ')') {
        gradient.kind = GradientKind::LinearY;
        value = value.substr(18, value.size() - 19);
    } else if (valueLower.rfind("linear-gradient(", 0) == 0 && value.back() == ')') {
        gradient.kind = GradientKind::LinearY;
        value = value.substr(16, value.size() - 17);
        std::string argsLower = lower(trim(value));
        if (argsLower.rfind("to right,", 0) == 0) {
            gradient.kind = GradientKind::LinearX;
            value = trim(std::string_view(value).substr(9));
        } else if (argsLower.rfind("to bottom,", 0) == 0) {
            gradient.kind = GradientKind::LinearY;
            value = trim(std::string_view(value).substr(10));
        }
    } else if (valueLower.rfind("radial-gradient(", 0) == 0 && value.back() == ')') {
        gradient.kind = GradientKind::Radial;
        value = value.substr(16, value.size() - 17);
    } else {
        return;
    }

    std::vector<std::string> colorParts;
    size_t start = 0;
    int parenDepth = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '(') {
            ++parenDepth;
        } else if (value[i] == ')' && parenDepth > 0) {
            --parenDepth;
        } else if (value[i] == ',' && parenDepth == 0) {
            colorParts.push_back(trim(std::string_view(value).substr(start, i - start)));
            start = i + 1;
        }
    }
    colorParts.push_back(trim(std::string_view(value).substr(start)));

    for (const std::string& color : colorParts) {
        gradient.colors.push_back(parseColor(color, SK_ColorTRANSPARENT));
    }
    if (gradient.colors.empty()) {
        return;
    }
    style.backgroundGradient = std::move(gradient);
    style.flags.backgroundGradient = true;
}

void parseStyleSheet(std::string_view css, std::vector<StyleRule>& rules);

void collectStyleSheets(lxb_dom_node_t* root, std::vector<StyleRule>& rules) {
    if (!root) {
        return;
    }
    if (root->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t* element = lxb_dom_interface_element(root);
        if (nodeName(element) == "style") {
            std::string css;
            for (lxb_dom_node_t* child = root->first_child; child; child = child->next) {
                if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
                    auto* data = lxb_dom_interface_character_data(child);
                    if (data && data->data.data && data->data.length > 0) {
                        css.append(reinterpret_cast<const char*>(data->data.data), data->data.length);
                    }
                }
            }
            parseStyleSheet(css, rules);
        }
    }
    for (lxb_dom_node_t* child = root->first_child; child; child = child->next) {
        collectStyleSheets(child, rules);
    }
}

void applyDeclaration(Style& style, std::string_view rawName, std::string_view rawValue) {
    const std::string name = lower(trim(rawName));
    const std::string value = trim(rawValue);
    if (name.empty()) {
        return;
    }

    auto length = parseLength(value);
    if (name == "display") {
        style.display = lower(value) == "none" ? Display::None : Display::Flex;
        style.flags.display = true;
    } else if (name == "position") {
        style.position = lower(value) == "absolute" ? Position::Absolute : Position::Relative;
        style.flags.position = true;
    } else if (name == "flex-direction") {
        const std::string v = lower(value);
        style.flexDirection = v == "row" ? YGFlexDirectionRow : YGFlexDirectionColumn;
        style.flags.flexDirection = true;
    } else if (name == "align-items") {
        const std::string v = lower(value);
        if (v == "center") {
            style.alignItems = YGAlignCenter;
        } else if (v == "flex-start") {
            style.alignItems = YGAlignFlexStart;
        } else if (v == "flex-end") {
            style.alignItems = YGAlignFlexEnd;
        } else {
            style.alignItems = YGAlignStretch;
        }
        style.flags.alignItems = true;
    } else if (name == "align-self") {
        const std::string v = lower(value);
        if (v == "center") {
            style.alignSelf = YGAlignCenter;
        } else if (v == "flex-start") {
            style.alignSelf = YGAlignFlexStart;
        } else if (v == "flex-end") {
            style.alignSelf = YGAlignFlexEnd;
        } else if (v == "stretch") {
            style.alignSelf = YGAlignStretch;
        } else {
            style.alignSelf = YGAlignAuto;
        }
        style.flags.alignSelf = true;
    } else if (name == "justify-content") {
        const std::string v = lower(value);
        if (v == "center") {
            style.justifyContent = YGJustifyCenter;
        } else if (v == "flex-end") {
            style.justifyContent = YGJustifyFlexEnd;
        } else if (v == "space-between") {
            style.justifyContent = YGJustifySpaceBetween;
        } else {
            style.justifyContent = YGJustifyFlexStart;
        }
        style.flags.justifyContent = true;
    } else if (name == "flex-grow" && length) {
        style.flexGrow = *length;
        style.flags.flexGrow = true;
    } else if (name == "flex-shrink" && length) {
        style.flexShrink = *length;
        style.flags.flexShrink = true;
    } else if (name == "width" && length) {
        style.width = *length;
        style.flags.width = true;
    } else if (name == "height" && length) {
        style.height = *length;
        style.flags.height = true;
    } else if (name == "min-width" && length) {
        style.minWidth = *length;
        style.flags.minWidth = true;
    } else if (name == "min-height" && length) {
        style.minHeight = *length;
        style.flags.minHeight = true;
    } else if (name == "max-width" && length) {
        style.maxWidth = *length;
        style.flags.maxWidth = true;
    } else if (name == "max-height" && length) {
        style.maxHeight = *length;
        style.flags.maxHeight = true;
    } else if (name == "margin" && length) {
        setEdges(style.margin,
                 style.flags,
                 &Style::Flags::marginLeft,
                 &Style::Flags::marginTop,
                 &Style::Flags::marginRight,
                 &Style::Flags::marginBottom,
                 *length);
    } else if (name == "margin-left" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginLeft, &EdgeValues::left, *length);
    } else if (name == "margin-top" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginTop, &EdgeValues::top, *length);
    } else if (name == "margin-right" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginRight, &EdgeValues::right, *length);
    } else if (name == "margin-bottom" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginBottom, &EdgeValues::bottom, *length);
    } else if (name == "padding" && length) {
        setEdges(style.padding,
                 style.flags,
                 &Style::Flags::paddingLeft,
                 &Style::Flags::paddingTop,
                 &Style::Flags::paddingRight,
                 &Style::Flags::paddingBottom,
                 *length);
    } else if (name == "padding-left" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingLeft, &EdgeValues::left, *length);
    } else if (name == "padding-top" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingTop, &EdgeValues::top, *length);
    } else if (name == "padding-right" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingRight, &EdgeValues::right, *length);
    } else if (name == "padding-bottom" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingBottom, &EdgeValues::bottom, *length);
    } else if (name == "left" && length) {
        setEdgeByName(style.inset, style.flags, &Style::Flags::insetLeft, &EdgeValues::left, *length);
    } else if (name == "top" && length) {
        setEdgeByName(style.inset, style.flags, &Style::Flags::insetTop, &EdgeValues::top, *length);
    } else if (name == "right" && length) {
        setEdgeByName(style.inset, style.flags, &Style::Flags::insetRight, &EdgeValues::right, *length);
    } else if (name == "bottom" && length) {
        setEdgeByName(style.inset, style.flags, &Style::Flags::insetBottom, &EdgeValues::bottom, *length);
    } else if (name == "color") {
        style.color = parseColor(value, style.color);
        style.flags.color = true;
    } else if (name == "background-color") {
        style.backgroundColor = parseColor(value, style.backgroundColor);
        style.flags.backgroundColor = true;
    } else if (name == "background") {
        parseGradient(value, style);
        if (!style.flags.backgroundGradient) {
            style.backgroundColor = parseColor(value, style.backgroundColor);
            style.flags.backgroundColor = true;
        }
    } else if (name == "border") {
        applyBorderShorthand(style, value);
    } else if (name == "border-color") {
        style.borderColor = parseColor(value, style.borderColor);
        style.flags.borderColor = true;
    } else if (name == "border-width" && length) {
        style.borderWidth = *length;
        style.flags.borderWidth = true;
    } else if (name == "border-style") {
        const std::string v = lower(value);
        style.borderStyle = v == "solid" ? BorderStyle::Solid : BorderStyle::None;
        style.flags.borderStyle = true;
    } else if (name == "border-radius" && length) {
        style.borderRadius = *length;
        style.flags.borderRadius = true;
    } else if (name == "font-size" && length) {
        style.fontSize = *length;
        style.flags.fontSize = true;
    } else if (name == "font-weight") {
        style.fontBold = lower(value) == "bold" || value == "600" || value == "700";
        style.flags.fontBold = true;
    }
}

void parseDeclarations(std::string_view block, Style& style) {
    size_t start = 0;
    while (start < block.size()) {
        const size_t semi = block.find(';', start);
        const std::string_view declaration = block.substr(start, semi == std::string_view::npos ? block.size() - start : semi - start);
        const size_t colon = declaration.find(':');
        if (colon != std::string_view::npos) {
            applyDeclaration(style, declaration.substr(0, colon), declaration.substr(colon + 1));
        }
        if (semi == std::string_view::npos) {
            break;
        }
        start = semi + 1;
    }
}

void parseStyleSheet(std::string_view css, std::vector<StyleRule>& rules) {
    size_t start = 0;
    unsigned order = static_cast<unsigned>(rules.size());
    while (start < css.size()) {
        const size_t open = css.find('{', start);
        if (open == std::string_view::npos) {
            break;
        }
        const size_t close = css.find('}', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        const std::string selectorText = trim(css.substr(start, open - start));
        const std::string block = trim(css.substr(open + 1, close - open - 1));
        std::stringstream selectorStream(selectorText);
        std::string selector;
        while (std::getline(selectorStream, selector, ',')) {
            selector = trim(selector);
            if (selector.empty()) {
                continue;
            }
            StyleRule rule;
            rule.order = order++;
            if (selector[0] == '.') {
                rule.kind = StyleRule::Kind::Class;
                rule.selector = selector.substr(1);
            } else if (selector[0] == '#') {
                rule.kind = StyleRule::Kind::Id;
                rule.selector = selector.substr(1);
            } else {
                rule.kind = StyleRule::Kind::Tag;
                rule.selector = lower(selector);
            }
            parseDeclarations(block, rule.style);
            rules.push_back(std::move(rule));
        }
        start = close + 1;
    }
}

std::unique_ptr<Node> convertElement(lxb_dom_element_t* element, Node* parent, std::vector<StyleRule>& rules) {
    if (!element) {
        return nullptr;
    }
    const std::string tag = nodeName(element);
    if (tag == "style") {
        std::string css;
        for (lxb_dom_node_t* child = lxb_dom_interface_node(element)->first_child; child; child = child->next) {
            if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
                auto* data = lxb_dom_interface_character_data(child);
                if (data && data->data.data && data->data.length > 0) {
                    css.append(reinterpret_cast<const char*>(data->data.data), data->data.length);
                }
            }
        }
        parseStyleSheet(css, rules);
        return nullptr;
    }
    if (tag == "script" || tag == "head" || tag == "meta" || tag == "title") {
        return nullptr;
    }

    auto node = std::make_unique<Node>();
    node->tag = tag;
    node->parent = parent;
    node->id = attr(element, "id");
    node->classes = splitWhitespace(attr(element, "class"));
    node->value = attr(element, "value");
    node->src = attr(element, "src");

    const std::string inlineStyle = attr(element, "style");
    if (!inlineStyle.empty()) {
        parseDeclarations(inlineStyle, node->inlineStyle);
    }

    if (tag == "svg") {
        node->svgMarkup = serializeTree(lxb_dom_interface_node(element));
        return node;
    }

    for (lxb_dom_node_t* child = lxb_dom_interface_node(element)->first_child; child; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            auto* data = lxb_dom_interface_character_data(child);
            if (data && data->data.data && data->data.length > 0) {
                appendText(*node, std::string_view(reinterpret_cast<const char*>(data->data.data), data->data.length));
            }
        } else if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            std::unique_ptr<Node> childNode = convertElement(lxb_dom_interface_element(child), node.get(), rules);
            if (childNode) {
                node->children.push_back(std::move(childNode));
            }
        }
    }
    return node;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    out = stream.str();
    return true;
}

}  // namespace

DocumentParser::DocumentParser(RuntimeOptions options) : options_(std::move(options)) {}

bool DocumentParser::loadFile(const std::string& path, Document& outDocument, std::string& error) {
    std::string html;
    if (!readFile(path, html)) {
        error = "无法读取 HTML 文件: " + path;
        return false;
    }
    const std::filesystem::path base = std::filesystem::path(path).parent_path();
    return loadString(html, base.string(), outDocument, error);
}

bool DocumentParser::loadString(std::string_view html,
                                std::string_view basePath,
                                Document& outDocument,
                                std::string& error) {
    HtmlDocumentPtr htmlDocument(lxb_html_document_create());
    if (!htmlDocument) {
        error = "Lexbor 创建文档失败";
        return false;
    }
    if (lxb_html_document_parse(htmlDocument.get(),
                                reinterpret_cast<const lxb_char_t*>(html.data()),
                                html.size()) != LXB_STATUS_OK) {
        error = "Lexbor 解析 HTML 失败";
        return false;
    }

    lxb_html_body_element_t* body = lxb_html_document_body_element(htmlDocument.get());
    if (!body) {
        error = "HTML 没有 body";
        return false;
    }

    auto root = std::make_unique<Node>();
    root->tag = "root";
    root->style.flexDirection = YGFlexDirectionColumn;
    root->style.alignItems = YGAlignStretch;
    root->style.flexGrow = 1.0f;

    std::vector<StyleRule> rules;
    if (lxb_html_head_element_t* head = lxb_html_document_head_element(htmlDocument.get())) {
        collectStyleSheets(lxb_dom_interface_node(head), rules);
    }
    for (lxb_dom_node_t* child = lxb_dom_interface_node(body)->first_child; child; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            std::unique_ptr<Node> childNode = convertElement(lxb_dom_interface_element(child), root.get(), rules);
            if (childNode) {
                root->children.push_back(std::move(childNode));
            }
        }
    }

    outDocument.root = std::move(root);
    outDocument.rules = std::move(rules);
    outDocument.basePath = std::string(basePath);
    applyInheritedStyle(*outDocument.root, options_);
    applyRules(*outDocument.root, outDocument.rules);
    applyInheritedStyle(*outDocument.root, options_);
    applyInlineStyles(*outDocument.root);
    applyInheritedStyle(*outDocument.root, options_);
    return true;
}

}  // namespace skui
