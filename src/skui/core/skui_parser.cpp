#include "skui_internal.h"

#include <lexbor/dom/interface.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/parser.h>
#include <lexbor/html/serialize.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
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

void rebindParents(Node& node, Node* parent) {
    node.parent = parent;
    for (auto& child : node.children) {
        rebindParents(*child, &node);
    }
}

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

std::optional<float> parseNumberOrPx(std::string_view raw) {
    float value = 0.0f;
    if (!parseFloat(raw, value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<float> parseSeconds(std::string_view raw) {
    std::string value = lower(trim(raw));
    float multiplier = 1.0f;
    if (value.ends_with("ms")) {
        value.resize(value.size() - 2);
        multiplier = 0.001f;
    } else if (value.ends_with("s")) {
        value.resize(value.size() - 1);
    }

    float seconds = 0.0f;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, seconds);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return seconds * multiplier;
}

std::optional<Length> parseLength(std::string_view raw) {
    std::string value = lower(trim(raw));
    if (value == "auto") {
        return Length{0.0f, LengthUnit::Auto};
    }

    LengthUnit unit = LengthUnit::Px;
    if (value.ends_with("%")) {
        value.resize(value.size() - 1);
        unit = LengthUnit::Percent;
    } else if (value.ends_with("px")) {
        value.resize(value.size() - 2);
    }

    float number = 0.0f;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, number);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return Length{number, unit};
}

std::vector<std::string> splitCommaList(std::string_view raw) {
    std::vector<std::string> items;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (ch == '(') {
            ++depth;
        } else if (ch == ')' && depth > 0) {
            --depth;
        } else if (ch == ',' && depth == 0) {
            items.push_back(trim(raw.substr(start, i - start)));
            start = i + 1;
        }
    }
    items.push_back(trim(raw.substr(start)));
    items.erase(std::remove_if(items.begin(),
                               items.end(),
                               [](const std::string& item) {
                                   return item.empty();
                               }),
                items.end());
    return items;
}

std::optional<std::string> parseCssUrl(std::string_view raw) {
    const std::string value = trim(raw);
    const std::string valueLower = lower(value);
    if (valueLower == "none") {
        return std::string{};
    }
    if (!valueLower.starts_with("url(") || value.size() < 5 || value.back() != ')') {
        return std::nullopt;
    }

    std::string path = trim(std::string_view(value).substr(4, value.size() - 5));
    if (path.size() >= 2 &&
        ((path.front() == '"' && path.back() == '"') ||
         (path.front() == '\'' && path.back() == '\''))) {
        path = path.substr(1, path.size() - 2);
    }
    return path;
}

std::optional<Length> parseBackgroundPositionValue(std::string_view raw, bool horizontal) {
    const std::string value = lower(trim(raw));
    if (value == "center") {
        return Length{50.0f, LengthUnit::Percent};
    }
    if (horizontal) {
        if (value == "left") {
            return Length{0.0f, LengthUnit::Percent};
        }
        if (value == "right") {
            return Length{100.0f, LengthUnit::Percent};
        }
    } else {
        if (value == "top") {
            return Length{0.0f, LengthUnit::Percent};
        }
        if (value == "bottom") {
            return Length{100.0f, LengthUnit::Percent};
        }
    }
    std::optional<Length> length = parseLength(value);
    if (!length || length->unit == LengthUnit::Auto) {
        return std::nullopt;
    }
    return length;
}

std::optional<BackgroundPosition> parseBackgroundPosition(std::string_view raw) {
    const std::vector<std::string> tokens = splitWhitespace(raw);
    if (tokens.empty() || tokens.size() > 2) {
        return std::nullopt;
    }

    BackgroundPosition position;
    if (tokens.size() == 1) {
        const std::string value = lower(tokens.front());
        if (value == "top" || value == "bottom") {
            position.x = {50.0f, LengthUnit::Percent};
            std::optional<Length> y = parseBackgroundPositionValue(tokens.front(), false);
            if (!y) {
                return std::nullopt;
            }
            position.y = *y;
            return position;
        }

        std::optional<Length> x = parseBackgroundPositionValue(tokens.front(), true);
        if (!x) {
            return std::nullopt;
        }
        position.x = *x;
        position.y = {50.0f, LengthUnit::Percent};
        return position;
    }

    std::optional<Length> x = parseBackgroundPositionValue(tokens[0], true);
    std::optional<Length> y = parseBackgroundPositionValue(tokens[1], false);
    if (!x || !y) {
        const std::string first = lower(tokens[0]);
        const std::string second = lower(tokens[1]);
        const bool firstVertical = first == "top" || first == "bottom";
        const bool secondHorizontal = second == "left" || second == "right";
        if (!firstVertical || !secondHorizontal) {
            return std::nullopt;
        }
        x = parseBackgroundPositionValue(tokens[1], true);
        y = parseBackgroundPositionValue(tokens[0], false);
    }
    if (!x || !y) {
        return std::nullopt;
    }

    position.x = *x;
    position.y = *y;
    return position;
}

std::optional<BackgroundSize> parseBackgroundSize(std::string_view raw) {
    const std::vector<std::string> tokens = splitWhitespace(raw);
    if (tokens.empty() || tokens.size() > 2) {
        return std::nullopt;
    }

    std::optional<Length> width = parseLength(tokens[0]);
    std::optional<Length> height =
        tokens.size() > 1 ? parseLength(tokens[1])
                          : std::optional<Length>{Length{0.0f, LengthUnit::Auto}};
    if (!width || !height) {
        return std::nullopt;
    }
    if ((width->unit != LengthUnit::Auto && width->value < 0.0f) ||
        (height->unit != LengthUnit::Auto && height->value < 0.0f)) {
        return std::nullopt;
    }
    return BackgroundSize{*width, *height};
}

std::optional<BackgroundRepeat> parseBackgroundRepeat(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "repeat") {
        return BackgroundRepeat::Repeat;
    }
    if (value == "no-repeat") {
        return BackgroundRepeat::NoRepeat;
    }
    if (value == "repeat-x") {
        return BackgroundRepeat::RepeatX;
    }
    if (value == "repeat-y") {
        return BackgroundRepeat::RepeatY;
    }
    return std::nullopt;
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

std::unordered_map<std::string, std::string> elementAttributes(lxb_dom_element_t* element) {
    std::unordered_map<std::string, std::string> attributes;
    if (!element) {
        return attributes;
    }
    for (lxb_dom_attr_t* attribute = lxb_dom_element_first_attribute(element);
         attribute;
         attribute = lxb_dom_element_next_attribute(attribute)) {
        size_t nameLen = 0;
        const lxb_char_t* rawName = lxb_dom_attr_local_name(attribute, &nameLen);
        if (!rawName || nameLen == 0) {
            continue;
        }
        std::string name(reinterpret_cast<const char*>(rawName), nameLen);
        name = lower(std::move(name));

        size_t valueLen = 0;
        const lxb_char_t* rawValue = lxb_dom_attr_value(attribute, &valueLen);
        std::string value;
        if (rawValue && valueLen > 0) {
            value.assign(reinterpret_cast<const char*>(rawValue), valueLen);
        }
        attributes[std::move(name)] = std::move(value);
    }
    return attributes;
}

std::string attributeValue(const std::unordered_map<std::string, std::string>& attributes, std::string_view name) {
    auto it = attributes.find(std::string(name));
    return it == attributes.end() ? std::string{} : it->second;
}

