#include "skui_internal.h"

#include "include/ports/SkTypeface_win.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace skui {

namespace {

bool isTextareaScrollableNode(const Node& node) {
    return node.tag == "textarea";
}

struct UiFontResources {
    sk_sp<SkFontMgr> manager;
    sk_sp<SkTypeface> regular;
    sk_sp<SkTypeface> bold;
};

struct DecodedCodepoint {
    SkUnichar value = 0;
    size_t length = 0;
};

struct PositionedGlyph {
    sk_sp<SkTypeface> typeface;
    SkGlyphID glyph = 0;
    float x = 0.0f;
};

struct FallbackTypefaceCache {
    std::mutex mutex;
    std::unordered_map<uint64_t, sk_sp<SkTypeface>> typefaces;
};

sk_sp<SkTypeface> pickUiTypeface(const sk_sp<SkFontMgr>& manager, bool bold) {
    if (!manager) {
        return nullptr;
    }

    const SkFontStyle style =
        bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
    constexpr std::array<const char*, 5> kFamilies = {
        "Microsoft YaHei UI",
        "Microsoft YaHei",
        "Segoe UI",
        "Arial",
        nullptr,
    };
    for (const char* family : kFamilies) {
        sk_sp<SkTypeface> typeface =
            manager->matchFamilyStyle(family, style);
        if (typeface) {
            return typeface;
        }
    }
    return nullptr;
}

UiFontResources createUiFontResources() {
    UiFontResources resources;
    resources.manager = SkFontMgr_New_DirectWrite();
    if (!resources.manager) {
        resources.manager = SkFontMgr_New_GDI();
    }
    resources.regular = pickUiTypeface(resources.manager, false);
    resources.bold = pickUiTypeface(resources.manager, true);
    return resources;
}

const UiFontResources& uiFontResources() {
    static const UiFontResources resources = createUiFontResources();
    return resources;
}

FallbackTypefaceCache& fallbackTypefaceCache() {
    static FallbackTypefaceCache cache;
    return cache;
}

SkFont makeConfiguredFont(sk_sp<SkTypeface> typeface, float size) {
    SkFont font(std::move(typeface), size);
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setSubpixel(true);
    return font;
}

DecodedCodepoint decodeUtf8Codepoint(std::string_view value, size_t offset) {
    constexpr SkUnichar kReplacementCharacter = 0xFFFD;
    const auto byte = [&](size_t index) {
        return static_cast<unsigned char>(value[index]);
    };
    const unsigned char lead = byte(offset);
    if (lead < 0x80u) {
        return {static_cast<SkUnichar>(lead), 1};
    }

    int continuationCount = 0;
    uint32_t codepoint = 0;
    uint32_t minimumCodepoint = 0;
    if ((lead & 0xE0u) == 0xC0u) {
        continuationCount = 1;
        codepoint = lead & 0x1Fu;
        minimumCodepoint = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
        continuationCount = 2;
        codepoint = lead & 0x0Fu;
        minimumCodepoint = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
        continuationCount = 3;
        codepoint = lead & 0x07u;
        minimumCodepoint = 0x10000u;
    } else {
        return {kReplacementCharacter, 1};
    }

    if (offset + static_cast<size_t>(continuationCount) >= value.size()) {
        return {kReplacementCharacter, 1};
    }
    for (int i = 1; i <= continuationCount; ++i) {
        const unsigned char next = byte(offset + static_cast<size_t>(i));
        if ((next & 0xC0u) != 0x80u) {
            return {kReplacementCharacter, 1};
        }
        codepoint = (codepoint << 6u) | (next & 0x3Fu);
    }

    const bool surrogate = codepoint >= 0xD800u && codepoint <= 0xDFFFu;
    if (codepoint < minimumCodepoint || codepoint > 0x10FFFFu || surrogate) {
        return {kReplacementCharacter, 1};
    }
    return {
        static_cast<SkUnichar>(codepoint),
        static_cast<size_t>(continuationCount + 1),
    };
}

bool isEmojiCharacter(SkUnichar codepoint) {
    return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) ||
           (codepoint >= 0x2600 && codepoint <= 0x27BF);
}

bool isEmojiFormatCharacter(SkUnichar codepoint) {
    return codepoint == 0x200D || codepoint == 0xFE0E || codepoint == 0xFE0F;
}

