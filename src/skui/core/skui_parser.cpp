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
#include <unordered_set>

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

std::optional<float> parseUnitlessFloat(std::string_view raw) {
    const std::string value = trim(raw);
    float number = 0.0f;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, number);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return number;
}

std::optional<float> parseNumberOrPx(std::string_view raw) {
    float value = 0.0f;
    if (!parseFloat(raw, value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> parseZIndex(std::string_view raw) {
    std::string value = lower(trim(raw));
    if (value == "auto") {
        return 0;
    }
    if (value.starts_with('+')) {
        value.erase(0, 1);
    }
    if (value.empty()) {
        return std::nullopt;
    }

    int zIndex = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, zIndex);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return zIndex;
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

std::optional<float> parseAngleDegrees(std::string_view raw) {
    std::string value = lower(trim(raw));
    float multiplier = 1.0f;
    if (value.ends_with("deg")) {
        value.resize(value.size() - 3);
    } else if (value.ends_with("rad")) {
        value.resize(value.size() - 3);
        multiplier = 180.0f / 3.14159265358979323846f;
    } else if (value.ends_with("turn")) {
        value.resize(value.size() - 4);
        multiplier = 360.0f;
    } else if (value.ends_with("grad")) {
        value.resize(value.size() - 4);
        multiplier = 0.9f;
    }

    std::optional<float> number = parseUnitlessFloat(value);
    if (!number) {
        return std::nullopt;
    }
    return *number * multiplier;
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

std::optional<TransformOrigin> parseTransformOrigin(std::string_view raw) {
    const std::vector<std::string> tokens = splitWhitespace(raw);
    if (tokens.empty() || tokens.size() > 2) {
        return std::nullopt;
    }

    TransformOrigin origin;
    if (tokens.size() == 1) {
        const std::string value = lower(tokens.front());
        if (value == "top" || value == "bottom") {
            origin.x = {50.0f, LengthUnit::Percent};
            std::optional<Length> y = parseBackgroundPositionValue(value, false);
            if (!y) {
                return std::nullopt;
            }
            origin.y = *y;
            return origin;
        }

        std::optional<Length> x = parseBackgroundPositionValue(value, true);
        if (!x) {
            return std::nullopt;
        }
        origin.x = *x;
        origin.y = {50.0f, LengthUnit::Percent};
        return origin;
    }

    std::optional<Length> x = parseBackgroundPositionValue(tokens[0], true);
    std::optional<Length> y = parseBackgroundPositionValue(tokens[1], false);
    if (!x || !y) {
        return std::nullopt;
    }
    origin.x = *x;
    origin.y = *y;
    return origin;
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

bool isClipboardBlockTag(std::string_view tag) {
    return tag == "address" || tag == "article" || tag == "aside" ||
           tag == "blockquote" || tag == "div" || tag == "footer" ||
           tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" ||
           tag == "h5" || tag == "h6" || tag == "header" || tag == "li" ||
           tag == "main" || tag == "nav" || tag == "ol" || tag == "p" ||
           tag == "section" || tag == "table" || tag == "tr" || tag == "ul";
}

void appendClipboardText(std::vector<ClipboardItem>& items,
                         std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pendingSpace = false;
    for (const char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            pendingSpace = !normalized.empty() || !items.empty();
            continue;
        }
        if (pendingSpace) {
            normalized.push_back(' ');
            pendingSpace = false;
        }
        normalized.push_back(character);
    }
    if (pendingSpace) {
        normalized.push_back(' ');
    }
    if (normalized.empty()) {
        return;
    }
    if (!items.empty() && items.back().type == ClipboardItemType::Text) {
        items.back().text += normalized;
        return;
    }
    items.push_back({ClipboardItemType::Text, std::move(normalized), {}});
}

void appendClipboardLineBreak(std::vector<ClipboardItem>& items) {
    while (!items.empty() && items.back().type == ClipboardItemType::Text) {
        std::string& text = items.back().text;
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        if (!text.empty()) {
            break;
        }
        items.pop_back();
    }
    if (items.empty()) {
        return;
    }
    if (items.back().type != ClipboardItemType::Text) {
        items.push_back({ClipboardItemType::Text, "\n", {}});
        return;
    }
    if (!items.back().text.ends_with('\n')) {
        items.back().text.push_back('\n');
    }
}

void appendDomText(lxb_dom_node_t* node, std::string& output) {
    if (!node) {
        return;
    }
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        auto* data = lxb_dom_interface_character_data(node);
        if (data && data->data.data && data->data.length > 0) {
            output.append(
                reinterpret_cast<const char*>(data->data.data),
                data->data.length);
        }
        return;
    }
    for (lxb_dom_node_t* child = node->first_child;
         child;
         child = child->next) {
        appendDomText(child, output);
    }
}

void collectClipboardItems(lxb_dom_node_t* domNode,
                           std::vector<ClipboardItem>& items) {
    if (!domNode) {
        return;
    }
    if (domNode->type == LXB_DOM_NODE_TYPE_TEXT) {
        auto* data = lxb_dom_interface_character_data(domNode);
        if (data && data->data.data && data->data.length > 0) {
            appendClipboardText(
                items,
                std::string_view(
                    reinterpret_cast<const char*>(data->data.data),
                    data->data.length));
        }
        return;
    }
    if (domNode->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return;
    }

    auto* element = lxb_dom_interface_element(domNode);
    const std::string tag = nodeName(element);
    if (tag == "br") {
        appendClipboardLineBreak(items);
        return;
    }
    if (tag == "img") {
        const std::string source = attr(element, "src");
        if (!source.empty()) {
            items.push_back({
                ClipboardItemType::Image,
                attr(element, "alt"),
                source,
            });
        }
        return;
    }
    if (tag == "a") {
        const std::string source = attr(element, "href");
        const std::string downloadName = attr(element, "download");
        if (!source.empty() &&
            (!downloadName.empty() || source.starts_with("file:"))) {
            std::string label;
            appendDomText(domNode, label);
            label = trim(label);
            items.push_back({
                ClipboardItemType::File,
                downloadName.empty() ? std::move(label) : downloadName,
                source,
            });
            return;
        }
    }

    const bool block = isClipboardBlockTag(tag);
    if (block) {
        appendClipboardLineBreak(items);
    }
    for (lxb_dom_node_t* child = domNode->first_child;
         child;
         child = child->next) {
        collectClipboardItems(child, items);
    }
    if (block) {
        appendClipboardLineBreak(items);
    }
}

void trimClipboardBoundaryLineBreaks(std::vector<ClipboardItem>& items) {
    while (!items.empty() && items.front().type == ClipboardItemType::Text) {
        std::string& text = items.front().text;
        while (!text.empty() &&
               (text.front() == '\n' || text.front() == ' ')) {
            text.erase(text.begin());
        }
        if (!text.empty()) {
            break;
        }
        items.erase(items.begin());
    }
    while (!items.empty() && items.back().type == ClipboardItemType::Text) {
        std::string& text = items.back().text;
        while (!text.empty() &&
               (text.back() == '\n' || text.back() == ' ')) {
            text.pop_back();
        }
        if (!text.empty()) {
            break;
        }
        items.pop_back();
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

std::optional<GridTrack> parseGridTrack(std::string_view raw) {
    std::string value = lower(trim(raw));
    if (value == "auto") {
        return GridTrack{};
    }
    if (value.starts_with("minmax(") && value.ends_with(')')) {
        const std::vector<std::string> arguments = splitCommaList(
            std::string_view(value).substr(7, value.size() - 8));
        if (arguments.size() != 2) {
            return std::nullopt;
        }
        return parseGridTrack(arguments[1]);
    }
    if (value.ends_with("fr")) {
        value.resize(value.size() - 2);
        std::optional<float> fraction = parseUnitlessFloat(value);
        if (!fraction || *fraction <= 0.0f) {
            return std::nullopt;
        }
        GridTrack track;
        track.kind = GridTrackKind::Fraction;
        track.fraction = *fraction;
        return track;
    }
    if (std::optional<Length> length = parseLength(value)) {
        if (length->unit != LengthUnit::Auto) {
            GridTrack track;
            track.kind = GridTrackKind::Fixed;
            track.length = *length;
            return track;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<GridTrack>> parseGridTrackList(
    std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "none") {
        return std::vector<GridTrack>{};
    }

    std::vector<GridTrack> tracks;
    for (const std::string& token : splitCssTokens(value)) {
        if (token.starts_with("repeat(") && token.ends_with(')')) {
            const std::vector<std::string> arguments = splitCommaList(
                std::string_view(token).substr(7, token.size() - 8));
            if (arguments.size() != 2) {
                return std::nullopt;
            }

            unsigned count = 0;
            const char* begin = arguments[0].data();
            const char* end = arguments[0].data() + arguments[0].size();
            const auto result = std::from_chars(begin, end, count);
            std::optional<std::vector<GridTrack>> repeated =
                parseGridTrackList(arguments[1]);
            if (result.ec != std::errc{} || result.ptr != end || count == 0 ||
                count > 64 || !repeated || repeated->empty() ||
                repeated->size() > (64 - tracks.size()) / count) {
                return std::nullopt;
            }
            for (unsigned repetition = 0; repetition < count; ++repetition) {
                tracks.insert(tracks.end(), repeated->begin(), repeated->end());
            }
            continue;
        }

        std::optional<GridTrack> track = parseGridTrack(token);
        if (!track || tracks.size() >= 64) {
            return std::nullopt;
        }
        tracks.push_back(*track);
    }
    return tracks.empty()
        ? std::nullopt
        : std::optional<std::vector<GridTrack>>{std::move(tracks)};
}

std::optional<int> parseGridInteger(std::string_view raw) {
    const std::string value = trim(raw);
    int resultValue = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, resultValue);
    if (result.ec != std::errc{} || result.ptr != end || resultValue == 0) {
        return std::nullopt;
    }
    return resultValue;
}

std::optional<GridLine> parseGridLine(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value == "auto") {
        return GridLine{};
    }

    const std::vector<std::string> tokens = splitCssTokens(value);
    if (tokens.size() == 1) {
        if (std::optional<int> index = parseGridInteger(tokens.front())) {
            return GridLine{GridLineKind::Index, *index};
        }
        return std::nullopt;
    }
    if (tokens.size() == 2 && tokens.front() == "span") {
        if (std::optional<int> span = parseGridInteger(tokens.back());
            span && *span > 0) {
            return GridLine{GridLineKind::Span, *span};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<GridLine, GridLine>> parseGridColumn(
    std::string_view raw) {
    const size_t slash = raw.find('/');
    if (slash == std::string_view::npos) {
        if (std::optional<GridLine> start = parseGridLine(raw)) {
            return std::pair<GridLine, GridLine>{*start, GridLine{}};
        }
        return std::nullopt;
    }
    if (raw.find('/', slash + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    std::optional<GridLine> start = parseGridLine(raw.substr(0, slash));
    std::optional<GridLine> end = parseGridLine(raw.substr(slash + 1));
    if (!start || !end) {
        return std::nullopt;
    }
    return std::pair<GridLine, GridLine>{*start, *end};
}

std::optional<GridItemAlignment> parseGridItemAlignment(
    std::string_view raw) {
    std::vector<std::string> tokens = splitCssTokens(lower(trim(raw)));
    if (tokens.size() == 2 &&
        (tokens.front() == "safe" || tokens.front() == "unsafe")) {
        tokens.erase(tokens.begin());
    }
    if (tokens.size() != 1) {
        return std::nullopt;
    }

    const std::string& value = tokens.front();
    if (value == "auto") {
        return GridItemAlignment::Auto;
    }
    if (value == "normal" || value == "stretch") {
        return GridItemAlignment::Stretch;
    }
    if (value == "start" || value == "self-start" ||
        value == "flex-start" || value == "left") {
        return GridItemAlignment::Start;
    }
    if (value == "center") {
        return GridItemAlignment::Center;
    }
    if (value == "end" || value == "self-end" ||
        value == "flex-end" || value == "right") {
        return GridItemAlignment::End;
    }
    return std::nullopt;
}

bool parseFlexShorthand(std::string_view raw, Style& style) {
    const std::string value = lower(trim(raw));
    if (value == "none") {
        style.flexGrow = 0.0f;
        style.flexShrink = 0.0f;
        style.flexBasis = Length{0.0f, LengthUnit::Auto};
    } else if (value == "auto") {
        style.flexGrow = 1.0f;
        style.flexShrink = 1.0f;
        style.flexBasis = Length{0.0f, LengthUnit::Auto};
    } else {
        const std::vector<std::string> tokens = splitCssTokens(value);
        if (tokens.empty() || tokens.size() > 3) {
            return false;
        }
        std::optional<float> grow = parseUnitlessFloat(tokens[0]);
        if (!grow || *grow < 0.0f) {
            return false;
        }
        style.flexGrow = *grow;
        style.flexShrink = 1.0f;
        style.flexBasis = Length{0.0f, LengthUnit::Percent};
        if (tokens.size() >= 2) {
            if (std::optional<float> shrink = parseUnitlessFloat(tokens[1])) {
                if (*shrink < 0.0f) {
                    return false;
                }
                style.flexShrink = *shrink;
            } else if (tokens.size() == 2) {
                style.flexBasis = parseLength(tokens[1]);
                if (!style.flexBasis) {
                    return false;
                }
            } else {
                return false;
            }
        }
        if (tokens.size() == 3) {
            style.flexBasis = parseLength(tokens[2]);
            if (!style.flexBasis) {
                return false;
            }
        }
    }
    style.flags.flexGrow = true;
    style.flags.flexShrink = true;
    style.flags.flexBasis = true;
    return true;
}

std::optional<Shadow> parseShadow(std::string_view raw,
                                  bool allowInset,
                                  bool allowSpread) {
    Shadow shadow;
    std::vector<float> lengths;
    const SkColor colorSentinel = SkColorSetARGB(1, 2, 3, 4);
    for (const std::string& token : splitCssTokens(raw)) {
        const std::string tokenLower = lower(token);
        if (tokenLower == "inset") {
            if (!allowInset || shadow.inset) {
                return std::nullopt;
            }
            shadow.inset = true;
            continue;
        }
        const SkColor color = parseColor(token, colorSentinel);
        if (color != colorSentinel) {
            if (!shadow.usesCurrentColor) {
                return std::nullopt;
            }
            shadow.color = color;
            shadow.usesCurrentColor = false;
            continue;
        }
        std::optional<float> length = parseNumberOrPx(token);
        if (!length) {
            return std::nullopt;
        }
        lengths.push_back(*length);
    }
    const size_t maximumLengthCount = allowSpread ? 4 : 3;
    if (lengths.size() < 2 || lengths.size() > maximumLengthCount) {
        return std::nullopt;
    }
    shadow.offsetX = lengths[0];
    shadow.offsetY = lengths[1];
    shadow.blurRadius = lengths.size() >= 3
        ? std::max(0.0f, lengths[2])
        : 0.0f;
    shadow.spreadRadius = lengths.size() >= 4 ? lengths[3] : 0.0f;
    return shadow;
}

std::optional<std::vector<Shadow>> parseShadowList(
    std::string_view raw,
    bool allowInset,
    bool allowSpread) {
    const std::string value = lower(trim(raw));
    if (value == "none") {
        return std::vector<Shadow>{};
    }
    std::vector<Shadow> shadows;
    for (const std::string& item : splitCommaList(raw)) {
        std::optional<Shadow> shadow = parseShadow(
            item,
            allowInset,
            allowSpread);
        if (!shadow) {
            return std::nullopt;
        }
        shadows.push_back(*shadow);
    }
    return shadows.empty()
        ? std::nullopt
        : std::optional<std::vector<Shadow>>{std::move(shadows)};
}

std::vector<std::string> splitCssWhitespaceTokens(std::string_view raw) {
    return splitCssTokens(raw);
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
    std::vector<Length> values;
    for (const std::string& token : splitCssTokens(raw)) {
        std::optional<Length> radius = parseLength(token);
        if (!radius || radius->unit == LengthUnit::Auto) {
            return false;
        }
        radius->value = std::max(0.0f, radius->value);
        values.push_back(*radius);
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

void setBorderCornerRadius(Style& style, bool Style::Flags::* flag, Length CornerRadii::* field,
                           Length radius) {
    radius.value = std::max(0.0f, radius.value);
    style.borderRadius.*field = radius;
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

void applyBorderShorthand(BorderSide& side, std::string_view rawValue) {
    const SkColor colorSentinel = SkColorSetARGB(1, 2, 3, 4);
    for (const std::string& token : splitCssTokens(rawValue)) {
        if (std::optional<float> width = parseNumberOrPx(token)) {
            side.width = std::max(0.0f, *width);
            continue;
        }
        if (std::optional<BorderStyle> borderStyle = parseBorderStyleValue(token)) {
            side.style = *borderStyle;
            continue;
        }

        const SkColor color = parseColor(token, colorSentinel);
        if (color != colorSentinel) {
            side.color = color;
        }
    }
}

void applyBorderShorthand(Style& style, std::string_view rawValue) {
    BorderSide parsed;
    applyBorderShorthand(parsed, rawValue);
    for (BorderSide* side : {
             &style.borders.left,
             &style.borders.top,
             &style.borders.right,
             &style.borders.bottom}) {
        if (parsed.color) {
            side->color = parsed.color;
        }
        if (parsed.width) {
            side->width = parsed.width;
        }
        if (parsed.style) {
            side->style = parsed.style;
        }
    }
}

void mergeBorderSide(BorderSide& target, const BorderSide& source) {
    if (source.color) {
        target.color = source.color;
    }
    if (source.width) {
        target.width = source.width;
    }
    if (source.style) {
        target.style = source.style;
    }
}

std::optional<EasingFunction> parseEasing(std::string_view raw) {
    const std::string value = lower(trim(raw));
    EasingFunction easing;
    if (value == "linear") {
        easing.keyword = Easing::Linear;
        return easing;
    }
    if (value == "ease-in") {
        easing.keyword = Easing::EaseIn;
        return easing;
    }
    if (value == "ease-out") {
        easing.keyword = Easing::EaseOut;
        return easing;
    }
    if (value == "ease-in-out") {
        easing.keyword = Easing::EaseInOut;
        return easing;
    }
    if (value == "ease") {
        easing.keyword = Easing::Ease;
        return easing;
    }
    if (value.starts_with("cubic-bezier(") && value.back() == ')') {
        const std::vector<std::string> args =
            splitCommaList(std::string_view(value).substr(13, value.size() - 14));
        if (args.size() != 4) {
            return std::nullopt;
        }
        std::optional<float> x1 = parseUnitlessFloat(args[0]);
        std::optional<float> y1 = parseUnitlessFloat(args[1]);
        std::optional<float> x2 = parseUnitlessFloat(args[2]);
        std::optional<float> y2 = parseUnitlessFloat(args[3]);
        if (!x1 || !y1 || !x2 || !y2 ||
            *x1 < 0.0f || *x1 > 1.0f ||
            *x2 < 0.0f || *x2 > 1.0f) {
            return std::nullopt;
        }
        easing.cubicBezier = CubicBezier{*x1, *y1, *x2, *y2};
        return easing;
    }
    return std::nullopt;
}

std::optional<AnimationTimingFunction> parseAnimationTimingFunction(std::string_view raw) {
    if (std::optional<EasingFunction> easing = parseEasing(raw)) {
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
        for (const std::string& token : splitCssWhitespaceTokens(item)) {
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
    if (value == "height") {
        return TransitionProperty::Height;
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
        for (const std::string& token : splitCssWhitespaceTokens(item)) {
            if (std::optional<TransitionProperty> property = parseTransitionProperty(token)) {
                transition.property = *property;
                continue;
            }
            if (std::optional<EasingFunction> easing = parseEasing(token)) {
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
        std::optional<Length> x = parseLength(args[0]);
        std::optional<Length> y = args.size() > 1 ? parseLength(args[1])
                                                  : std::optional<Length>{Length{0.0f, LengthUnit::Px}};
        if (!x || !y) {
            return false;
        }
        transform.operations.push_back(TransformOperation{
            TransformOperationKind::Translate,
            *x,
            *y,
        });
        return true;
    }
    if (function == "translatex") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<Length> x = parseLength(args[0]);
        if (!x) {
            return false;
        }
        transform.operations.push_back(TransformOperation{
            TransformOperationKind::Translate,
            *x,
            Length{0.0f, LengthUnit::Px},
        });
        return true;
    }
    if (function == "translatey") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<Length> y = parseLength(args[0]);
        if (!y) {
            return false;
        }
        transform.operations.push_back(TransformOperation{
            TransformOperationKind::Translate,
            Length{0.0f, LengthUnit::Px},
            *y,
        });
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
        TransformOperation operation;
        operation.kind = TransformOperationKind::Scale;
        operation.scaleX = *x;
        operation.scaleY = *y;
        transform.operations.push_back(operation);
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
        TransformOperation operation;
        operation.kind = TransformOperationKind::Scale;
        operation.scaleX = *x;
        transform.operations.push_back(operation);
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
        TransformOperation operation;
        operation.kind = TransformOperationKind::Scale;
        operation.scaleY = *y;
        transform.operations.push_back(operation);
        return true;
    }
    if (function == "rotate" || function == "rotatez") {
        if (args.size() != 1) {
            return false;
        }
        std::optional<float> deg = parseAngleDegrees(args[0]);
        if (!deg) {
            return false;
        }
        TransformOperation operation;
        operation.kind = TransformOperationKind::Rotate;
        operation.rotateDeg = *deg;
        transform.operations.push_back(operation);
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

std::optional<float> parseFilterAmount(std::string_view raw, float defaultValue) {
    std::string value = trim(raw);
    if (value.empty()) {
        return defaultValue;
    }

    const bool percentage = value.ends_with('%');
    if (percentage) {
        value.pop_back();
    }
    std::optional<float> amount = parseUnitlessFloat(value);
    if (!amount || !std::isfinite(*amount) || *amount < 0.0f) {
        return std::nullopt;
    }
    return percentage ? *amount / 100.0f : *amount;
}

bool parseFilterFunction(std::string_view name,
                         std::string_view rawArguments,
                         Filter& filter) {
    const std::string function = lower(trim(name));
    if (function == "grayscale") {
        std::optional<float> amount = parseFilterAmount(rawArguments, 1.0f);
        if (!amount) {
            return false;
        }
        filter.operations.push_back({
            FilterOperationKind::Grayscale,
            clampf(*amount, 0.0f, 1.0f),
        });
        return true;
    }
    if (function == "brightness") {
        std::optional<float> amount = parseFilterAmount(rawArguments, 1.0f);
        if (!amount) {
            return false;
        }
        filter.operations.push_back({
            FilterOperationKind::Brightness,
            *amount,
        });
        return true;
    }
    if (function == "drop-shadow") {
        std::optional<Shadow> shadow = parseShadow(
            rawArguments,
            false,
            false);
        if (!shadow) {
            return false;
        }
        FilterOperation operation;
        operation.kind = FilterOperationKind::DropShadow;
        operation.shadow = *shadow;
        filter.operations.push_back(std::move(operation));
        return true;
    }
    return false;
}

std::optional<Filter> parseFilter(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (value.empty()) {
        return std::nullopt;
    }
    if (value == "none") {
        return Filter{};
    }

    Filter filter;
    size_t pos = 0;
    while (pos < raw.size()) {
        while (pos < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[pos])) != 0) {
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
        const size_t argumentStart = pos;
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
        if (!parseFilterFunction(
                name,
                raw.substr(argumentStart, pos - argumentStart),
                filter)) {
            return std::nullopt;
        }
        ++pos;
    }
    return filter.operations.empty() ? std::nullopt
                                     : std::optional<Filter>{std::move(filter)};
}

void mergeStyle(Style& target, const Style& source) {
    const Style::Flags& f = source.flags;
    if (f.display) {
        target.display = source.display;
        target.displayFlex = source.displayFlex;
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
    if (f.zIndex) {
        target.zIndex = source.zIndex;
        target.flags.zIndex = true;
    }
    if (f.flexDirection) {
        target.flexDirection = source.flexDirection;
        target.flags.flexDirection = true;
    }
    if (f.flexWrap) {
        target.flexWrap = source.flexWrap;
        target.flags.flexWrap = true;
    }
    if (f.rowGap) {
        target.rowGap = source.rowGap;
        target.flags.rowGap = true;
    }
    if (f.columnGap) {
        target.columnGap = source.columnGap;
        target.flags.columnGap = true;
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
    if (f.flexBasis) {
        target.flexBasis = source.flexBasis;
        target.flags.flexBasis = true;
    }
    if (f.gridTemplateColumns) {
        target.gridTemplateColumns = source.gridTemplateColumns;
        target.flags.gridTemplateColumns = true;
    }
    if (f.gridTemplateRows) {
        target.gridTemplateRows = source.gridTemplateRows;
        target.flags.gridTemplateRows = true;
    }
    if (f.gridAutoRows) {
        target.gridAutoRows = source.gridAutoRows;
        target.flags.gridAutoRows = true;
    }
    if (f.gridColumnStart) {
        target.gridColumnStart = source.gridColumnStart;
        target.flags.gridColumnStart = true;
    }
    if (f.gridColumnEnd) {
        target.gridColumnEnd = source.gridColumnEnd;
        target.flags.gridColumnEnd = true;
    }
    if (f.gridRowStart) {
        target.gridRowStart = source.gridRowStart;
        target.flags.gridRowStart = true;
    }
    if (f.gridRowEnd) {
        target.gridRowEnd = source.gridRowEnd;
        target.flags.gridRowEnd = true;
    }
    if (f.justifyItems) {
        target.justifyItems = source.justifyItems;
        target.flags.justifyItems = true;
    }
    if (f.justifySelf) {
        target.justifySelf = source.justifySelf;
        target.flags.justifySelf = true;
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
    mergeBorderSide(target.borders.left, source.borders.left);
    mergeBorderSide(target.borders.top, source.borders.top);
    mergeBorderSide(target.borders.right, source.borders.right);
    mergeBorderSide(target.borders.bottom, source.borders.bottom);
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
    if (f.lineHeight) {
        target.lineHeight = source.lineHeight;
        target.flags.lineHeight = true;
    }
    if (f.textAlign) {
        target.textAlign = source.textAlign;
        target.flags.textAlign = true;
    }
    if (f.textOverflow) {
        target.textOverflowEllipsis = source.textOverflowEllipsis;
        target.flags.textOverflow = true;
    }
    if (f.whiteSpace) {
        target.whiteSpaceNoWrap = source.whiteSpaceNoWrap;
        target.flags.whiteSpace = true;
    }
    if (f.backgroundGradient) {
        target.backgroundGradients = source.backgroundGradients;
        target.flags.backgroundGradient = true;
    }
    if (f.maskImage) {
        target.maskGradient = source.maskGradient;
        target.flags.maskImage = true;
    }
    if (f.boxShadow) {
        target.boxShadows = source.boxShadows;
        target.flags.boxShadow = true;
    }
    if (f.textShadow) {
        target.textShadows = source.textShadows;
        target.flags.textShadow = true;
    }
    if (f.content) {
        target.content = source.content;
        target.flags.content = true;
    }
    if (f.opacity) {
        target.opacity = source.opacity;
        target.flags.opacity = true;
    }
    if (f.transform) {
        target.transform = source.transform;
        target.flags.transform = true;
    }
    if (f.transformOrigin) {
        target.transformOrigin = source.transformOrigin;
        target.flags.transformOrigin = true;
    }
    if (f.filter) {
        target.filter = source.filter;
        target.flags.filter = true;
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
    if (node.attributes.contains("disabled")) {
        style.cursor = Cursor::Default;
    }
    return style;
}

bool nodeHasClass(const Node& node, const std::string& className) {
    return std::find(node.classes.begin(), node.classes.end(), className) != node.classes.end();
}

bool nodeHasAttribute(const Node& node, std::string_view name) {
    return node.attributes.find(std::string(name)) != node.attributes.end();
}

std::optional<CompoundSelector> parseCompoundSelector(std::string_view raw);
bool matchesCompound(const Node& node, const CompoundSelector& selector);

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
    value.erase(std::remove_if(value.begin(), value.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    }), value.end());
    if (value == "odd") {
        return (*index % 2) == 1;
    }
    if (value == "even") {
        return (*index % 2) == 0;
    }
    const auto parseInteger = [](std::string_view raw) -> std::optional<int> {
        if (raw.starts_with('+')) {
            raw.remove_prefix(1);
        }
        if (raw.empty()) {
            return std::nullopt;
        }
        int parsed = 0;
        const char* begin = raw.data();
        const char* end = raw.data() + raw.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (result.ec != std::errc{} || result.ptr != end) {
            return std::nullopt;
        }
        return parsed;
    };

    const size_t n = value.find('n');
    if (n == std::string::npos) {
        std::optional<int> expected = parseInteger(value);
        return expected && *expected > 0 &&
               *index == static_cast<size_t>(*expected);
    }
    if (value.find('n', n + 1) != std::string::npos) {
        return false;
    }

    const std::string_view coefficientText(value.data(), n);
    int coefficient = 0;
    if (coefficientText.empty() || coefficientText == "+") {
        coefficient = 1;
    } else if (coefficientText == "-") {
        coefficient = -1;
    } else if (std::optional<int> parsed = parseInteger(coefficientText)) {
        coefficient = *parsed;
    } else {
        return false;
    }

    int offset = 0;
    const std::string_view offsetText(value.data() + n + 1,
                                      value.size() - n - 1);
    if (!offsetText.empty()) {
        std::optional<int> parsed = parseInteger(offsetText);
        if (!parsed) {
            return false;
        }
        offset = *parsed;
    }

    const int delta = static_cast<int>(*index) - offset;
    if (coefficient == 0) {
        return delta == 0;
    }
    return delta % coefficient == 0 && delta / coefficient >= 0;
}

bool matchesPseudo(const Node& node, const std::string& pseudo) {
    if (pseudo == "root") {
        return node.parent == nullptr;
    }
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
    constexpr std::string_view notPrefix = "not(";
    if (pseudo.rfind(notPrefix, 0) == 0 && pseudo.ends_with(')')) {
        const std::string_view body(pseudo.data() + notPrefix.size(),
                                    pseudo.size() - notPrefix.size() - 1);
        std::optional<CompoundSelector> selector = parseCompoundSelector(body);
        return selector && !matchesCompound(node, *selector);
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

void applyRules(Node& node,
                const std::vector<StyleRule>& rules,
                float viewportWidth,
                float viewportHeight,
                bool important) {
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
        const Style& ruleStyle = important ? rule->importantStyle : rule->style;
        if (rule->pseudoElement == PseudoElement::Before) {
            mergeStyle(node.beforeStyle, ruleStyle);
        } else if (rule->pseudoElement == PseudoElement::After) {
            mergeStyle(node.afterStyle, ruleStyle);
        } else if (important) {
            mergeStyle(node.importantStyle, ruleStyle);
        } else {
            mergeStyle(node.style, ruleStyle);
        }
    }
    node.hasBeforeStyle = node.beforeStyle.flags.content &&
                          node.beforeStyle.content != "none" &&
                          node.beforeStyle.content != "normal";
    node.hasAfterStyle = node.afterStyle.flags.content &&
                         node.afterStyle.content != "none" &&
                         node.afterStyle.content != "normal";
    for (auto& child : node.children) {
        applyRules(*child, rules, viewportWidth, viewportHeight, important);
    }
}

void applyInlineStyles(Node& node, bool important) {
    if (important) {
        mergeStyle(node.importantStyle, node.inlineImportantStyle);
    } else {
        mergeStyle(node.style, node.inlineStyle);
    }
    for (auto& child : node.children) {
        applyInlineStyles(*child, important);
    }
}

void applyImportantStyles(Node& node) {
    mergeStyle(node.style, node.importantStyle);
    for (auto& child : node.children) {
        applyImportantStyles(*child);
    }
}

void applyPresentationStyles(Node& node) {
    mergeStyle(node.style, node.presentationStyle);
    for (auto& child : node.children) {
        applyPresentationStyles(*child);
    }
}

void applyAnimatedStylesToSelf(Node& node) {
    if (!node.hasAnimatedStyle) {
        return;
    }
    Style animated;
    animated.flags = node.animatedStyleFlags;
    animated.height = node.animatedStyle.height;
    animated.opacity = node.animatedStyle.opacity;
    animated.transform = node.animatedStyle.transform;
    animated.backgroundPosition = node.animatedStyle.backgroundPosition;
    mergeStyle(node.style, animated);
    mergeStyle(node.style, node.importantStyle);
}

void applyAnimatedStylesRecursive(Node& node) {
    applyAnimatedStylesToSelf(node);
    for (auto& child : node.children) {
        applyAnimatedStylesRecursive(*child);
    }
}

void applyInheritedStyle(Node& node, const RuntimeOptions& options) {
    if (node.style.display == Display::Flex &&
        node.style.displayFlex &&
        !node.style.flags.flexDirection) {
        node.style.flexDirection = YGFlexDirectionRow;
    }

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
        if (!node.style.flags.lineHeight) {
            node.style.lineHeight = parent.lineHeight;
        }
        if (!node.style.flags.textAlign) {
            node.style.textAlign = parent.textAlign;
        }
        if (!node.style.flags.whiteSpace) {
            node.style.whiteSpaceNoWrap = parent.whiteSpaceNoWrap;
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
        node.tag == "img" || node.tag == "video" || node.tag == "svg") {
        node.style.flexShrink = 0.0f;
    }

    const auto applyPseudoInheritance = [&](Style& pseudoStyle) {
        if (!pseudoStyle.flags.color) {
            pseudoStyle.color = node.style.color;
        }
        if (!pseudoStyle.flags.fontSize) {
            pseudoStyle.fontSize = node.style.fontSize;
        }
        if (!pseudoStyle.flags.fontBold) {
            pseudoStyle.fontBold = node.style.fontBold;
        }
        if (!pseudoStyle.flags.lineHeight) {
            pseudoStyle.lineHeight = node.style.lineHeight;
        }
        if (!pseudoStyle.flags.textAlign) {
            pseudoStyle.textAlign = node.style.textAlign;
        }
        if (!pseudoStyle.flags.whiteSpace) {
            pseudoStyle.whiteSpaceNoWrap = node.style.whiteSpaceNoWrap;
        }
        if (!pseudoStyle.flags.visibility) {
            pseudoStyle.visibility = node.style.visibility;
        }
    };
    if (node.hasBeforeStyle) {
        applyPseudoInheritance(node.beforeStyle);
    }
    if (node.hasAfterStyle) {
        applyPseudoInheritance(node.afterStyle);
    }
    for (auto& child : node.children) {
        applyInheritedStyle(*child, options);
    }
}

void resetStyles(Node& node) {
    node.style = defaultStyleForNode(node);
    node.importantStyle = Style{};
    node.beforeStyle = Style{};
    node.afterStyle = Style{};
    node.hasBeforeStyle = false;
    node.hasAfterStyle = false;
    for (auto& child : node.children) {
        resetStyles(*child);
    }
}

std::optional<Length> parseGradientStopPosition(std::string_view raw) {
    std::optional<Length> position = parseLength(trim(raw));
    if (!position ||
        (position->unit != LengthUnit::Percent &&
         position->unit != LengthUnit::Px)) {
        return std::nullopt;
    }
    return position;
}

bool parseGradientColorStop(std::string_view raw,
                            SkColor& color,
                            std::optional<Length>& position) {
    const std::vector<std::string> tokens = splitCssTokens(raw);
    if (tokens.empty() || tokens.size() > 2) {
        return false;
    }
    const SkColor sentinel = SkColorSetARGB(1, 2, 3, 4);
    color = parseColor(tokens[0], sentinel);
    if (color == sentinel) {
        return false;
    }
    if (tokens.size() == 2) {
        position = parseGradientStopPosition(tokens[1]);
        if (!position) {
            return false;
        }
    }
    return true;
}

std::optional<float> parseLinearGradientDirection(std::string_view raw) {
    const std::string value = lower(trim(raw));
    if (std::optional<float> angle = parseAngleDegrees(value)) {
        return *angle;
    }
    if (!value.starts_with("to ")) {
        return std::nullopt;
    }
    const bool top = value.find("top") != std::string::npos;
    const bool right = value.find("right") != std::string::npos;
    const bool bottom = value.find("bottom") != std::string::npos;
    const bool left = value.find("left") != std::string::npos;
    if (top && right) {
        return 45.0f;
    }
    if (bottom && right) {
        return 135.0f;
    }
    if (bottom && left) {
        return 225.0f;
    }
    if (top && left) {
        return 315.0f;
    }
    if (top) {
        return 0.0f;
    }
    if (right) {
        return 90.0f;
    }
    if (bottom) {
        return 180.0f;
    }
    if (left) {
        return 270.0f;
    }
    return std::nullopt;
}

void parseRadialGradientHeader(std::string_view raw, Gradient& gradient) {
    const std::string value = lower(trim(raw));
    const size_t at = value.find(" at ");
    if (at == std::string::npos) {
        return;
    }
    const std::vector<std::string> coordinates = splitCssTokens(
        std::string_view(value).substr(at + 4));
    if (coordinates.empty() || coordinates.size() > 2) {
        return;
    }
    if (std::optional<Length> x = parseBackgroundPositionValue(
            coordinates[0],
            true)) {
        gradient.centerX = *x;
    }
    const std::string_view yValue = coordinates.size() == 2
        ? std::string_view(coordinates[1])
        : std::string_view(coordinates[0]);
    if (std::optional<Length> y = parseBackgroundPositionValue(
            yValue,
            false)) {
        gradient.centerY = *y;
    }
}

std::optional<Gradient> parseGradientLayer(std::string_view raw) {
    std::string value = trim(raw);
    const std::string valueLower = lower(value);
    Gradient gradient;
    if (valueLower.starts_with("linear-gradient-x(") && value.ends_with(')')) {
        gradient.kind = GradientKind::LinearX;
        gradient.angleDegrees = 90.0f;
        value = value.substr(18, value.size() - 19);
    } else if (valueLower.starts_with("linear-gradient-y(") && value.ends_with(')')) {
        gradient.kind = GradientKind::LinearY;
        value = value.substr(18, value.size() - 19);
    } else if (valueLower.starts_with("linear-gradient(") && value.ends_with(')')) {
        gradient.kind = GradientKind::LinearY;
        value = trim(std::string_view(value).substr(16, value.size() - 17));
    } else if (valueLower.starts_with("radial-gradient(") && value.ends_with(')')) {
        gradient.kind = GradientKind::Radial;
        value = value.substr(16, value.size() - 17);
    } else {
        return std::nullopt;
    }

    std::vector<std::string> arguments = splitCommaList(value);
    if (arguments.empty()) {
        return std::nullopt;
    }
    if (gradient.kind == GradientKind::Radial) {
        const std::string first = lower(arguments.front());
        if (first.starts_with("circle") || first.starts_with("ellipse") ||
            first.find(" at ") != std::string::npos) {
            parseRadialGradientHeader(arguments.front(), gradient);
            arguments.erase(arguments.begin());
        }
    } else if (std::optional<float> angle =
                   parseLinearGradientDirection(arguments.front())) {
        gradient.kind = GradientKind::LinearAngle;
        gradient.angleDegrees = *angle;
        arguments.erase(arguments.begin());
    }

    for (const std::string& argument : arguments) {
        SkColor color = SK_ColorTRANSPARENT;
        std::optional<Length> position;
        if (!parseGradientColorStop(argument, color, position)) {
            return std::nullopt;
        }
        gradient.colors.push_back(color);
        gradient.stopPositions.push_back(position);
    }
    if (gradient.colors.size() < 2) {
        return std::nullopt;
    }
    return gradient;
}

void parseGradients(std::string_view raw, Style& style) {
    std::vector<Gradient> gradients;
    for (const std::string& layer : splitCommaList(raw)) {
        if (std::optional<Gradient> gradient = parseGradientLayer(layer)) {
            gradients.push_back(std::move(*gradient));
        }
    }
    if (!gradients.empty()) {
        style.backgroundGradients = std::move(gradients);
        style.flags.backgroundGradient = true;
    }
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

void parseStyleSheet(std::string_view css, std::vector<StyleRule>& rules,
                     std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                     CssEnvironment& environment, MediaContext media = {},
                     bool prepareEnvironment = true);

bool readFile(const std::string& path, std::string& out);
std::string unquoteCssValue(std::string value);

std::optional<std::string> resolveLocalStylesheetPath(std::string_view rawHref,
                                                      std::string_view basePath) {
    const std::string href = trim(rawHref);
    if (href.empty() || href.front() == '#') {
        return std::nullopt;
    }

    const std::string hrefLower = lower(href);
    if (hrefLower.starts_with("http://") ||
        hrefLower.starts_with("https://") ||
        hrefLower.starts_with("data:")) {
        return std::nullopt;
    }

    std::filesystem::path path = pathFromUtf8(href);
    if (path.is_absolute()) {
        return pathToUtf8(path);
    }

    if (!basePath.empty()) {
        return pathToUtf8(pathFromUtf8(basePath) / path);
    }
    return pathToUtf8(path);
}

bool parseLinkedStyleSheet(lxb_dom_element_t* element, std::string_view basePath,
                           std::vector<StyleRule>& rules,
                           std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                           CssEnvironment& environment, std::string& error) {
    const std::vector<std::string> relTokens = splitWhitespace(lower(attr(element, "rel")));
    if (std::find(relTokens.begin(), relTokens.end(), "stylesheet") == relTokens.end()) {
        return true;
    }

    const std::optional<std::string> path =
        resolveLocalStylesheetPath(attr(element, "href"), basePath);
    if (!path) {
        return true;
    }

    std::string css;
    if (!readFile(*path, css)) {
        error = "无法读取 CSS 文件: " + *path;
        return false;
    }
    parseStyleSheet(css, rules, keyframes, environment);
    return true;
}
void collectStyleSheets(lxb_dom_node_t* root, std::string_view basePath,
                        std::vector<StyleRule>& rules,
                        std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                        CssEnvironment& environment, std::string& error) {
    if (!root) {
        return;
    }
    if (root->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t* element = lxb_dom_interface_element(root);
        const std::string tag = nodeName(element);
        if (tag == "link") {
            if (!parseLinkedStyleSheet(element, basePath, rules, keyframes, environment, error)) {
                return;
            }
        } else if (tag == "style") {
            std::string css;
            for (lxb_dom_node_t* child = root->first_child; child; child = child->next) {
                if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
                    auto* data = lxb_dom_interface_character_data(child);
                    if (data && data->data.data && data->data.length > 0) {
                        css.append(reinterpret_cast<const char*>(data->data.data), data->data.length);
                    }
                }
            }
            parseStyleSheet(css, rules, keyframes, environment);
        }
    }
    for (lxb_dom_node_t* child = root->first_child; child; child = child->next) {
        if (!error.empty()) {
            return;
        }
        collectStyleSheets(child, basePath, rules, keyframes, environment, error);
    }
}

bool collectDocumentType(lxb_dom_node_t* root,
                         std::optional<DocumentType>& declaredType,
                         std::string& error) {
    if (!root) {
        return true;
    }
    if (root->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t* element = lxb_dom_interface_element(root);
        if (nodeName(element) == "meta" &&
            lower(trim(attr(element, "name"))) == "skui-document-type") {
            const std::string value = lower(trim(attr(element, "content")));
            std::optional<DocumentType> currentType;
            if (value == "page") {
                currentType = DocumentType::Page;
            } else if (value == "layout") {
                currentType = DocumentType::Layout;
            } else {
                error = "invalid skui-document-type: " + value;
                return false;
            }
            if (declaredType && *declaredType != *currentType) {
                error = "conflicting skui-document-type declarations";
                return false;
            }
            declaredType = currentType;
        }
    }
    for (lxb_dom_node_t* child = root->first_child; child; child = child->next) {
        if (!collectDocumentType(child, declaredType, error)) {
            return false;
        }
    }
    return true;
}

bool validateDocumentNode(const Node& node,
                          DocumentType documentType,
                          std::unordered_set<std::string>& pageIds,
                          std::string& error) {
    if (node.tag == "skui-page") {
        if (documentType != DocumentType::Layout) {
            error = "<skui-page> is only valid in layout documents";
            return false;
        }
        if (node.id.empty()) {
            error = "<skui-page> requires a non-empty id attribute";
            return false;
        }
        if (node.src.empty()) {
            error = "<skui-page id=\"" + node.id + "\"> requires a src attribute";
            return false;
        }
        if (!pageIds.insert(node.id).second) {
            error = "duplicate <skui-page> id: " + node.id;
            return false;
        }
    }
    for (const auto& child : node.children) {
        if (!validateDocumentNode(*child, documentType, pageIds, error)) {
            return false;
        }
    }
    return true;
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
        const std::string v = lower(value);
        if (v == "none") {
            style.display = Display::None;
        } else if (v == "grid" || v == "inline-grid") {
            style.display = Display::Grid;
        } else {
            style.display = Display::Flex;
        }
        style.displayFlex = v == "flex" || v == "inline-flex" ||
                            v == "grid" || v == "inline-grid";
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
    } else if (name == "z-index") {
        if (std::optional<int> zIndex = parseZIndex(value)) {
            style.zIndex = *zIndex;
            style.flags.zIndex = true;
        }
    } else if (name == "flex-direction") {
        const std::string v = lower(value);
        style.flexDirection = v == "row" ? YGFlexDirectionRow : YGFlexDirectionColumn;
        style.flags.flexDirection = true;
    } else if (name == "flex-wrap") {
        const std::string v = lower(value);
        style.flexWrap = v == "wrap" ? YGWrapWrap : YGWrapNoWrap;
        style.flags.flexWrap = true;
    } else if (name == "gap") {
        std::vector<std::string> values = splitCssTokens(value);
        if (!values.empty() && values.size() <= 2) {
            std::optional<Length> rowGap = parseLength(values[0]);
            std::optional<Length> columnGap = values.size() > 1 ? parseLength(values[1]) : rowGap;
            if (rowGap && columnGap) {
                style.rowGap = *rowGap;
                style.columnGap = *columnGap;
                style.flags.rowGap = true;
                style.flags.columnGap = true;
            }
        }
    } else if (name == "row-gap" && length) {
        style.rowGap = *length;
        style.flags.rowGap = true;
    } else if (name == "column-gap" && length) {
        style.columnGap = *length;
        style.flags.columnGap = true;
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
    } else if (name == "flex") {
        parseFlexShorthand(value, style);
    } else if (name == "flex-basis" && length) {
        style.flexBasis = *length;
        style.flags.flexBasis = true;
    } else if (name == "grid-template-columns") {
        if (std::optional<std::vector<GridTrack>> tracks =
                parseGridTrackList(value)) {
            style.gridTemplateColumns = std::move(*tracks);
            style.flags.gridTemplateColumns = true;
        }
    } else if (name == "grid-template-rows") {
        if (std::optional<std::vector<GridTrack>> tracks =
                parseGridTrackList(value)) {
            style.gridTemplateRows = std::move(*tracks);
            style.flags.gridTemplateRows = true;
        }
    } else if (name == "grid-auto-rows") {
        if (std::optional<GridTrack> track = parseGridTrack(value)) {
            style.gridAutoRows = *track;
            style.flags.gridAutoRows = true;
        }
    } else if (name == "grid-column") {
        if (std::optional<std::pair<GridLine, GridLine>> column =
                parseGridColumn(value)) {
            style.gridColumnStart = column->first;
            style.gridColumnEnd = column->second;
            style.flags.gridColumnStart = true;
            style.flags.gridColumnEnd = true;
        }
    } else if (name == "grid-column-start") {
        if (std::optional<GridLine> line = parseGridLine(value)) {
            style.gridColumnStart = *line;
            style.flags.gridColumnStart = true;
        }
    } else if (name == "grid-column-end") {
        if (std::optional<GridLine> line = parseGridLine(value)) {
            style.gridColumnEnd = *line;
            style.flags.gridColumnEnd = true;
        }
    } else if (name == "grid-row") {
        if (std::optional<std::pair<GridLine, GridLine>> row = parseGridColumn(value)) {
            style.gridRowStart = row->first;
            style.gridRowEnd = row->second;
            style.flags.gridRowStart = true;
            style.flags.gridRowEnd = true;
        }
    } else if (name == "grid-row-start") {
        if (std::optional<GridLine> line = parseGridLine(value)) {
            style.gridRowStart = *line;
            style.flags.gridRowStart = true;
        }
    } else if (name == "grid-row-end") {
        if (std::optional<GridLine> line = parseGridLine(value)) {
            style.gridRowEnd = *line;
            style.flags.gridRowEnd = true;
        }
    } else if (name == "justify-items") {
        if (std::optional<GridItemAlignment> alignment =
                parseGridItemAlignment(value)) {
            style.justifyItems = *alignment;
            style.flags.justifyItems = true;
        }
    } else if (name == "justify-self") {
        if (std::optional<GridItemAlignment> alignment =
                parseGridItemAlignment(value)) {
            style.justifySelf = *alignment;
            style.flags.justifySelf = true;
        }
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
    } else if (name == "inset") {
        applyEdgeShorthand(style.inset,
                           style.flags,
                           &Style::Flags::insetLeft,
                           &Style::Flags::insetTop,
                           &Style::Flags::insetRight,
                           &Style::Flags::insetBottom,
                           value);
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
        } else {
            parseGradients(value, style);
        }
    } else if (name == "mask-image" || name == "-webkit-mask-image") {
        if (std::optional<Gradient> gradient = parseGradientLayer(value)) {
            style.maskGradient = std::move(*gradient);
            style.flags.maskImage = true;
        } else if (lower(value) == "none") {
            style.maskGradient.reset();
            style.flags.maskImage = true;
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
            parseGradients(value, style);
        }
        if (!style.flags.backgroundGradient && !style.flags.backgroundImage) {
            style.backgroundColor = parseColor(value, style.backgroundColor);
            style.flags.backgroundColor = true;
        }
    } else if (name == "border") {
        applyBorderShorthand(style, value);
    } else if (name == "border-color") {
        const SkColor color = parseColor(value, style.color);
        style.borders.left.color = color;
        style.borders.top.color = color;
        style.borders.right.color = color;
        style.borders.bottom.color = color;
    } else if (name == "border-width" && number) {
        const float width = std::max(0.0f, *number);
        style.borders.left.width = width;
        style.borders.top.width = width;
        style.borders.right.width = width;
        style.borders.bottom.width = width;
    } else if (name == "border-style") {
        const std::string v = lower(value);
        const BorderStyle borderStyle =
            v == "solid" ? BorderStyle::Solid : BorderStyle::None;
        style.borders.left.style = borderStyle;
        style.borders.top.style = borderStyle;
        style.borders.right.style = borderStyle;
        style.borders.bottom.style = borderStyle;
    } else if (name == "border-left") {
        applyBorderShorthand(style.borders.left, value);
    } else if (name == "border-top") {
        applyBorderShorthand(style.borders.top, value);
    } else if (name == "border-right") {
        applyBorderShorthand(style.borders.right, value);
    } else if (name == "border-bottom") {
        applyBorderShorthand(style.borders.bottom, value);
    } else if (name == "border-left-color") {
        style.borders.left.color = parseColor(value, style.color);
    } else if (name == "border-top-color") {
        style.borders.top.color = parseColor(value, style.color);
    } else if (name == "border-right-color") {
        style.borders.right.color = parseColor(value, style.color);
    } else if (name == "border-bottom-color") {
        style.borders.bottom.color = parseColor(value, style.color);
    } else if (name == "border-left-width" && number) {
        style.borders.left.width = std::max(0.0f, *number);
    } else if (name == "border-top-width" && number) {
        style.borders.top.width = std::max(0.0f, *number);
    } else if (name == "border-right-width" && number) {
        style.borders.right.width = std::max(0.0f, *number);
    } else if (name == "border-bottom-width" && number) {
        style.borders.bottom.width = std::max(0.0f, *number);
    } else if (name == "border-left-style") {
        style.borders.left.style = parseBorderStyleValue(value);
    } else if (name == "border-top-style") {
        style.borders.top.style = parseBorderStyleValue(value);
    } else if (name == "border-right-style") {
        style.borders.right.style = parseBorderStyleValue(value);
    } else if (name == "border-bottom-style") {
        style.borders.bottom.style = parseBorderStyleValue(value);
    } else if (name == "border-radius") {
        applyBorderRadiusShorthand(style, value);
    } else if (name == "border-top-left-radius" && length && length->unit != LengthUnit::Auto) {
        setBorderCornerRadius(style, &Style::Flags::borderTopLeftRadius, &CornerRadii::topLeft,
                              *length);
    } else if (name == "border-top-right-radius" && length && length->unit != LengthUnit::Auto) {
        setBorderCornerRadius(style, &Style::Flags::borderTopRightRadius, &CornerRadii::topRight,
                              *length);
    } else if (name == "border-bottom-right-radius" && length && length->unit != LengthUnit::Auto) {
        setBorderCornerRadius(style, &Style::Flags::borderBottomRightRadius,
                              &CornerRadii::bottomRight, *length);
    } else if (name == "border-bottom-left-radius" && length && length->unit != LengthUnit::Auto) {
        setBorderCornerRadius(style, &Style::Flags::borderBottomLeftRadius,
                              &CornerRadii::bottomLeft, *length);
    } else if (name == "box-shadow") {
        if (std::optional<std::vector<Shadow>> shadows =
                parseShadowList(value, true, true)) {
            style.boxShadows = std::move(*shadows);
            style.flags.boxShadow = true;
        }
    } else if (name == "text-shadow") {
        if (std::optional<std::vector<Shadow>> shadows =
                parseShadowList(value, false, false)) {
            style.textShadows = std::move(*shadows);
            style.flags.textShadow = true;
        }
    } else if (name == "content") {
        style.content = unquoteCssValue(value);
        style.flags.content = true;
    } else if (name == "font-size" && number) {
        style.fontSize = *number;
        style.flags.fontSize = true;
    } else if (name == "font-weight") {
        style.fontBold = lower(value) == "bold" || value == "600" || value == "700";
        style.flags.fontBold = true;
    } else if (name == "line-height") {
        if (std::optional<float> multiplier = parseUnitlessFloat(value);
            multiplier && *multiplier > 0.0f) {
            style.lineHeight = *multiplier;
            style.flags.lineHeight = true;
        }
    } else if (name == "text-align") {
        const std::string alignment = lower(value);
        if (alignment == "center") {
            style.textAlign = TextAlign::Center;
        } else if (alignment == "right" || alignment == "end") {
            style.textAlign = TextAlign::Right;
        } else {
            style.textAlign = TextAlign::Left;
        }
        style.flags.textAlign = true;
    } else if (name == "text-overflow") {
        style.textOverflowEllipsis = lower(value) == "ellipsis";
        style.flags.textOverflow = true;
    } else if (name == "white-space") {
        style.whiteSpaceNoWrap = lower(value) == "nowrap";
        style.flags.whiteSpace = true;
    } else if (name == "accent-color") {
        style.color = parseColor(value, style.color);
        style.flags.color = true;
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
    } else if (name == "transform-origin") {
        if (std::optional<TransformOrigin> origin = parseTransformOrigin(value)) {
            style.transformOrigin = *origin;
            style.flags.transformOrigin = true;
        }
    } else if (name == "filter") {
        if (std::optional<Filter> filter = parseFilter(value)) {
            style.filter = std::move(*filter);
            style.flags.filter = true;
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

struct DeclarationValue {
    std::string value;
    bool important = false;
};

std::optional<std::string> resolveCssVariables(std::string_view raw,
                                               const CssEnvironment& environment, int depth = 0) {
    if (depth > 16) {
        return std::nullopt;
    }

    std::string resolved(raw);
    size_t search = 0;
    while (true) {
        const size_t open = lower(resolved).find("var(", search);
        if (open == std::string::npos) {
            return resolved;
        }

        size_t close = open + 4;
        int parentheses = 1;
        for (; close < resolved.size() && parentheses > 0; ++close) {
            if (resolved[close] == '(') {
                ++parentheses;
            } else if (resolved[close] == ')') {
                --parentheses;
            }
        }
        if (parentheses != 0) {
            return std::nullopt;
        }

        const size_t bodyEnd = close - 1;
        const std::string_view body(resolved.data() + open + 4, bodyEnd - open - 4);
        size_t comma = std::string_view::npos;
        int nested = 0;
        for (size_t index = 0; index < body.size(); ++index) {
            if (body[index] == '(') {
                ++nested;
            } else if (body[index] == ')' && nested > 0) {
                --nested;
            } else if (body[index] == ',' && nested == 0) {
                comma = index;
                break;
            }
        }

        const std::string name =
            trim(body.substr(0, comma == std::string_view::npos ? body.size() : comma));
        std::string replacement;
        const auto variable = environment.rootVariables.find(name);
        if (variable != environment.rootVariables.end()) {
            std::optional<std::string> nestedValue =
                resolveCssVariables(variable->second, environment, depth + 1);
            if (!nestedValue) {
                return std::nullopt;
            }
            replacement = std::move(*nestedValue);
        } else if (comma != std::string_view::npos) {
            std::optional<std::string> fallback =
                resolveCssVariables(body.substr(comma + 1), environment, depth + 1);
            if (!fallback) {
                return std::nullopt;
            }
            replacement = trim(*fallback);
        } else {
            return std::nullopt;
        }

        resolved.replace(open, close - open, replacement);
        search = open;
    }
}

std::string resolveRemLengths(std::string_view raw, float rootFontSize) {
    std::string resolved;
    resolved.reserve(raw.size());
    char quote = '\0';
    for (size_t index = 0; index < raw.size();) {
        const char ch = raw[index];
        if (quote != '\0') {
            resolved.push_back(ch);
            if (ch == quote && (index == 0 || raw[index - 1] != '\\')) {
                quote = '\0';
            }
            ++index;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            resolved.push_back(ch);
            ++index;
            continue;
        }

        const bool signedNumber = (ch == '+' || ch == '-') && index + 1 < raw.size() &&
                                  (std::isdigit(static_cast<unsigned char>(raw[index + 1])) != 0 ||
                                   raw[index + 1] == '.');
        const bool numberStart = std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
                                 (ch == '.' && index + 1 < raw.size() &&
                                  std::isdigit(static_cast<unsigned char>(raw[index + 1])) != 0) ||
                                 signedNumber;
        if (!numberStart) {
            resolved.push_back(ch);
            ++index;
            continue;
        }

        size_t numberEnd = index + (signedNumber ? 1 : 0);
        bool sawDot = false;
        while (numberEnd < raw.size()) {
            const char numberChar = raw[numberEnd];
            if (std::isdigit(static_cast<unsigned char>(numberChar)) != 0) {
                ++numberEnd;
                continue;
            }
            if (numberChar == '.' && !sawDot) {
                sawDot = true;
                ++numberEnd;
                continue;
            }
            break;
        }

        if (numberEnd + 3 <= raw.size() && lower(std::string(raw.substr(numberEnd, 3))) == "rem") {
            float multiplier = 0.0f;
            const std::string number(raw.substr(index, numberEnd - index));
            const auto parsed =
                std::from_chars(number.data(), number.data() + number.size(), multiplier);
            if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size()) {
                std::ostringstream stream;
                stream << multiplier * rootFontSize << "px";
                resolved += stream.str();
                index = numberEnd + 3;
                continue;
            }
        }

        resolved.append(raw.substr(index, numberEnd - index));
        index = numberEnd;
    }
    return resolved;
}

std::optional<std::string> resolveCssValue(std::string_view raw,
                                           const CssEnvironment& environment) {
    std::optional<std::string> variables = resolveCssVariables(raw, environment);
    if (!variables) {
        return std::nullopt;
    }
    return resolveRemLengths(*variables, environment.rootFontSize);
}

DeclarationValue parseDeclarationValue(std::string_view rawValue) {
    DeclarationValue parsed{trim(rawValue)};
    const size_t marker = parsed.value.rfind('!');
    if (marker == std::string::npos ||
        lower(trim(std::string_view(parsed.value).substr(marker + 1))) !=
            "important") {
        return parsed;
    }

    parsed.value = trim(std::string_view(parsed.value).substr(0, marker));
    parsed.important = true;
    return parsed;
}

void parseDeclarations(std::string_view block, Style& style, Style& importantStyle,
                       const CssEnvironment& environment) {
    size_t start = 0;
    while (start < block.size()) {
        const size_t semi = block.find(';', start);
        const std::string_view declaration =
            block.substr(start,
                         semi == std::string_view::npos ? block.size() - start
                                                        : semi - start);
        const size_t colon = declaration.find(':');
        if (colon != std::string_view::npos) {
            DeclarationValue value =
                parseDeclarationValue(declaration.substr(colon + 1));
            const std::string name = trim(declaration.substr(0, colon));
            std::optional<std::string> resolvedValue = resolveCssValue(value.value, environment);
            if (!name.starts_with("--") && resolvedValue && !resolvedValue->empty()) {
                Style& destination = value.important ? importantStyle : style;
                applyDeclaration(destination, name, *resolvedValue);
            }
        }
        if (semi == std::string_view::npos) {
            break;
        }
        start = semi + 1;
    }
}

void parseDeclarations(std::string_view block, Style& style,
                       const CssEnvironment& environment = {}) {
    Style ignoredImportantStyle;
    parseDeclarations(block, style, ignoredImportantStyle, environment);
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
                const size_t open = pos;
                int depth = 0;
                do {
                    if (raw[pos] == '(') {
                        ++depth;
                    } else if (raw[pos] == ')') {
                        --depth;
                    }
                    ++pos;
                } while (pos < raw.size() && depth > 0);
                if (depth != 0) {
                    return std::nullopt;
                }
                pseudo += std::string(raw.substr(open, pos - open));
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
    std::string normalized = trim(selectorText);
    if (normalized.ends_with("::before")) {
        normalized.resize(normalized.size() - std::string_view("::before").size());
        rule.pseudoElement = PseudoElement::Before;
        ++rule.specificity;
    } else if (normalized.ends_with("::after")) {
        normalized.resize(normalized.size() - std::string_view("::after").size());
        rule.pseudoElement = PseudoElement::After;
        ++rule.specificity;
    }
    normalized = trim(normalized);
    selectorText = normalized;
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

bool selectorListContainsRoot(std::string_view selectorText) {
    for (const std::string& selector : splitSelectorList(selectorText)) {
        if (lower(trim(selector)) == ":root") {
            return true;
        }
    }
    return false;
}

template <typename Visitor> void visitDeclarations(std::string_view block, Visitor visitor) {
    size_t start = 0;
    while (start < block.size()) {
        const size_t semi = block.find(';', start);
        const std::string_view declaration = block.substr(
            start, semi == std::string_view::npos ? block.size() - start : semi - start);
        const size_t colon = declaration.find(':');
        if (colon != std::string_view::npos) {
            const std::string name = trim(declaration.substr(0, colon));
            DeclarationValue value = parseDeclarationValue(declaration.substr(colon + 1));
            if (!name.empty() && !value.value.empty()) {
                visitor(name, value);
            }
        }
        if (semi == std::string_view::npos) {
            break;
        }
        start = semi + 1;
    }
}

void prepareRootEnvironment(std::string_view css, CssEnvironment& environment) {
    struct RootBlock {
        std::string declarations;
    };
    std::vector<RootBlock> rootBlocks;
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
        const std::string selector = trim(css.substr(start, open - start));
        if (!selector.empty() && selector.front() != '@' && selectorListContainsRoot(selector)) {
            rootBlocks.push_back({
                trim(css.substr(open + 1, *close - open - 1)),
            });
        }
        start = *close + 1;
    }

    for (const RootBlock& rootBlock : rootBlocks) {
        visitDeclarations(rootBlock.declarations,
                          [&](const std::string& name, const DeclarationValue& value) {
                              if (name.starts_with("--")) {
                                  environment.rootVariables[name] = value.value;
                              }
                          });
    }
    for (const RootBlock& rootBlock : rootBlocks) {
        visitDeclarations(
            rootBlock.declarations, [&](const std::string& name, const DeclarationValue& value) {
                if (lower(name) != "font-size") {
                    return;
                }
                std::optional<std::string> resolved = resolveCssValue(value.value, environment);
                if (resolved) {
                    if (std::optional<float> fontSize = parseNumberOrPx(*resolved);
                        fontSize && *fontSize > 0.0f) {
                        environment.rootFontSize = *fontSize;
                    }
                }
            });
    }
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

void parseKeyframes(std::string_view name, std::string_view css,
                    std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                    const CssEnvironment& environment) {
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
        parseDeclarations(block, style, environment);
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

void parseStyleSheet(std::string_view css, std::vector<StyleRule>& rules,
                     std::unordered_map<std::string, KeyframesDefinition>& keyframes,
                     CssEnvironment& environment, MediaContext media, bool prepareEnvironment) {
    const std::string cleanedCss = stripCssComments(css);
    css = cleanedCss;
    if (prepareEnvironment) {
        prepareRootEnvironment(css, environment);
    }
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
            parseStyleSheet(block, rules, keyframes, environment, nestedMedia, false);
            start = *close + 1;
            continue;
        }
        constexpr std::string_view keyframesPrefix = "@keyframes";
        constexpr std::string_view webkitKeyframesPrefix = "@-webkit-keyframes";
        if (selectorLower.rfind(keyframesPrefix, 0) == 0) {
            parseKeyframes(std::string_view(selectorText).substr(keyframesPrefix.size()), block,
                           keyframes, environment);
            start = *close + 1;
            continue;
        }
        if (selectorLower.rfind(webkitKeyframesPrefix, 0) == 0) {
            parseKeyframes(std::string_view(selectorText).substr(webkitKeyframesPrefix.size()),
                           block, keyframes, environment);
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
            parseDeclarations(block, rule.style, rule.importantStyle, environment);
            rules.push_back(std::move(rule));
        }
        start = *close + 1;
    }
}

std::unique_ptr<Node>
convertElement(lxb_dom_element_t* element, Node* parent, std::vector<StyleRule>& rules,
               std::unordered_map<std::string, KeyframesDefinition>& keyframes,
               CssEnvironment& environment, std::string_view basePath, std::string& error) {
    if (!element) {
        return nullptr;
    }
    const std::string tag = nodeName(element);
    if (tag == "link") {
        parseLinkedStyleSheet(element, basePath, rules, keyframes, environment, error);
        return nullptr;
    }
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
        parseStyleSheet(css, rules, keyframes, environment);
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
        parseDeclarations(inlineStyle, node->inlineStyle, node->inlineImportantStyle, environment);
    }

    if (tag == "img" || tag == "video" || tag == "svg") {
        if (std::optional<Length> width =
                parseLength(attributeValue(node->attributes, "width"))) {
            node->presentationStyle.width = *width;
            node->presentationStyle.flags.width = true;
        }
        if (std::optional<Length> height =
                parseLength(attributeValue(node->attributes, "height"))) {
            node->presentationStyle.height = *height;
            node->presentationStyle.flags.height = true;
        }
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
                    convertElement(lxb_dom_interface_element(child), node.get(), rules, keyframes,
                                   environment, basePath, error);
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

void parseInlineStyle(std::string_view declarations, Style& style, Style& importantStyle,
                      const CssEnvironment& environment) {
    parseDeclarations(declarations, style, importantStyle, environment);
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
    CssEnvironment environment;
    std::optional<DocumentType> declaredType;
    if (lxb_html_head_element_t* head = lxb_html_document_head_element(htmlDocument.get())) {
        if (!collectDocumentType(
                lxb_dom_interface_node(head), declaredType, error)) {
            return false;
        }
        collectStyleSheets(lxb_dom_interface_node(head), basePath, rules, keyframes, environment,
                           error);
        if (!error.empty()) {
            return false;
        }
    }

    lxb_dom_element_t* documentElement =
        lxb_dom_document_element(lxb_dom_interface_document(htmlDocument.get()));
    std::unique_ptr<Node> root =
        convertElement(documentElement, nullptr, rules, keyframes, environment, basePath, error);
    if (!error.empty()) {
        return false;
    }
    if (!root || root->tag != "html") {
        error = "HTML document element conversion failed";
        return false;
    }

    const DocumentType documentType = declaredType.value_or(DocumentType::Page);
    std::unordered_set<std::string> pageIds;
    if (!validateDocumentNode(*root, documentType, pageIds, error)) {
        return false;
    }

    outDocument.root = std::move(root);
    outDocument.rules = std::move(rules);
    outDocument.keyframes = std::move(keyframes);
    outDocument.cssEnvironment = std::move(environment);
    outDocument.basePath = std::string(basePath);
    outDocument.type = documentType;
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

bool DocumentParser::parseClipboardHtml(
    std::string_view html,
    std::vector<ClipboardItem>& outItems,
    std::string& error) {
    HtmlDocumentPtr htmlDocument(lxb_html_document_create());
    if (!htmlDocument) {
        error = "Lexbor 创建剪贴板文档失败";
        return false;
    }
    if (lxb_html_document_parse(
            htmlDocument.get(),
            reinterpret_cast<const lxb_char_t*>(html.data()),
            html.size()) != LXB_STATUS_OK) {
        error = "Lexbor 解析剪贴板 HTML 失败";
        return false;
    }

    lxb_html_body_element_t* body =
        lxb_html_document_body_element(htmlDocument.get());
    if (!body) {
        error = "剪贴板 HTML 没有 body";
        return false;
    }

    outItems.clear();
    for (lxb_dom_node_t* child =
             lxb_dom_interface_node(body)->first_child;
         child;
         child = child->next) {
        collectClipboardItems(child, outItems);
    }
    trimClipboardBoundaryLineBreaks(outItems);
    error.clear();
    return true;
}

void recomputeStyles(Document& document, const RuntimeOptions& options, float viewportWidth, float viewportHeight) {
    if (!document.root) {
        return;
    }
    resetStyles(*document.root);
    applyInheritedStyle(*document.root, options);
    applyPresentationStyles(*document.root);
    applyRules(*document.root,
               document.rules,
               viewportWidth,
               viewportHeight,
               false);
    applyInheritedStyle(*document.root, options);
    applyInlineStyles(*document.root, false);
    applyInheritedStyle(*document.root, options);
    applyRules(*document.root,
               document.rules,
               viewportWidth,
               viewportHeight,
               true);
    applyInlineStyles(*document.root, true);
    applyImportantStyles(*document.root);
    applyInheritedStyle(*document.root, options);
}

void applyAnimatedStyles(Node& node) {
    applyAnimatedStylesRecursive(node);
}

}  // namespace skui