std::vector<Node::TextLink> parseTextLinks(std::string_view raw, size_t textSize) {
    std::vector<Node::TextLink> links;
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t end = raw.find('\n', start);
        const std::string_view row(raw.data() + start,
                                   (end == std::string_view::npos ? raw.size() : end) - start);
        const size_t first = row.find(':');
        const size_t second = first == std::string_view::npos
            ? std::string_view::npos
            : row.find(':', first + 1);
        if (first != std::string_view::npos && second != std::string_view::npos) {
            size_t linkStart = 0;
            size_t linkEnd = 0;
            const std::string startText(row.substr(0, first));
            const std::string endText(row.substr(first + 1, second - first - 1));
            const auto startResult = std::from_chars(startText.data(),
                                                     startText.data() + startText.size(),
                                                     linkStart);
            const auto endResult = std::from_chars(endText.data(),
                                                   endText.data() + endText.size(),
                                                   linkEnd);
            if (startResult.ec == std::errc{} &&
                endResult.ec == std::errc{} &&
                linkStart < linkEnd &&
                linkEnd <= textSize) {
                links.push_back(Node::TextLink{
                    linkStart,
                    linkEnd,
                    std::string(row.substr(second + 1)),
                });
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return links;
}

float attributeFloat(const std::unordered_map<std::string, std::string>& attributes, std::string_view name, float fallback) {
    float out = fallback;
    const std::string value = attributeValue(attributes, name);
    return parseFloat(value, out) ? out : fallback;
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
        ++node.textRevision;
    }
}

void flushInlineSpace(std::string& text, bool& pendingSpace) {
    if (pendingSpace && !text.empty() && text.back() != '\n') {
        text.push_back(' ');
    }
    pendingSpace = false;
}

void appendInlineText(std::string& output,
                      std::string_view text,
                      bool& pendingSpace) {
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            pendingSpace = !output.empty() && output.back() != '\n';
            continue;
        }
        flushInlineSpace(output, pendingSpace);
        output.push_back(ch);
    }
}

void appendSelectableContent(lxb_dom_node_t* domNode,
                             Node& node,
                             bool& pendingSpace) {
    if (!domNode) {
        return;
    }
    if (domNode->type == LXB_DOM_NODE_TYPE_TEXT) {
        auto* data = lxb_dom_interface_character_data(domNode);
        if (data && data->data.data && data->data.length > 0) {
            appendInlineText(
                node.text,
                std::string_view(
                    reinterpret_cast<const char*>(data->data.data),
                    data->data.length),
                pendingSpace);
        }
        return;
    }
    if (domNode->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return;
    }

    auto* element = lxb_dom_interface_element(domNode);
    const std::string tag = nodeName(element);
    if (tag == "br") {
        pendingSpace = false;
        node.text.push_back('\n');
        return;
    }

    const bool isLink = tag == "a";
    if (isLink) {
        flushInlineSpace(node.text, pendingSpace);
    }
    const size_t linkStart = node.text.size();
    for (lxb_dom_node_t* child = domNode->first_child; child; child = child->next) {
        appendSelectableContent(child, node, pendingSpace);
    }
    if (!isLink) {
        return;
    }

    flushInlineSpace(node.text, pendingSpace);
    const size_t linkEnd = node.text.size();
    std::string action = attr(element, "data-action");
    if (action.empty()) {
        const std::string href = attr(element, "href");
        if (!href.empty()) {
            action = "open-url:" + href;
        }
    }
    if (linkStart < linkEnd && !action.empty()) {
        node.textLinks.push_back({linkStart, linkEnd, std::move(action)});
    }
}

void parseSelectableContent(lxb_dom_element_t* element, Node& node) {
    bool pendingSpace = false;
    for (lxb_dom_node_t* child = lxb_dom_interface_node(element)->first_child;
         child;
         child = child->next) {
        appendSelectableContent(child, node, pendingSpace);
    }
    if (!node.text.empty()) {
        ++node.textRevision;
    }
}

void setEdges(EdgeValues& edges,
              Style::Flags& flags,
              bool Style::Flags::*leftFlag,
              bool Style::Flags::*topFlag,
              bool Style::Flags::*rightFlag,
              bool Style::Flags::*bottomFlag,
              Length value) {
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
                   std::optional<Length> EdgeValues::*field,
                   Length value) {
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

bool applyEdgeShorthand(EdgeValues& edges,
                        Style::Flags& flags,
                        bool Style::Flags::*leftFlag,
                        bool Style::Flags::*topFlag,
                        bool Style::Flags::*rightFlag,
                        bool Style::Flags::*bottomFlag,
                        std::string_view raw) {
    std::vector<Length> values;
    for (const std::string& token : splitCssTokens(raw)) {
        std::optional<Length> length = parseLength(token);
        if (!length) {
            return false;
        }
        values.push_back(*length);
    }
    if (values.empty() || values.size() > 4) {
        return false;
    }

    const Length top = values[0];
    const Length right = values.size() >= 2 ? values[1] : values[0];
    const Length bottom = values.size() >= 3 ? values[2] : values[0];
    const Length left = values.size() >= 4 ? values[3] : right;
    setEdgeByName(edges, flags, leftFlag, &EdgeValues::left, left);
    setEdgeByName(edges, flags, topFlag, &EdgeValues::top, top);
    setEdgeByName(edges, flags, rightFlag, &EdgeValues::right, right);
    setEdgeByName(edges, flags, bottomFlag, &EdgeValues::bottom, bottom);
    return true;
}

bool applyBorderRadiusShorthand(Style& style, std::string_view raw) {
    std::vector<float> values;
    for (const std::string& token : splitCssTokens(raw)) {
        std::optional<float> radius = parseNumberOrPx(token);
        if (!radius) {
            return false;
        }
        values.push_back(std::max(0.0f, *radius));
    }
    if (values.empty() || values.size() > 4) {
        return false;
    }

    style.borderRadius.topLeft = values[0];
    style.borderRadius.topRight = values.size() >= 2 ? values[1] : values[0];
    style.borderRadius.bottomRight = values.size() >= 3 ? values[2] : values[0];
    style.borderRadius.bottomLeft = values.size() >= 4 ? values[3] : style.borderRadius.topRight;
    style.flags.borderTopLeftRadius = true;
    style.flags.borderTopRightRadius = true;
    style.flags.borderBottomRightRadius = true;
    style.flags.borderBottomLeftRadius = true;
    return true;
}

void setBorderCornerRadius(Style& style, bool Style::Flags::*flag, float CornerRadii::*field, float radius) {
    style.borderRadius.*field = std::max(0.0f, radius);
    style.flags.*flag = true;
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
        if (std::optional<float> width = parseNumberOrPx(token)) {
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

std::optional<Easing> parseEasing(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "linear") {
        return Easing::Linear;
    }
    if (value == "ease-in") {
        return Easing::EaseIn;
    }
    if (value == "ease-out") {
        return Easing::EaseOut;
    }
    if (value == "ease-in-out") {
        return Easing::EaseInOut;
    }
    if (value == "ease") {
        return Easing::Ease;
    }
    return std::nullopt;
}

std::optional<AnimationTimingFunction> parseAnimationTimingFunction(std::string_view raw) {
    if (std::optional<Easing> easing = parseEasing(raw)) {
        AnimationTimingFunction timing;
        timing.easing = *easing;
        return timing;
    }

    const std::string value = lower(trim(raw));
    if (value == "step-start") {
        AnimationTimingFunction timing;
        timing.kind = AnimationTimingKind::Steps;
        timing.stepPosition = AnimationStepPosition::Start;
        return timing;
    }
    if (value == "step-end") {
        AnimationTimingFunction timing;
        timing.kind = AnimationTimingKind::Steps;
        timing.stepPosition = AnimationStepPosition::End;
        return timing;
    }
    if (!value.starts_with("steps(") || value.back() != ')') {
        return std::nullopt;
    }

    const std::vector<std::string> args =
        splitCommaList(std::string_view(value).substr(6, value.size() - 7));
    if (args.empty() || args.size() > 2) {
        return std::nullopt;
    }

    int steps = 0;
    const auto result = std::from_chars(args[0].data(),
                                        args[0].data() + args[0].size(),
                                        steps);
    if (result.ec != std::errc{} || result.ptr != args[0].data() + args[0].size() ||
        steps <= 0) {
        return std::nullopt;
    }

    AnimationTimingFunction timing;
    timing.kind = AnimationTimingKind::Steps;
    timing.steps = steps;
    if (args.size() == 2) {
        const std::string position = lower(trim(args[1]));
        if (position == "start" || position == "jump-start") {
            timing.stepPosition = AnimationStepPosition::Start;
        } else if (position == "end" || position == "jump-end") {
            timing.stepPosition = AnimationStepPosition::End;
        } else {
            return std::nullopt;
        }
    }
    return timing;
}

bool isAnimationTimeToken(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "0") {
        return true;
    }
    if (!value.ends_with("ms") && !value.ends_with("s")) {
        return false;
    }
    return parseSeconds(value).has_value();
}

void parseAnimation(std::string_view raw, Style& style) {
    const std::string value = lower(trim(raw));
    style.animations.clear();
    style.flags.animation = true;
    if (value.empty() || value == "none") {
        return;
    }

    for (const std::string& item : splitCommaList(raw)) {
        AnimationDefinition animation;
        bool hasDuration = false;
        bool hasDelay = false;
        bool valid = true;
        for (const std::string& token : splitWhitespace(item)) {
            const std::string tokenLower = lower(token);
            if (std::optional<AnimationTimingFunction> timing =
                    parseAnimationTimingFunction(token)) {
                animation.timing = *timing;
                continue;
            }
            if (isAnimationTimeToken(token)) {
                std::optional<float> seconds = parseSeconds(token);
                if (!seconds) {
                    valid = false;
                    break;
                }
                if (!hasDuration) {
                    animation.durationSeconds = *seconds;
                    hasDuration = true;
                } else if (!hasDelay) {
                    animation.delaySeconds = *seconds;
                    hasDelay = true;
                } else {
                    valid = false;
                    break;
                }
                continue;
            }
            if (tokenLower == "infinite") {
                animation.iterationCount = -1.0f;
                continue;
            }
            if (tokenLower == "normal") {
                animation.direction = AnimationDirection::Normal;
                continue;
            }
            if (tokenLower == "reverse") {
                animation.direction = AnimationDirection::Reverse;
                continue;
            }
            if (tokenLower == "alternate") {
                animation.direction = AnimationDirection::Alternate;
                continue;
            }
            if (tokenLower == "alternate-reverse") {
                animation.direction = AnimationDirection::AlternateReverse;
                continue;
            }
            if (tokenLower == "forwards") {
                animation.fillMode = AnimationFillMode::Forwards;
                continue;
            }
            if (tokenLower == "backwards") {
                animation.fillMode = AnimationFillMode::Backwards;
                continue;
            }
            if (tokenLower == "both") {
                animation.fillMode = AnimationFillMode::Both;
                continue;
            }
            if (tokenLower == "running") {
                animation.playState = AnimationPlayState::Running;
                continue;
            }
            if (tokenLower == "paused") {
                animation.playState = AnimationPlayState::Paused;
                continue;
            }

            float iterations = 0.0f;
            if (parseFloat(token, iterations) && iterations >= 0.0f) {
                animation.iterationCount = iterations;
                continue;
            }
            if (animation.name.empty()) {
                animation.name = token;
                continue;
            }
            valid = false;
            break;
        }

        if (valid &&
            !animation.name.empty() &&
            hasDuration &&
            animation.durationSeconds > 0.0f &&
            animation.iterationCount != 0.0f) {
            style.animations.push_back(std::move(animation));
        }
    }
}

std::optional<TransitionProperty> parseTransitionProperty(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "all") {
        return TransitionProperty::All;
    }
    if (value == "opacity") {
        return TransitionProperty::Opacity;
    }
    if (value == "transform") {
        return TransitionProperty::Transform;
    }
    return std::nullopt;
}