sk_sp<SkTypeface> resolveTypeface(SkUnichar codepoint, bool bold) {
    const UiFontResources& resources = uiFontResources();
    const sk_sp<SkTypeface>& primary = bold ? resources.bold : resources.regular;
    if (!isEmojiCharacter(codepoint) &&
        primary &&
        primary->unicharToGlyph(codepoint) != 0) {
        return primary;
    }

    const uint64_t key = static_cast<uint32_t>(codepoint) |
                         (static_cast<uint64_t>(bold) << 32u);
    FallbackTypefaceCache& cache = fallbackTypefaceCache();
    std::lock_guard lock(cache.mutex);
    if (const auto it = cache.typefaces.find(key); it != cache.typefaces.end()) {
        return it->second;
    }

    sk_sp<SkTypeface> resolved;
    if (resources.manager && isEmojiCharacter(codepoint)) {
        constexpr std::array<const char*, 3> kEmojiFamilies = {
            "Segoe UI Emoji",
            "Apple Color Emoji",
            "Noto Color Emoji",
        };
        const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
        for (const char* family : kEmojiFamilies) {
            sk_sp<SkTypeface> candidate =
                resources.manager->matchFamilyStyle(family, style);
            if (candidate && candidate->unicharToGlyph(codepoint) != 0) {
                resolved = std::move(candidate);
                break;
            }
        }
    }
    if (!resolved && primary && primary->unicharToGlyph(codepoint) != 0) {
        resolved = primary;
    }
    if (!resolved && resources.manager) {
        const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
        const char* languages[] = {"zh-Hans"};
        resolved = resources.manager->matchFamilyStyleCharacter(
            nullptr,
            style,
            languages,
            static_cast<int>(std::size(languages)),
            codepoint);
    }
    if (!resolved) {
        resolved = primary;
    }
    cache.typefaces.emplace(key, resolved);
    return resolved;
}

UiTextLayout buildUiTextLayout(std::string_view value,
                               float size,
                               bool bold,
                               bool buildBlob) {
    UiTextLayout layout;
    const SkFont primaryFont = makeUiFont(size, bold);
    primaryFont.getMetrics(&layout.metrics);
    if (value.empty()) {
        return layout;
    }

    const UiFontResources& resources = uiFontResources();
    const sk_sp<SkTypeface>& primary = bold ? resources.bold : resources.regular;
    std::vector<PositionedGlyph> glyphs;
    glyphs.reserve(value.size());
    bool needsFallback = false;
    float x = 0.0f;
    for (size_t offset = 0; offset < value.size();) {
        const DecodedCodepoint decoded = decodeUtf8Codepoint(value, offset);
        offset += decoded.length;
        if (isEmojiFormatCharacter(decoded.value)) {
            needsFallback = true;
            continue;
        }

        sk_sp<SkTypeface> typeface = resolveTypeface(decoded.value, bold);
        if (!typeface) {
            typeface = primary;
        }
        needsFallback = needsFallback || typeface.get() != primary.get();
        const SkFont glyphFont = makeConfiguredFont(typeface, size);
        const SkGlyphID glyph = glyphFont.unicharToGlyph(decoded.value);
        glyphs.push_back({std::move(typeface), glyph, x});
        x += glyphFont.getWidth(glyph);
    }

    if (!needsFallback || glyphs.empty()) {
        layout.width = primaryFont.measureText(
            value.data(),
            value.size(),
            SkTextEncoding::kUTF8,
            &layout.bounds);
        if (buildBlob) {
            layout.blob = SkTextBlob::MakeFromText(
                value.data(),
                value.size(),
                primaryFont,
                SkTextEncoding::kUTF8);
        }
        return layout;
    }

    layout.width = x;
    if (!buildBlob) {
        return layout;
    }

    SkTextBlobBuilder builder;
    for (size_t start = 0; start < glyphs.size();) {
        size_t end = start + 1;
        while (end < glyphs.size() &&
               glyphs[end].typeface.get() == glyphs[start].typeface.get()) {
            ++end;
        }
        const SkFont runFont = makeConfiguredFont(glyphs[start].typeface, size);
        const auto& run = builder.allocRunPosH(
            runFont,
            static_cast<int>(end - start),
            0.0f);
        for (size_t index = start; index < end; ++index) {
            const size_t runIndex = index - start;
            run.glyphs[runIndex] = glyphs[index].glyph;
            run.pos[runIndex] = glyphs[index].x;
        }
        start = end;
    }
    layout.blob = builder.make();
    if (layout.blob) {
        layout.bounds = layout.blob->bounds();
    }
    return layout;
}

template <typename NodePointer>
void sortChildrenByZIndex(std::vector<NodePointer>& children) {
    std::stable_sort(children.begin(), children.end(), [](NodePointer lhs, NodePointer rhs) {
        return lhs->style.zIndex < rhs->style.zIndex;
    });
}

std::string lowerAsciiText(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

bool isContentEditableTextTag(std::string_view tag) {
    return tag == "div" ||
           tag == "p" ||
           tag == "span" ||
           tag == "selectable" ||
           tag == "blockquote" ||
           tag == "li" ||
           tag == "h1" ||
           tag == "h2" ||
           tag == "h3" ||
           tag == "h4" ||
           tag == "h5" ||
           tag == "h6";
}

bool hasContentEditableElementChildren(const Node& node) {
    return std::any_of(
        node.children.begin(),
        node.children.end(),
        [](const std::unique_ptr<Node>& child) {
            return child->tag != "br";
        });
}

void appendTextContent(const Node& node, std::string& result) {
    if (!node.value.empty()) {
        result += node.value;
    } else {
        result += node.text;
    }
    for (const auto& child : node.children) {
        appendTextContent(*child, result);
    }
}

void appendEditableTextContent(const Node& node,
                               const Node& host,
                               std::string& result) {
    if (&node != &host && !isContentEditable(node)) {
        return;
    }
    const bool textNode = isContentEditableTextNode(node);
    if (textNode) {
        const bool startsParagraph =
            node.contentEditableFlowPosition ==
            ContentEditableFlowPosition::ParagraphStart;
        if (!result.empty() &&
            (!usesInlineContentEditableFlow(host) || startsParagraph)) {
            result.push_back('\n');
        }
        result += node.value;
        return;
    }
    for (const auto& child : node.children) {
        appendEditableTextContent(*child, host, result);
    }
}

} // namespace

Theme Theme::dark() {
    return {};
}

SkColor rgb(unsigned r, unsigned g, unsigned b) {
    return SkColorSetRGB(static_cast<uint8_t>(std::min(r, 255u)),
                         static_cast<uint8_t>(std::min(g, 255u)),
                         static_cast<uint8_t>(std::min(b, 255u)));
}

SkColor rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return SkColorSetARGB(static_cast<uint8_t>(std::min(a, 255u)),
                          static_cast<uint8_t>(std::min(r, 255u)),
                          static_cast<uint8_t>(std::min(g, 255u)),
                          static_cast<uint8_t>(std::min(b, 255u)));
}

sk_sp<SkFontMgr> uiFontManager() {
    return uiFontResources().manager;
}

SkFont makeUiFont(float size, bool bold) {
    const UiFontResources& resources = uiFontResources();
    return makeConfiguredFont(bold ? resources.bold : resources.regular, size);
}

UiTextLayout makeUiTextLayout(std::string_view value, float size, bool bold) {
    return buildUiTextLayout(value, size, bold, true);
}

float measureUiTextWidth(std::string_view value, float size, bool bold) {
    return buildUiTextLayout(value, size, bold, false).width;
}

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