void parseTransition(std::string_view raw, Style& style) {
    const std::string value = lower(trim(raw));
    style.transitions.clear();
    style.flags.transition = true;
    if (value.empty() || value == "none") {
        return;
    }

    for (const std::string& item : splitCommaList(raw)) {
        TransitionDefinition transition;
        bool hasDuration = false;
        bool valid = true;
        for (const std::string& token : splitWhitespace(item)) {
            if (std::optional<TransitionProperty> property = parseTransitionProperty(token)) {
                transition.property = *property;
                continue;
            }
            if (std::optional<Easing> easing = parseEasing(token)) {
                transition.easing = *easing;
                continue;
            }
            if (std::optional<float> seconds = parseSeconds(token)) {
                if (!hasDuration) {
                    transition.durationSeconds = *seconds;
                    hasDuration = true;
                } else {
                    transition.delaySeconds = *seconds;
                }
                continue;
            }
            valid = false;
        }
        if (valid && hasDuration && transition.durationSeconds > 0.0f) {
            style.transitions.push_back(transition);
        }
    }
}

std::vector<std::string> parseFunctionArguments(std::string_view raw) {
    std::string value(raw);
    std::replace(value.begin(), value.end(), ',', ' ');
    return splitWhitespace(value);
}

bool parseTransformFunction(std::string_view name, std::string_view rawArguments, Transform& transform) {
    std::vector<std::string> args = parseFunctionArguments(rawArguments);
    const std::string function = lower(trim(name));
    if (function == "translate" || function == "translate3d") {
        if (args.empty()) {
            return false;
        }
        std::optional<float> x = parseNumberOrPx(args[0]);
        std::optional<float> y = args.size() > 1 ? parseNumberOrPx(args[1])
                                                  : std::optional<float>{0.0f};
        if (!x || !y) {
            return false;
        }
        transform.translateX += *x;
        transform.translateY += *y;
        return true;
    }
    if (function == "translatex") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<float> x = parseNumberOrPx(args[0]);
        if (!x) {
            return false;
        }
        transform.translateX += *x;
        return true;
    }
    if (function == "translatey") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<float> y = parseNumberOrPx(args[0]);
        if (!y) {
            return false;
        }
        transform.translateY += *y;
        return true;
    }
    if (function == "scale" || function == "scale3d") {
        if (args.empty()) {
            return false;
        }
        std::optional<float> x = parseNumberOrPx(args[0]);
        std::optional<float> y = args.size() > 1 ? parseNumberOrPx(args[1]) : x;
        if (!x || !y) {
            return false;
        }
        transform.scaleX *= *x;
        transform.scaleY *= *y;
        return true;
    }
    if (function == "scalex") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<float> x = parseNumberOrPx(args[0]);
        if (!x) {
            return false;
        }
        transform.scaleX *= *x;
        return true;
    }
    if (function == "scaley") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<float> y = parseNumberOrPx(args[0]);
        if (!y) {
            return false;
        }
        transform.scaleY *= *y;
        return true;
    }
    if (function == "rotate" || function == "rotatez") {
        if (args.size() != 1) {
            return false;
        }
        std::string angle = lower(trim(args[0]));
        if (angle.ends_with("deg")) {
            angle.resize(angle.size() - 3);
        }
        std::optional<float> deg = parseNumberOrPx(angle);
        if (!deg) {
            return false;
        }
        transform.rotateDeg += *deg;
        return true;
    }
    return false;
}

std::optional<Transform> parseTransform(std::string_view raw) {
    std::string value = lower(trim(raw));
    if (value.empty()) {
        return std::nullopt;
    }
    if (value == "none") {
        return Transform{};
    }

    Transform transform;
    size_t pos = 0;
    while (pos < raw.size()) {
        while (pos < raw.size() && std::isspace(static_cast<unsigned char>(raw[pos]))) {
            ++pos;
        }
        if (pos >= raw.size()) {
            break;
        }
        const size_t nameStart = pos;
        while (pos < raw.size() && raw[pos] != '(') {
            ++pos;
        }
        if (pos >= raw.size()) {
            return std::nullopt;
        }
        const std::string_view name = raw.substr(nameStart, pos - nameStart);
        ++pos;
        const size_t argStart = pos;
        int depth = 1;
        while (pos < raw.size() && depth > 0) {
            if (raw[pos] == '(') {
                ++depth;
            } else if (raw[pos] == ')') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
            ++pos;
        }
        if (pos >= raw.size() || depth != 0) {
            return std::nullopt;
        }
        if (!parseTransformFunction(name, raw.substr(argStart, pos - argStart), transform)) {
            return std::nullopt;
        }
        ++pos;
    }
    return transform;
}