std::string trim(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::vector<std::string> splitWhitespace(std::string_view value) {
    std::vector<std::string> parts;
    std::istringstream stream{std::string(value)};
    std::string part;
    while (stream >> part) {
        parts.push_back(part);
    }
    return parts;
}

ContentEditableState contentEditableState(const Node& node) {
    const auto it = node.attributes.find("contenteditable");
    if (it == node.attributes.end()) {
        return ContentEditableState::Inherit;
    }
    const std::string value = lowerAsciiText(trim(it->second));
    if (value.empty() || value == "true") {
        return ContentEditableState::True;
    }
    if (value == "false") {
        return ContentEditableState::False;
    }
    if (value == "plaintext-only") {
        return ContentEditableState::PlaintextOnly;
    }
    return ContentEditableState::Inherit;
}

bool isContentEditable(const Node& node) {
    for (const Node* current = &node; current; current = current->parent) {
        switch (contentEditableState(*current)) {
        case ContentEditableState::True:
        case ContentEditableState::PlaintextOnly:
            return true;
        case ContentEditableState::False:
            return false;
        case ContentEditableState::Inherit:
            break;
        }
    }
    return false;
}

bool isContentEditableEditingHost(const Node& node) {
    return isContentEditable(node) &&
           (!node.parent || !isContentEditable(*node.parent));
}

bool usesInlineContentEditableFlow(const Node& node) {
    if (!isContentEditableEditingHost(node)) {
        return false;
    }
    const auto flow = node.attributes.find("contenteditable-flow");
    return flow != node.attributes.end() && trim(flow->second) == "inline";
}

bool isInlineFlowElement(std::string_view tag) {
    return tag == "a" || tag == "abbr" || tag == "b" || tag == "code" ||
           tag == "del" || tag == "em" || tag == "i" || tag == "img" ||
           tag == "ins" || tag == "label" || tag == "mark" || tag == "q" ||
           tag == "s" || tag == "selectable" || tag == "small" ||
           tag == "span" || tag == "strong" || tag == "sub" || tag == "sup" ||
           tag == "svg" || tag == "text" || tag == "time" || tag == "u";
}

bool usesInlineFlow(const Node& node) {
    bool hasVisibleChild = false;
    bool hasTextChild = false;
    for (const auto& child : node.children) {
        if (child->style.display == Display::None) {
            continue;
        }
        if (!isInlineFlowElement(child->tag)) {
            return false;
        }
        hasVisibleChild = true;
        hasTextChild = hasTextChild || child->tag == "text";
    }
    return hasVisibleChild && hasTextChild;
}

bool isContentEditableTextNode(const Node& node) {
    if (!isContentEditable(node) || !isContentEditableTextTag(node.tag)) {
        return false;
    }
    return !hasContentEditableElementChildren(node);
}

bool isTextEditingNode(const Node& node) {
    return node.tag == "input" ||
           node.tag == "textarea" ||
           isContentEditableTextNode(node);
}

bool isSelectNode(const Node& node) {
    return node.tag == "select";
}

bool isOptionNode(const Node& node) {
    return node.tag == "option";
}

Node* owningSelect(Node* node) {
    return const_cast<Node*>(owningSelect(static_cast<const Node*>(node)));
}

const Node* owningSelect(const Node* node) {
    for (const Node* current = node; current; current = current->parent) {
        if (isSelectNode(*current)) {
            return current;
        }
    }
    return nullptr;
}

namespace {

template <typename NodeType>
void collectSelectOptions(NodeType& node,
                          std::vector<NodeType*>& options,
                          bool isRoot) {
    for (auto& childOwner : node.children) {
        NodeType& child = *childOwner;
        if (isOptionNode(child)) {
            options.push_back(&child);
        } else if (!isSelectNode(child) || isRoot) {
            collectSelectOptions(child, options, false);
        }
    }
}

template <typename NodeType>
std::vector<NodeType*> selectOptionsImpl(NodeType& select) {
    std::vector<NodeType*> options;
    if (!isSelectNode(select)) {
        return options;
    }
    collectSelectOptions(select, options, true);
    return options;
}

std::optional<size_t> lastOptionWithSelectedAttribute(
    const std::vector<Node*>& options) {
    std::optional<size_t> selected;
    for (size_t index = 0; index < options.size(); ++index) {
        if (options[index]->attributes.contains("selected")) {
            selected = index;
        }
    }
    return selected;
}

std::optional<size_t> firstEnabledOption(
    const std::vector<Node*>& options) {
    for (size_t index = 0; index < options.size(); ++index) {
        if (!isOptionDisabled(*options[index])) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<size_t> optionIndexWithValue(const std::vector<Node*>& options,
                                           std::string_view value) {
    for (size_t index = 0; index < options.size(); ++index) {
        if (optionValue(*options[index]) == value) {
            return index;
        }
    }
    return std::nullopt;
}

void appendOptionText(const Node& node, std::string& result) {
    result += node.text;
    for (const auto& child : node.children) {
        appendOptionText(*child, result);
    }
}

std::string optionText(const Node& option) {
    std::string result;
    appendOptionText(option, result);
    return result;
}

}  // namespace

std::vector<Node*> selectOptions(Node& select) {
    return selectOptionsImpl(select);
}

std::vector<const Node*> selectOptions(const Node& select) {
    return selectOptionsImpl(select);
}

std::string optionLabel(const Node& option) {
    const auto label = option.attributes.find("label");
    return label == option.attributes.end() ? optionText(option) : label->second;
}

std::string optionValue(const Node& option) {
    const auto value = option.attributes.find("value");
    return value == option.attributes.end() ? optionText(option) : value->second;
}

bool isOptionDisabled(const Node& option) {
    if (option.attributes.contains("disabled")) {
        return true;
    }
    for (const Node* current = option.parent;
         current && !isSelectNode(*current);
         current = current->parent) {
        if (current->tag == "optgroup" &&
            current->attributes.contains("disabled")) {
            return true;
        }
    }
    return false;
}

bool selectOptionAt(Node& select, size_t optionIndex) {
    std::vector<Node*> options = selectOptions(select);
    if (optionIndex >= options.size()) {
        return false;
    }

    const std::string nextValue = optionValue(*options[optionIndex]);
    bool changed = select.selectedOptionIndex != optionIndex ||
                   select.value != nextValue;
    for (size_t index = 0; index < options.size(); ++index) {
        if (index == optionIndex) {
            changed = options[index]->attributes.emplace("selected", "").second ||
                      changed;
        } else {
            changed = options[index]->attributes.erase("selected") > 0 ||
                      changed;
        }
    }
    select.selectedOptionIndex = optionIndex;
    select.highlightedOptionIndex = optionIndex;
    select.value = nextValue;
    return changed;
}

bool initializeSelectState(Node& select) {
    if (!isSelectNode(select)) {
        return false;
    }
    std::vector<Node*> options = selectOptions(select);
    if (options.empty()) {
        const bool changed = select.selectedOptionIndex.has_value() ||
                             select.highlightedOptionIndex.has_value() ||
                             !select.value.empty();
        select.selectedOptionIndex.reset();
        select.highlightedOptionIndex.reset();
        select.selectPopupFirstOption = 0;
        select.value.clear();
        return changed;
    }

    const std::optional<size_t> selected =
        lastOptionWithSelectedAttribute(options).or_else(
            [&options] { return firstEnabledOption(options); });
    if (!selected) {
        const bool changed = select.selectedOptionIndex.has_value() ||
                             select.highlightedOptionIndex.has_value() ||
                             !select.value.empty();
        select.selectedOptionIndex.reset();
        select.highlightedOptionIndex.reset();
        select.value.clear();
        return changed;
    }
    return selectOptionAt(select, *selected);
}

void initializeSelectStates(Node& node) {
    if (isSelectNode(node)) {
        initializeSelectState(node);
    }
    for (auto& child : node.children) {
        initializeSelectStates(*child);
    }
}

bool setSelectValue(Node& select, std::string_view value) {
    std::vector<Node*> options = selectOptions(select);
    const std::optional<size_t> optionIndex = optionIndexWithValue(options, value);
    if (optionIndex) {
        return selectOptionAt(select, *optionIndex);
    }

    bool changed = select.selectedOptionIndex.has_value() ||
                   select.highlightedOptionIndex.has_value() ||
                   !select.value.empty();
    for (Node* option : options) {
        changed = option->attributes.erase("selected") > 0 || changed;
    }
    select.selectedOptionIndex.reset();
    select.highlightedOptionIndex.reset();
    select.value.clear();
    return changed;
}

void synchronizeSelectStatesAfterMutation(Node& node) {
    if (isSelectNode(node)) {
        std::vector<Node*> options = selectOptions(node);
        if (options.empty()) {
            initializeSelectState(node);
        } else if (const std::optional<size_t> selected =
                       lastOptionWithSelectedAttribute(options)) {
            selectOptionAt(node, *selected);
        } else if (const std::optional<size_t> matchingValue =
                       optionIndexWithValue(options, node.value)) {
            selectOptionAt(node, *matchingValue);
        } else {
            initializeSelectState(node);
        }
    }
    for (auto& child : node.children) {
        synchronizeSelectStatesAfterMutation(*child);
    }
}

Rect selectVisualRect(const Node& select) {
    Rect rect = select.layout;
    for (const Node* current = &select; current; current = current->parent) {
        rect.y += stickyVisualOffsetY(*current);
        if (current->parent) {
            rect.x -= current->parent->scrollX;
            rect.y -= current->parent->scrollY;
        }
    }
    return rect;
}

SelectPopupGeometry selectPopupGeometry(const Node& select,
                                        float viewportWidth,
                                        float viewportHeight) {
    SelectPopupGeometry geometry;
    const std::vector<const Node*> options = selectOptions(select);
    if (options.empty()) {
        return geometry;
    }

    constexpr size_t kMaxVisibleOptions = 8;
    constexpr float kPopupBorderWidth = 1.0f;
    geometry.optionHeight = std::max(
        28.0f,
        select.style.fontSize * select.style.lineHeight + 12.0f);

    const Rect anchor = selectVisualRect(select);
    const float desiredHeight =
        static_cast<float>(std::min(options.size(), kMaxVisibleOptions)) *
            geometry.optionHeight +
        kPopupBorderWidth * 2.0f;
    const float spaceBelow = std::max(
        0.0f,
        viewportHeight - (anchor.y + anchor.h));
    const float spaceAbove = std::max(0.0f, anchor.y);
    const bool opensBelow = spaceBelow >= desiredHeight ||
                            spaceBelow >= spaceAbove;
    const float availableHeight = opensBelow ? spaceBelow : spaceAbove;
    const size_t rowsThatFit = std::max<size_t>(
        1,
        static_cast<size_t>(std::floor(
            std::max(0.0f, availableHeight - kPopupBorderWidth * 2.0f) /
            geometry.optionHeight)));
    geometry.visibleOptionCount = std::min(
        options.size(),
        std::min(kMaxVisibleOptions, rowsThatFit));

    const size_t maxFirstOption = options.size() - geometry.visibleOptionCount;
    geometry.firstOption = std::min(select.selectPopupFirstOption,
                                    maxFirstOption);
    if (select.highlightedOptionIndex) {
        if (*select.highlightedOptionIndex < geometry.firstOption) {
            geometry.firstOption = *select.highlightedOptionIndex;
        } else if (*select.highlightedOptionIndex >=
                   geometry.firstOption + geometry.visibleOptionCount) {
            geometry.firstOption =
                *select.highlightedOptionIndex -
                geometry.visibleOptionCount + 1;
        }
    }

    const float popupHeight =
        static_cast<float>(geometry.visibleOptionCount) *
            geometry.optionHeight +
        kPopupBorderWidth * 2.0f;
    const float popupWidth = std::min(
        std::max(40.0f, anchor.w),
        std::max(40.0f, viewportWidth));
    const float popupX = clampf(
        anchor.x,
        0.0f,
        std::max(0.0f, viewportWidth - popupWidth));
    const float preferredY = opensBelow
        ? anchor.y + anchor.h
        : anchor.y - popupHeight;
    const float popupY = clampf(
        preferredY,
        0.0f,
        std::max(0.0f, viewportHeight - popupHeight));
    geometry.rect = {popupX, popupY, popupWidth, popupHeight};
    return geometry;
}

std::optional<size_t> selectPopupOptionAtPoint(const Node& select,
                                               float x,
                                               float y,
                                               float viewportWidth,
                                               float viewportHeight) {
    const SelectPopupGeometry geometry = selectPopupGeometry(
        select,
        viewportWidth,
        viewportHeight);
    if (!geometry.rect.contains(x, y) ||
        geometry.visibleOptionCount == 0 ||
        geometry.optionHeight <= 0.0f) {
        return std::nullopt;
    }
    constexpr float kPopupBorderWidth = 1.0f;
    const float optionY = y - geometry.rect.y - kPopupBorderWidth;
    if (optionY < 0.0f) {
        return std::nullopt;
    }
    const size_t row = static_cast<size_t>(optionY / geometry.optionHeight);
    if (row >= geometry.visibleOptionCount) {
        return std::nullopt;
    }
    return geometry.firstOption + row;
}

Node* contentEditableEditingHost(Node* node) {
    return const_cast<Node*>(contentEditableEditingHost(
        static_cast<const Node*>(node)));
}

const Node* contentEditableEditingHost(const Node* node) {
    if (!node || !isContentEditable(*node)) {
        return nullptr;
    }
    const Node* current = node;
    while (current->parent && isContentEditable(*current->parent)) {
        current = current->parent;
    }
    return current;
}

void prepareContentEditableTree(Node& node) {
    for (auto& child : node.children) {
        prepareContentEditableTree(*child);
    }
    if (!isContentEditableTextNode(node) || node.text.empty()) {
        return;
    }
    if (node.value.empty()) {
        node.value = std::move(node.text);
        ++node.textRevision;
    } else {
        node.text.clear();
    }
}

void syncContentEditablePlaceholder(Node& node) {
    if (!isContentEditableTextNode(node)) {
        return;
    }
    if (node.value.empty()) {
        if (node.children.empty()) {
            auto lineBreak = std::make_unique<Node>();
            lineBreak->tag = "br";
            lineBreak->parent = &node;
            node.children.push_back(std::move(lineBreak));
        }
        return;
    }
    node.children.erase(
        std::remove_if(
            node.children.begin(),
            node.children.end(),
            [](const std::unique_ptr<Node>& child) {
                return child->tag == "br";
            }),
        node.children.end());
}

std::string textContent(const Node& node) {
    std::string result;
    appendTextContent(node, result);
    return result;
}

std::string editableTextContent(const Node& node) {
    std::string result;
    appendEditableTextContent(node, node, result);
    return result;
}

float scrollViewportWidth(const Node& node) {
    float width = node.layout.w;
    if (node.style.scrollbarGutterStable &&
        (node.style.overflowY == Overflow::Auto || node.style.overflowY == Overflow::Scroll)) {
        width -= kSkuiScrollbarGutter;
    }
    return std::max(0.0f, width);
}

float scrollViewportHeight(const Node& node) {
    float height = node.layout.h;
    const bool needsHorizontalGutter = node.style.overflowX == Overflow::Scroll ||
        (node.style.overflowX == Overflow::Auto && node.scrollContentWidth > scrollViewportWidth(node));
    if (node.style.scrollbarGutterStable && needsHorizontalGutter) {
        height -= kSkuiScrollbarGutter;
    }
    return std::max(0.0f, height);
}

float scrollMaxX(const Node& node) {
    return std::max(0.0f, node.scrollContentWidth - scrollViewportWidth(node));
}

float scrollMaxY(const Node& node) {
    return std::max(0.0f, node.scrollContentHeight - scrollViewportHeight(node));
}

bool shouldShowScrollbarX(const Node& node) {
    return node.style.overflowX == Overflow::Scroll ||
           ((node.style.overflowX == Overflow::Auto || isTextareaScrollableNode(node)) &&
            scrollMaxX(node) > 0.0f);
}

bool shouldShowScrollbarY(const Node& node) {
    return node.style.overflowY == Overflow::Scroll ||
           ((node.style.overflowY == Overflow::Auto || isTextareaScrollableNode(node)) &&
            scrollMaxY(node) > 0.0f);
}

Rect scrollContentClipRect(const Node& node) {
    Rect clip = node.layout;
    if (node.style.scrollbarGutterStable) {
        if (node.style.overflowY == Overflow::Scroll ||
            node.style.overflowY == Overflow::Auto) {
            clip.w = std::max(0.0f, clip.w - kSkuiScrollbarGutter);
        }
        if (shouldShowScrollbarX(node)) {
            clip.h = std::max(0.0f, clip.h - kSkuiScrollbarGutter);
        }
    }
    return clip;
}

float stickyVisualOffsetY(const Node& node) {
    if (node.style.position != Position::Sticky) {
        return 0.0f;
    }

    for (const Node* ancestor = node.parent; ancestor; ancestor = ancestor->parent) {
        const bool scrollsY = ancestor->style.overflowY == Overflow::Auto ||
            ancestor->style.overflowY == Overflow::Scroll;
        if (scrollsY && ancestor->scrollY > 0.0f) {
            return ancestor->scrollY;
        }
    }
    return 0.0f;
}

bool requiresZIndexOrdering(const Node& node) {
    return std::any_of(node.children.begin(), node.children.end(), [](const auto& child) {
        return child->style.zIndex != 0;
    });
}

std::vector<const Node*> childrenInPaintOrder(const Node& node) {
    std::vector<const Node*> children;
    children.reserve(node.children.size());
    for (const auto& child : node.children) {
        children.push_back(child.get());
    }
    sortChildrenByZIndex(children);
    return children;
}

std::vector<Node*> childrenInPaintOrder(Node& node) {
    std::vector<Node*> children;
    children.reserve(node.children.size());
    for (auto& child : node.children) {
        children.push_back(child.get());
    }
    sortChildrenByZIndex(children);
    return children;
}

std::filesystem::path pathFromUtf8(std::string_view text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::filesystem::path(std::string(text));
    }

    const int sourceSize = static_cast<int>(text.size());
    const int wideSize = MultiByteToWideChar(CP_UTF8,
                                             MB_ERR_INVALID_CHARS,
                                             text.data(),
                                             sourceSize,
                                             nullptr,
                                             0);
    if (wideSize <= 0) {
        return std::filesystem::path(std::string(text));
    }

    std::wstring wide(static_cast<size_t>(wideSize), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), sourceSize, wide.data(), wideSize);
    return std::filesystem::path(wide);
#else
#ifdef __cpp_char8_t
    std::u8string value;
    value.reserve(text.size());
    for (char ch : text) {
        value.push_back(static_cast<char8_t>(ch));
    }
    return std::filesystem::path(value);
#else
    return std::filesystem::path(std::string(text));
#endif
#endif
}

std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return path.string();
    }

    const int sourceSize = static_cast<int>(wide.size());
    const int utf8Size = WideCharToMultiByte(CP_UTF8,
                                             0,
                                             wide.data(),
                                             sourceSize,
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (utf8Size <= 0) {
        return path.string();
    }

    std::string utf8(static_cast<size_t>(utf8Size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        sourceSize,
                        utf8.data(),
                        utf8Size,
                        nullptr,
                        nullptr);
    return utf8;
#else
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
#endif
}

static bool parseHexByte(std::string_view text, unsigned& out) {
    unsigned value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

SkColor parseColor(std::string_view raw, SkColor fallback) {
    std::string value = trim(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (value.empty()) {
        return fallback;
    }
    if (value == "transparent") {
        return SK_ColorTRANSPARENT;
    }
    if (value == "white") {
        return SK_ColorWHITE;
    }
    if (value == "black") {
        return SK_ColorBLACK;
    }
    if (value.size() == 7 && value[0] == '#') {
        unsigned r = 0;
        unsigned g = 0;
        unsigned b = 0;
        if (parseHexByte(std::string_view(value).substr(1, 2), r) &&
            parseHexByte(std::string_view(value).substr(3, 2), g) &&
            parseHexByte(std::string_view(value).substr(5, 2), b)) {
            return rgb(r, g, b);
        }
    }
    if (value.rfind("rgba(", 0) == 0 && value.back() == ')') {
        std::string args = value.substr(5, value.size() - 6);
        std::replace(args.begin(), args.end(), ',', ' ');
        std::istringstream stream(args);
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
        if (stream >> r >> g >> b >> a) {
            return rgba(static_cast<unsigned>(clampf(r, 0.0f, 255.0f)),
                        static_cast<unsigned>(clampf(g, 0.0f, 255.0f)),
                        static_cast<unsigned>(clampf(b, 0.0f, 255.0f)),
                        static_cast<unsigned>(clampf(a <= 1.0f ? a * 255.0f : a, 0.0f, 255.0f)));
        }
    }
    if (value.rfind("rgb(", 0) == 0 && value.back() == ')') {
        std::string args = value.substr(4, value.size() - 5);
        std::replace(args.begin(), args.end(), ',', ' ');
        std::istringstream stream(args);
        unsigned r = 0;
        unsigned g = 0;
        unsigned b = 0;
        if (stream >> r >> g >> b) {
            return rgb(r, g, b);
        }
    }
    return fallback;
}

YogaNode::YogaNode() : node_(YGNodeNew()) {}

YogaNode::~YogaNode() {
    if (node_) {
        YGNodeFreeRecursive(node_);
    }
}

YGNodeRef YogaNode::get() const {
    return node_;
}

}  // namespace skui