void mergeStyle(Style& target, const Style& source) {
    const Style::Flags& f = source.flags;
    if (f.display) {
        target.display = source.display;
        target.flags.display = true;
    }
    if (f.visibility) {
        target.visibility = source.visibility;
        target.flags.visibility = true;
    }
    if (f.position) {
        target.position = source.position;
        target.flags.position = true;
    }
    if (f.flexDirection) {
        target.flexDirection = source.flexDirection;
        target.flags.flexDirection = true;
    }
    if (f.flexWrap) {
        target.flexWrap = source.flexWrap;
        target.flags.flexWrap = true;
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
    if (f.boxSizing) {
        target.boxSizing = source.boxSizing;
        target.flags.boxSizing = true;
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
    if (f.backgroundImage) {
        target.backgroundImage = source.backgroundImage;
        target.flags.backgroundImage = true;
    }
    if (f.backgroundPosition) {
        target.backgroundPosition = source.backgroundPosition;
        target.flags.backgroundPosition = true;
    }
    if (f.backgroundSize) {
        target.backgroundSize = source.backgroundSize;
        target.flags.backgroundSize = true;
    }
    if (f.backgroundRepeat) {
        target.backgroundRepeat = source.backgroundRepeat;
        target.flags.backgroundRepeat = true;
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
    if (f.borderTopLeftRadius) {
        target.borderRadius.topLeft = source.borderRadius.topLeft;
        target.flags.borderTopLeftRadius = true;
    }
    if (f.borderTopRightRadius) {
        target.borderRadius.topRight = source.borderRadius.topRight;
        target.flags.borderTopRightRadius = true;
    }
    if (f.borderBottomRightRadius) {
        target.borderRadius.bottomRight = source.borderRadius.bottomRight;
        target.flags.borderBottomRightRadius = true;
    }
    if (f.borderBottomLeftRadius) {
        target.borderRadius.bottomLeft = source.borderRadius.bottomLeft;
        target.flags.borderBottomLeftRadius = true;
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
    if (f.opacity) {
        target.opacity = source.opacity;
        target.flags.opacity = true;
    }
    if (f.transform) {
        target.transform = source.transform;
        target.flags.transform = true;
    }
    if (f.transition) {
        target.transitions = source.transitions;
        target.flags.transition = true;
    }
    if (f.animation) {
        target.animations = source.animations;
        target.flags.animation = true;
    }
    if (f.overflowX) {
        target.overflowX = source.overflowX;
        target.flags.overflowX = true;
    }
    if (f.overflowY) {
        target.overflowY = source.overflowY;
        target.flags.overflowY = true;
    }
    if (f.scrollbarGutter) {
        target.scrollbarGutterStable = source.scrollbarGutterStable;
        target.flags.scrollbarGutter = true;
    }
    if (f.pointerEvents) {
        target.pointerEvents = source.pointerEvents;
        target.flags.pointerEvents = true;
    }
    if (f.cursor) {
        target.cursor = source.cursor;
        target.flags.cursor = true;
    }
}

Style defaultStyleForNode(const Node& node) {
    Style style;
    if (!node.parent) {
        style.flexDirection = YGFlexDirectionColumn;
        style.alignItems = YGAlignStretch;
        style.flexGrow = 1.0f;
    }
    return style;
}

bool nodeHasClass(const Node& node, const std::string& className) {
    return std::find(node.classes.begin(), node.classes.end(), className) != node.classes.end();
}

bool nodeHasAttribute(const Node& node, std::string_view name) {
    return node.attributes.find(std::string(name)) != node.attributes.end();
}

std::optional<size_t> childIndex(const Node& node) {
    if (!node.parent) {
        return std::nullopt;
    }
    size_t index = 1;
    for (const auto& child : node.parent->children) {
        if (child.get() == &node) {
            return index;
        }
        ++index;
    }
    return std::nullopt;
}

bool matchesNthChild(const Node& node, std::string_view expression) {
    std::optional<size_t> index = childIndex(node);
    if (!index) {
        return false;
    }
    std::string value = lower(trim(expression));
    if (value == "odd") {
        return (*index % 2) == 1;
    }
    if (value == "even") {
        return (*index % 2) == 0;
    }
    unsigned expected = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, expected);
    return result.ec == std::errc{} && result.ptr == end && *index == expected;
}

bool matchesPseudo(const Node& node, const std::string& pseudo) {
    if (pseudo == "hover") {
        return node.hovered;
    }
    if (pseudo == "active") {
        return node.active;
    }
    if (pseudo == "focus") {
        return node.focused;
    }
    if (pseudo == "disabled") {
        return nodeHasAttribute(node, "disabled");
    }
    if (pseudo == "checked") {
        return nodeHasAttribute(node, "checked");
    }
    if (pseudo == "selected") {
        return nodeHasAttribute(node, "selected");
    }
    if (pseudo == "first-child") {
        return childIndex(node).value_or(0) == 1;
    }
    if (pseudo == "last-child") {
        return node.parent && !node.parent->children.empty() && node.parent->children.back().get() == &node;
    }
    constexpr std::string_view nthPrefix = "nth-child(";
    if (pseudo.rfind(nthPrefix, 0) == 0 && pseudo.ends_with(')')) {
        return matchesNthChild(node, std::string_view(pseudo).substr(nthPrefix.size(), pseudo.size() - nthPrefix.size() - 1));
    }
    return false;
}

bool matchesCompound(const Node& node, const CompoundSelector& selector) {
    if (!selector.tag.empty() && selector.tag != "*" && node.tag != selector.tag) {
        return false;
    }
    if (!selector.id.empty() && node.id != selector.id) {
        return false;
    }
    for (const std::string& className : selector.classes) {
        if (!nodeHasClass(node, className)) {
            return false;
        }
    }
    for (const AttributeSelector& attribute : selector.attributes) {
        auto it = node.attributes.find(attribute.name);
        if (it == node.attributes.end()) {
            return false;
        }
        if (attribute.value && it->second != *attribute.value) {
            return false;
        }
    }
    for (const std::string& pseudo : selector.pseudos) {
        if (!matchesPseudo(node, pseudo)) {
            return false;
        }
    }
    return true;
}

bool matchesSelectorAt(const Node& node, const std::vector<SelectorPart>& selector, size_t index) {
    if (!matchesCompound(node, selector[index].selector)) {
        return false;
    }
    if (index == 0) {
        return true;
    }

    const SelectorCombinator combinator = selector[index].combinator;
    if (combinator == SelectorCombinator::Child) {
        return node.parent && matchesSelectorAt(*node.parent, selector, index - 1);
    }

    for (const Node* ancestor = node.parent; ancestor; ancestor = ancestor->parent) {
        if (matchesSelectorAt(*ancestor, selector, index - 1)) {
            return true;
        }
    }
    return false;
}

bool matchesRule(const Node& node, const StyleRule& rule) {
    return !rule.selector.empty() && matchesSelectorAt(node, rule.selector, rule.selector.size() - 1);
}

bool matchesMedia(const StyleRule& rule, float viewportWidth, float viewportHeight) {
    if ((rule.minViewportWidth || rule.maxViewportWidth) && viewportWidth <= 0.0f) {
        return false;
    }
    if ((rule.minViewportHeight || rule.maxViewportHeight) && viewportHeight <= 0.0f) {
        return false;
    }
    if (rule.minViewportWidth && viewportWidth < *rule.minViewportWidth) {
        return false;
    }
    if (rule.maxViewportWidth && viewportWidth > *rule.maxViewportWidth) {
        return false;
    }
    if (rule.minViewportHeight && viewportHeight < *rule.minViewportHeight) {
        return false;
    }
    if (rule.maxViewportHeight && viewportHeight > *rule.maxViewportHeight) {
        return false;
    }
    return true;
}

void applyRules(Node& node, const std::vector<StyleRule>& rules, float viewportWidth, float viewportHeight) {
    std::vector<const StyleRule*> matched;
    for (const StyleRule& rule : rules) {
        if (matchesMedia(rule, viewportWidth, viewportHeight) && matchesRule(node, rule)) {
            matched.push_back(&rule);
        }
    }
    std::sort(matched.begin(), matched.end(), [](const StyleRule* lhs, const StyleRule* rhs) {
        if (lhs->specificity != rhs->specificity) {
            return lhs->specificity < rhs->specificity;
        }
        return lhs->order < rhs->order;
    });
    for (const StyleRule* rule : matched) {
        mergeStyle(node.style, rule->style);
    }
    for (auto& child : node.children) {
        applyRules(*child, rules, viewportWidth, viewportHeight);
    }
}

void applyInlineStyles(Node& node) {
    mergeStyle(node.style, node.inlineStyle);
    for (auto& child : node.children) {
        applyInlineStyles(*child);
    }
}

void applyAnimatedStylesToSelf(Node& node) {
    if (!node.hasAnimatedStyle) {
        return;
    }
    Style animated;
    animated.flags = node.animatedStyleFlags;
    animated.opacity = node.animatedStyle.opacity;
    animated.transform = node.animatedStyle.transform;
    animated.backgroundPosition = node.animatedStyle.backgroundPosition;
    mergeStyle(node.style, animated);
}

void applyAnimatedStylesRecursive(Node& node) {
    applyAnimatedStylesToSelf(node);
    for (auto& child : node.children) {
        applyAnimatedStylesRecursive(*child);
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
        if (!node.style.flags.cursor) {
            node.style.cursor = parent.cursor;
        }
        if (!node.style.flags.visibility) {
            node.style.visibility = parent.visibility;
        }
    }

    if (node.tag == "text" || node.tag == "span" || node.tag == "label" || node.tag == "button" ||
        node.tag == "input" || node.tag == "textarea" || node.tag == "progress" ||
        node.tag == "img" || node.tag == "svg") {
        node.style.flexShrink = 0.0f;
    }
    for (auto& child : node.children) {
        applyInheritedStyle(*child, options);
    }
}

void resetStyles(Node& node) {
    node.style = defaultStyleForNode(node);
    for (auto& child : node.children) {
        resetStyles(*child);
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

std::optional<Overflow> parseOverflow(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "hidden") {
        return Overflow::Hidden;
    }
    if (value == "auto") {
        return Overflow::Auto;
    }
    if (value == "scroll") {
        return Overflow::Scroll;
    }
    if (value == "visible") {
        return Overflow::Visible;
    }
    return std::nullopt;
}

std::optional<Cursor> parseCursor(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "auto") {
        return Cursor::Auto;
    }
    if (value == "default") {
        return Cursor::Default;
    }
    if (value == "pointer") {
        return Cursor::Pointer;
    }
    if (value == "text") {
        return Cursor::Text;
    }
    if (value == "ew-resize" || value == "col-resize" || value == "e-resize" || value == "w-resize") {
        return Cursor::EWResize;
    }
    if (value == "ns-resize" || value == "row-resize" || value == "n-resize" || value == "s-resize") {
        return Cursor::NSResize;
    }
    if (value == "move") {
        return Cursor::Move;
    }
    if (value == "crosshair") {
        return Cursor::Crosshair;
    }
    if (value == "not-allowed") {
        return Cursor::NotAllowed;
    }
    return std::nullopt;
}

struct MediaContext {
    std::optional<float> minViewportWidth;
    std::optional<float> maxViewportWidth;
    std::optional<float> minViewportHeight;
    std::optional<float> maxViewportHeight;
};

void parseStyleSheet(std::string_view css,
                     std::vector<StyleRule>& rules,
                     std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                     MediaContext media = {});

void collectStyleSheets(lxb_dom_node_t* root,
                        std::vector<StyleRule>& rules,
                        std::unordered_map<std::string, KeyframesDefinition>& keyframes) {
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
            parseStyleSheet(css, rules, keyframes);
        }
    }
    for (lxb_dom_node_t* child = root->first_child; child; child = child->next) {
        collectStyleSheets(child, rules, keyframes);
    }
}

void applyDeclaration(Style& style, std::string_view rawName, std::string_view rawValue) {
    const std::string name = lower(trim(rawName));
    const std::string value = trim(rawValue);
    if (name.empty()) {
        return;
    }

    auto length = parseLength(value);
    auto number = parseNumberOrPx(value);
    if (name == "display") {
        style.display = lower(value) == "none" ? Display::None : Display::Flex;
        style.flags.display = true;
    } else if (name == "visibility") {
        style.visibility = lower(value) == "hidden" ? Visibility::Hidden : Visibility::Visible;
        style.flags.visibility = true;
    } else if (name == "position") {
        const std::string v = lower(value);
        if (v == "absolute") {
            style.position = Position::Absolute;
        } else if (v == "sticky") {
            style.position = Position::Sticky;
        } else {
            style.position = Position::Relative;
        }
        style.flags.position = true;
    } else if (name == "flex-direction") {
        const std::string v = lower(value);
        style.flexDirection = v == "row" ? YGFlexDirectionRow : YGFlexDirectionColumn;
        style.flags.flexDirection = true;
    } else if (name == "flex-wrap") {
        const std::string v = lower(value);
        style.flexWrap = v == "wrap" ? YGWrapWrap : YGWrapNoWrap;
        style.flags.flexWrap = true;
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
    } else if (name == "flex-grow" && number) {
        style.flexGrow = *number;
        style.flags.flexGrow = true;
    } else if (name == "flex-shrink" && number) {
        style.flexShrink = *number;
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
    } else if (name == "margin") {
        applyEdgeShorthand(style.margin,
                           style.flags,
                           &Style::Flags::marginLeft,
                           &Style::Flags::marginTop,
                           &Style::Flags::marginRight,
                           &Style::Flags::marginBottom,
                           value);
    } else if (name == "margin-left" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginLeft, &EdgeValues::left, *length);
    } else if (name == "margin-top" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginTop, &EdgeValues::top, *length);
    } else if (name == "margin-right" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginRight, &EdgeValues::right, *length);
    } else if (name == "margin-bottom" && length) {
        setEdgeByName(style.margin, style.flags, &Style::Flags::marginBottom, &EdgeValues::bottom, *length);
    } else if (name == "padding") {
        applyEdgeShorthand(style.padding,
                           style.flags,
                           &Style::Flags::paddingLeft,
                           &Style::Flags::paddingTop,
                           &Style::Flags::paddingRight,
                           &Style::Flags::paddingBottom,
                           value);
    } else if (name == "padding-left" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingLeft, &EdgeValues::left, *length);
    } else if (name == "padding-top" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingTop, &EdgeValues::top, *length);
    } else if (name == "padding-right" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingRight, &EdgeValues::right, *length);
    } else if (name == "padding-bottom" && length) {
        setEdgeByName(style.padding, style.flags, &Style::Flags::paddingBottom, &EdgeValues::bottom, *length);
    } else if (name == "box-sizing") {
        const std::string sizing = lower(value);
        if (sizing == "content-box" || sizing == "border-box") {
            style.boxSizing = sizing == "border-box"
                ? YGBoxSizingBorderBox
                : YGBoxSizingContentBox;
            style.flags.boxSizing = true;
        }
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
    } else if (name == "background-image") {
        if (std::optional<std::string> image = parseCssUrl(value)) {
            style.backgroundImage = std::move(*image);
            style.flags.backgroundImage = true;
        }
    } else if (name == "background-position") {
        if (std::optional<BackgroundPosition> position = parseBackgroundPosition(value)) {
            style.backgroundPosition = *position;
            style.flags.backgroundPosition = true;
        }
    } else if (name == "background-size") {
        if (std::optional<BackgroundSize> size = parseBackgroundSize(value)) {
            style.backgroundSize = *size;
            style.flags.backgroundSize = true;
        }
    } else if (name == "background-repeat") {
        if (std::optional<BackgroundRepeat> repeat = parseBackgroundRepeat(value)) {
            style.backgroundRepeat = *repeat;
            style.flags.backgroundRepeat = true;
        }
    } else if (name == "background") {
        if (std::optional<std::string> image = parseCssUrl(value)) {
            style.backgroundImage = std::move(*image);
            style.flags.backgroundImage = true;
        } else {
            parseGradient(value, style);
        }
        if (!style.flags.backgroundGradient && !style.flags.backgroundImage) {
            style.backgroundColor = parseColor(value, style.backgroundColor);
            style.flags.backgroundColor = true;
        }
    } else if (name == "border") {
        applyBorderShorthand(style, value);
    } else if (name == "border-color") {
        style.borderColor = parseColor(value, style.borderColor);
        style.flags.borderColor = true;
    } else if (name == "border-width" && number) {
        style.borderWidth = *number;
        style.flags.borderWidth = true;
    } else if (name == "border-style") {
        const std::string v = lower(value);
        style.borderStyle = v == "solid" ? BorderStyle::Solid : BorderStyle::None;
        style.flags.borderStyle = true;
    } else if (name == "border-radius") {
        applyBorderRadiusShorthand(style, value);
    } else if (name == "border-top-left-radius" && number) {
        setBorderCornerRadius(style, &Style::Flags::borderTopLeftRadius, &CornerRadii::topLeft, *number);
    } else if (name == "border-top-right-radius" && number) {
        setBorderCornerRadius(style, &Style::Flags::borderTopRightRadius, &CornerRadii::topRight, *number);
    } else if (name == "border-bottom-right-radius" && number) {
        setBorderCornerRadius(style, &Style::Flags::borderBottomRightRadius, &CornerRadii::bottomRight, *number);
    } else if (name == "border-bottom-left-radius" && number) {
        setBorderCornerRadius(style, &Style::Flags::borderBottomLeftRadius, &CornerRadii::bottomLeft, *number);
    } else if (name == "font-size" && number) {
        style.fontSize = *number;
        style.flags.fontSize = true;
    } else if (name == "font-weight") {
        style.fontBold = lower(value) == "bold" || value == "600" || value == "700";
        style.flags.fontBold = true;
    } else if (name == "opacity") {
        if (std::optional<float> opacity = parseNumberOrPx(value)) {
            style.opacity = clampf(*opacity, 0.0f, 1.0f);
            style.flags.opacity = true;
        }
    } else if (name == "transform") {
        if (std::optional<Transform> transform = parseTransform(value)) {
            style.transform = *transform;
            style.flags.transform = true;
        }
    } else if (name == "transition") {
        parseTransition(value, style);
    } else if (name == "animation") {
        parseAnimation(value, style);
    } else if (name == "overflow") {
        if (std::optional<Overflow> overflow = parseOverflow(value)) {
            style.overflowX = *overflow;
            style.overflowY = *overflow;
            style.flags.overflowX = true;
            style.flags.overflowY = true;
        }
    } else if (name == "overflow-x") {
        if (std::optional<Overflow> overflow = parseOverflow(value)) {
            style.overflowX = *overflow;
            style.flags.overflowX = true;
        }
    } else if (name == "overflow-y") {
        if (std::optional<Overflow> overflow = parseOverflow(value)) {
            style.overflowY = *overflow;
            style.flags.overflowY = true;
        }
    } else if (name == "scrollbar-gutter") {
        style.scrollbarGutterStable = lower(value).find("stable") != std::string::npos;
        style.flags.scrollbarGutter = true;
    } else if (name == "pointer-events") {
        style.pointerEvents = lower(value) == "none" ? PointerEvents::None : PointerEvents::Auto;
        style.flags.pointerEvents = true;
    } else if (name == "cursor") {
        if (std::optional<Cursor> cursor = parseCursor(value)) {
            style.cursor = *cursor;
            style.flags.cursor = true;
        }
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

std::string stripCssComments(std::string_view css) {
    std::string out;
    out.reserve(css.size());
    for (size_t i = 0; i < css.size();) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            const size_t end = css.find("*/", i + 2);
            if (end == std::string_view::npos) {
                break;
            }
            i = end + 2;
            continue;
        }
        out.push_back(css[i++]);
    }
    return out;
}

std::vector<std::string> splitSelectorList(std::string_view selectors) {
    std::vector<std::string> out;
    size_t start = 0;
    int bracketDepth = 0;
    int parenDepth = 0;
    for (size_t i = 0; i < selectors.size(); ++i) {
        const char ch = selectors[i];
        if (ch == '[') {
            ++bracketDepth;
        } else if (ch == ']' && bracketDepth > 0) {
            --bracketDepth;
        } else if (ch == '(') {
            ++parenDepth;
        } else if (ch == ')' && parenDepth > 0) {
            --parenDepth;
        } else if (ch == ',' && bracketDepth == 0 && parenDepth == 0) {
            out.push_back(trim(selectors.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim(selectors.substr(start)));
    return out;
}

bool isSelectorNameChar(char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return std::isalnum(c) != 0 || ch == '_' || ch == '-';
}

std::string parseSelectorName(std::string_view raw, size_t& pos) {
    const size_t start = pos;
    while (pos < raw.size() && isSelectorNameChar(raw[pos])) {
        ++pos;
    }
    return std::string(raw.substr(start, pos - start));
}

std::string unquoteCssValue(std::string value) {
    value = trim(value);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::optional<CompoundSelector> parseCompoundSelector(std::string_view raw) {
    CompoundSelector selector;
    size_t pos = 0;
    while (pos < raw.size()) {
        const char ch = raw[pos];
        if (ch == '*') {
            selector.tag = "*";
            ++pos;
        } else if (ch == '.') {
            ++pos;
            std::string className = parseSelectorName(raw, pos);
            if (className.empty()) {
                return std::nullopt;
            }
            selector.classes.push_back(std::move(className));
        } else if (ch == '#') {
            ++pos;
            selector.id = parseSelectorName(raw, pos);
            if (selector.id.empty()) {
                return std::nullopt;
            }
        } else if (ch == '[') {
            const size_t close = raw.find(']', pos + 1);
            if (close == std::string_view::npos) {
                return std::nullopt;
            }
            std::string body = trim(raw.substr(pos + 1, close - pos - 1));
            const size_t eq = body.find('=');
            AttributeSelector attribute;
            if (eq == std::string::npos) {
                attribute.name = lower(trim(body));
            } else {
                attribute.name = lower(trim(std::string_view(body).substr(0, eq)));
                attribute.value = unquoteCssValue(std::string(std::string_view(body).substr(eq + 1)));
            }
            if (attribute.name.empty()) {
                return std::nullopt;
            }
            selector.attributes.push_back(std::move(attribute));
            pos = close + 1;
        } else if (ch == ':') {
            ++pos;
            std::string pseudo = lower(parseSelectorName(raw, pos));
            if (pseudo.empty()) {
                return std::nullopt;
            }
            if (pos < raw.size() && raw[pos] == '(') {
                const size_t close = raw.find(')', pos + 1);
                if (close == std::string_view::npos) {
                    return std::nullopt;
                }
                pseudo += std::string(raw.substr(pos, close - pos + 1));
                pos = close + 1;
            }
            selector.pseudos.push_back(std::move(pseudo));
        } else if (std::isalpha(static_cast<unsigned char>(ch)) != 0) {
            selector.tag = lower(parseSelectorName(raw, pos));
        } else {
            return std::nullopt;
        }
    }
    if (selector.tag.empty() && selector.id.empty() && selector.classes.empty() &&
        selector.attributes.empty() && selector.pseudos.empty()) {
        return std::nullopt;
    }
    return selector;
}

unsigned selectorSpecificity(const CompoundSelector& selector) {
    unsigned value = 0;
    if (!selector.id.empty()) {
        value += 100;
    }
    value += static_cast<unsigned>(selector.classes.size() + selector.attributes.size() + selector.pseudos.size()) * 10;
    if (!selector.tag.empty() && selector.tag != "*") {
        value += 1;
    }
    return value;
}

std::optional<StyleRule> parseSelector(std::string_view selectorText) {
    StyleRule rule;
    SelectorCombinator nextCombinator = SelectorCombinator::None;
    size_t pos = 0;
    while (pos < selectorText.size()) {
        bool sawWhitespace = false;
        while (pos < selectorText.size() && std::isspace(static_cast<unsigned char>(selectorText[pos])) != 0) {
            sawWhitespace = true;
            ++pos;
        }
        if (pos >= selectorText.size()) {
            break;
        }
        if (selectorText[pos] == '>') {
            nextCombinator = SelectorCombinator::Child;
            ++pos;
            continue;
        }
        if (sawWhitespace && !rule.selector.empty() && nextCombinator == SelectorCombinator::None) {
            nextCombinator = SelectorCombinator::Descendant;
        }

        const size_t partStart = pos;
        int bracketDepth = 0;
        int parenDepth = 0;
        while (pos < selectorText.size()) {
            const char ch = selectorText[pos];
            if (ch == '[') {
                ++bracketDepth;
            } else if (ch == ']' && bracketDepth > 0) {
                --bracketDepth;
            } else if (ch == '(') {
                ++parenDepth;
            } else if (ch == ')' && parenDepth > 0) {
                --parenDepth;
            } else if (bracketDepth == 0 && parenDepth == 0 &&
                       (ch == '>' || std::isspace(static_cast<unsigned char>(ch)) != 0)) {
                break;
            }
            ++pos;
        }

        std::optional<CompoundSelector> compound = parseCompoundSelector(selectorText.substr(partStart, pos - partStart));
        if (!compound) {
            return std::nullopt;
        }
        SelectorPart part;
        part.combinator = rule.selector.empty() ? SelectorCombinator::None : nextCombinator;
        if (!rule.selector.empty() && part.combinator == SelectorCombinator::None) {
            part.combinator = SelectorCombinator::Descendant;
        }
        part.selector = std::move(*compound);
        rule.specificity += selectorSpecificity(part.selector);
        rule.selector.push_back(std::move(part));
        nextCombinator = SelectorCombinator::None;
    }
    if (rule.selector.empty()) {
        return std::nullopt;
    }
    return rule;
}

std::optional<size_t> findMatchingBrace(std::string_view css, size_t open) {
    int depth = 0;
    for (size_t i = open; i < css.size(); ++i) {
        if (css[i] == '{') {
            ++depth;
        } else if (css[i] == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::nullopt;
}

bool parseMediaFeature(std::string_view feature, MediaContext& context) {
    std::string body = lower(trim(feature));
    if (body.size() >= 2 && body.front() == '(' && body.back() == ')') {
        body = trim(std::string_view(body).substr(1, body.size() - 2));
    }
    const size_t colon = body.find(':');
    if (colon == std::string::npos) {
        return true;
    }
    const std::string name = trim(std::string_view(body).substr(0, colon));
    const std::string value = trim(std::string_view(body).substr(colon + 1));
    std::optional<float> number = parseNumberOrPx(value);
    if (!number) {
        return false;
    }
    if (name == "min-width") {
        context.minViewportWidth = std::max(context.minViewportWidth.value_or(*number), *number);
    } else if (name == "max-width") {
        context.maxViewportWidth = std::min(context.maxViewportWidth.value_or(*number), *number);
    } else if (name == "min-height") {
        context.minViewportHeight = std::max(context.minViewportHeight.value_or(*number), *number);
    } else if (name == "max-height") {
        context.maxViewportHeight = std::min(context.maxViewportHeight.value_or(*number), *number);
    }
    return true;
}

MediaContext parseMediaCondition(std::string_view header, MediaContext inherited) {
    std::string condition = trim(header);
    constexpr std::string_view mediaPrefix = "@media";
    if (lower(condition).rfind(mediaPrefix, 0) == 0) {
        condition = trim(std::string_view(condition).substr(mediaPrefix.size()));
    }

    size_t start = 0;
    while (start < condition.size()) {
        const std::string remainingLower = lower(std::string(std::string_view(condition).substr(start)));
        const size_t found = remainingLower.find(" and ");
        const size_t andPos = found == std::string::npos ? std::string::npos : start + found;
        const std::string_view part =
            std::string_view(condition).substr(
                start,
                andPos == std::string::npos
                    ? condition.size() - start
                    : andPos - start);
        parseMediaFeature(part, inherited);
        if (andPos == std::string::npos) {
            break;
        }
        start = andPos + 5;
    }
    return inherited;
}

std::optional<float> parseKeyframeOffset(std::string_view raw) {
    std::string value = lower(trim(raw));
    if (value == "from") {
        return 0.0f;
    }
    if (value == "to") {
        return 1.0f;
    }
    if (!value.ends_with("%")) {
        return std::nullopt;
    }
    value.pop_back();

    float percentage = 0.0f;
    if (!parseFloat(value, percentage) || percentage < 0.0f || percentage > 100.0f) {
        return std::nullopt;
    }
    return percentage / 100.0f;
}

void parseKeyframes(std::string_view name,
                    std::string_view css,
                    std::unordered_map<std::string, KeyframesDefinition>& keyframes) {
    const std::string animationName = trim(name);
    if (animationName.empty()) {
        return;
    }

    KeyframesDefinition definition;
    definition.name = animationName;
    size_t start = 0;
    while (start < css.size()) {
        const size_t open = css.find('{', start);
        if (open == std::string_view::npos) {
            break;
        }
        const std::optional<size_t> close = findMatchingBrace(css, open);
        if (!close) {
            break;
        }

        const std::string selectorText = trim(css.substr(start, open - start));
        const std::string block = trim(css.substr(open + 1, *close - open - 1));
        Style style;
        parseDeclarations(block, style);
        for (const std::string& selector : splitCommaList(selectorText)) {
            if (std::optional<float> offset = parseKeyframeOffset(selector)) {
                definition.frames.push_back(Keyframe{*offset, style});
            }
        }
        start = *close + 1;
    }

    if (definition.frames.empty()) {
        return;
    }
    std::stable_sort(definition.frames.begin(),
                     definition.frames.end(),
                     [](const Keyframe& lhs, const Keyframe& rhs) {
                         return lhs.offset < rhs.offset;
                     });
    std::vector<Keyframe> normalized;
    normalized.reserve(definition.frames.size());
    for (Keyframe& frame : definition.frames) {
        if (!normalized.empty() &&
            std::abs(normalized.back().offset - frame.offset) <= 0.000001f) {
            normalized.back() = std::move(frame);
        } else {
            normalized.push_back(std::move(frame));
        }
    }
    definition.frames = std::move(normalized);
    keyframes[animationName] = std::move(definition);
}

void parseStyleSheet(std::string_view css,
                     std::vector<StyleRule>& rules,
                     std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                     MediaContext media) {
    const std::string cleanedCss = stripCssComments(css);
    css = cleanedCss;
    size_t start = 0;
    while (start < css.size()) {
        const size_t open = css.find('{', start);
        if (open == std::string_view::npos) {
            break;
        }
        const std::optional<size_t> close = findMatchingBrace(css, open);
        if (!close) {
            break;
        }
        const std::string selectorText = trim(css.substr(start, open - start));
        const std::string block = trim(css.substr(open + 1, *close - open - 1));
        const std::string selectorLower = lower(selectorText);
        if (selectorLower.rfind("@media", 0) == 0) {
            const MediaContext nestedMedia =
                parseMediaCondition(selectorText, media);
            parseStyleSheet(block,
                            rules,
                            keyframes,
                            nestedMedia);
            start = *close + 1;
            continue;
        }
        constexpr std::string_view keyframesPrefix = "@keyframes";
        constexpr std::string_view webkitKeyframesPrefix = "@-webkit-keyframes";
        if (selectorLower.rfind(keyframesPrefix, 0) == 0) {
            parseKeyframes(std::string_view(selectorText).substr(keyframesPrefix.size()),
                           block,
                           keyframes);
            start = *close + 1;
            continue;
        }
        if (selectorLower.rfind(webkitKeyframesPrefix, 0) == 0) {
            parseKeyframes(std::string_view(selectorText).substr(webkitKeyframesPrefix.size()),
                           block,
                           keyframes);
            start = *close + 1;
            continue;
        }
        if (!selectorText.empty() && selectorText.front() == '@') {
            start = *close + 1;
            continue;
        }
        for (const std::string& selector : splitSelectorList(selectorText)) {
            if (selector.empty()) {
                continue;
            }
            std::optional<StyleRule> parsed = parseSelector(selector);
            if (!parsed) {
                continue;
            }
            StyleRule rule = std::move(*parsed);
            rule.minViewportWidth = media.minViewportWidth;
            rule.maxViewportWidth = media.maxViewportWidth;
            rule.minViewportHeight = media.minViewportHeight;
            rule.maxViewportHeight = media.maxViewportHeight;
            rule.order = static_cast<unsigned>(rules.size());
            parseDeclarations(block, rule.style);
            rules.push_back(std::move(rule));
        }
        start = *close + 1;
    }
}

std::unique_ptr<Node> convertElement(
    lxb_dom_element_t* element,
    Node* parent,
    std::vector<StyleRule>& rules,
    std::unordered_map<std::string, KeyframesDefinition>& keyframes) {
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
        parseStyleSheet(css, rules, keyframes);
        return nullptr;
    }
    if (tag == "script" || tag == "head" || tag == "meta" || tag == "title") {
        return nullptr;
    }

    auto node = std::make_unique<Node>();
    node->tag = tag;
    node->parent = parent;
    node->attributes = elementAttributes(element);
    node->id = attributeValue(node->attributes, "id");
    node->classes = splitWhitespace(attributeValue(node->attributes, "class"));
    node->value = attributeValue(node->attributes, "value");
    node->placeholder = attributeValue(node->attributes, "placeholder");
    node->src = attributeValue(node->attributes, "src");
    node->action = attributeValue(node->attributes, "data-action");
    node->numericValue = attributeFloat(node->attributes, "value", 0.0f);
    node->numericMax = std::max(0.0001f, attributeFloat(node->attributes, "max", 1.0f));
    node->virtualContentWidth = std::max(0.0f, attributeFloat(node->attributes, "data-virtual-width", 0.0f));
    node->virtualContentHeight = std::max(0.0f, attributeFloat(node->attributes, "data-virtual-height", 0.0f));
    const std::string inlineStyle = attributeValue(node->attributes, "style");
    if (!inlineStyle.empty()) {
        parseDeclarations(inlineStyle, node->inlineStyle);
    }

    if (tag == "svg") {
        node->svgMarkup = serializeTree(lxb_dom_interface_node(element));
        return node;
    }

    if (tag == "selectable") {
        if (node->value.empty()) {
            parseSelectableContent(element, *node);
        }
    } else {
        for (lxb_dom_node_t* child = lxb_dom_interface_node(element)->first_child;
             child;
             child = child->next) {
            if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
                auto* data = lxb_dom_interface_character_data(child);
                if (data && data->data.data && data->data.length > 0) {
                    std::string_view text(
                        reinterpret_cast<const char*>(data->data.data),
                        data->data.length);
                    if (tag == "textarea" && node->value.empty()) {
                        node->value += trim(text);
                        ++node->textRevision;
                    } else {
                        appendText(*node, text);
                    }
                }
            } else if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                std::unique_ptr<Node> childNode =
                    convertElement(lxb_dom_interface_element(child),
                                   node.get(),
                                   rules,
                                   keyframes);
                if (childNode) {
                    node->children.push_back(std::move(childNode));
                }
            }
        }
    }
    const std::string links = attributeValue(node->attributes, "data-links");
    if (!links.empty()) {
        const std::string& textValue = !node->value.empty() ? node->value : node->text;
        node->textLinks = parseTextLinks(links, textValue.size());
    }
    return node;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream file(pathFromUtf8(path), std::ios::binary);
    if (!file) {
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    out = stream.str();
    return true;
}

}  // namespace

void parseInlineStyle(std::string_view declarations, Style& style) {
    parseDeclarations(declarations, style);
}

DocumentParser::DocumentParser(RuntimeOptions options) : theme_(options.theme) {}

bool DocumentParser::loadFile(const std::string& path, Document& outDocument, std::string& error) {
    std::string html;
    if (!readFile(path, html)) {
        error = "无法读取 HTML 文件: " + path;
        return false;
    }
    const std::filesystem::path base = pathFromUtf8(path).parent_path();
    return loadString(html, pathToUtf8(base), outDocument, error);
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

    std::vector<StyleRule> rules;
    std::unordered_map<std::string, KeyframesDefinition> keyframes;
    if (lxb_html_head_element_t* head = lxb_html_document_head_element(htmlDocument.get())) {
        collectStyleSheets(lxb_dom_interface_node(head), rules, keyframes);
    }

    lxb_dom_element_t* documentElement =
        lxb_dom_document_element(lxb_dom_interface_document(htmlDocument.get()));
    std::unique_ptr<Node> root = convertElement(documentElement,
                                                nullptr,
                                                rules,
                                                keyframes);
    if (!root || root->tag != "html") {
        error = "HTML document element conversion failed";
        return false;
    }

    outDocument.root = std::move(root);
    outDocument.rules = std::move(rules);
    outDocument.keyframes = std::move(keyframes);
    outDocument.basePath = std::string(basePath);
    RuntimeOptions styleOptions;
    styleOptions.theme = theme_;
    recomputeStyles(outDocument, styleOptions);
    return true;
}

bool DocumentParser::loadFragment(std::string_view html,
                                  std::string_view basePath,
                                  std::vector<std::unique_ptr<Node>>& outNodes,
                                  std::vector<StyleRule>& outRules,
                                  std::string& error) {
    Document fragmentDocument;
    if (!loadString(html, basePath, fragmentDocument, error)) {
        return false;
    }
    outNodes.clear();
    outRules = std::move(fragmentDocument.rules);
    if (!fragmentDocument.root) {
        return true;
    }
    Node* body = nullptr;
    for (const auto& child : fragmentDocument.root->children) {
        if (child->tag == "body") {
            body = child.get();
            break;
        }
    }
    if (!body) {
        error = "HTML fragment did not contain body";
        return false;
    }
    outNodes = std::move(body->children);
    for (auto& node : outNodes) {
        rebindParents(*node, nullptr);
    }
    return true;
}

void recomputeStyles(Document& document, const RuntimeOptions& options, float viewportWidth, float viewportHeight) {
    if (!document.root) {
        return;
    }
    resetStyles(*document.root);
    applyInheritedStyle(*document.root, options);
    applyRules(*document.root, document.rules, viewportWidth, viewportHeight);
    applyInheritedStyle(*document.root, options);
    applyInlineStyles(*document.root);
    applyInheritedStyle(*document.root, options);
}

void applyAnimatedStyles(Node& node) {
    applyAnimatedStylesRecursive(node);
}

}  // namespace skui
