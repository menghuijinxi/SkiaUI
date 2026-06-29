#include "skui_internal.h"

#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skui {
namespace {

void rebindParents(Node& node, Node* parent) {
    node.parent = parent;
    for (auto& child : node.children) {
        rebindParents(*child, &node);
    }
}

bool isRenderableNode(const Node& node) {
    return node.style.display != Display::None && node.layout.w > 0.0f && node.layout.h > 0.0f;
}

Node* hitTest(Node& node, float x, float y) {
    if (!isRenderableNode(node) || !node.layout.contains(x, y)) {
        return nullptr;
    }
    const float childX = x + node.scrollX;
    const float childY = y + node.scrollY;
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (Node* hit = hitTest(**it, childX, childY)) {
            return hit;
        }
    }
    return &node;
}

void collectChain(Node* node, std::vector<Node*>& out) {
    out.clear();
    for (Node* current = node; current; current = current->parent) {
        out.push_back(current);
    }
}

bool containsNode(const std::vector<Node*>& nodes, const Node& node) {
    return std::find(nodes.begin(), nodes.end(), &node) != nodes.end();
}

bool hasClass(const Node& node, std::string_view className) {
    return std::find(node.classes.begin(), node.classes.end(), className) != node.classes.end();
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string classAttributeValue(const std::vector<std::string>& classes) {
    std::string value;
    for (const std::string& className : classes) {
        if (!value.empty()) {
            value.push_back(' ');
        }
        value += className;
    }
    return value;
}

Node* findById(Node& node, std::string_view id) {
    if (node.id == id) {
        return &node;
    }
    for (auto& child : node.children) {
        if (Node* found = findById(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

const Node* findById(const Node& node, std::string_view id) {
    if (node.id == id) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const Node* found = findById(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

bool updateStateTree(Node& node, const std::vector<Node*>& hovered, const std::vector<Node*>& active) {
    bool changed = false;
    const bool nextHovered = containsNode(hovered, node);
    const bool nextActive = containsNode(active, node);
    if (node.hovered != nextHovered) {
        node.hovered = nextHovered;
        changed = true;
    }
    if (node.active != nextActive) {
        node.active = nextActive;
        changed = true;
    }
    for (auto& child : node.children) {
        changed = updateStateTree(*child, hovered, active) || changed;
    }
    return changed;
}

bool isPointerEvent(EventType type) {
    return type == EventType::MouseMove ||
           type == EventType::MouseLeave ||
           type == EventType::MouseDown ||
           type == EventType::MouseUp ||
           type == EventType::MouseDoubleClick ||
           type == EventType::MouseWheel;
}

bool isEditableNode(const Node* node) {
    return node && (node->tag == "input" || node->tag == "textarea");
}

bool isSelectableTextNode(const Node* node) {
    return node && node->tag == "selectable";
}

bool isTextareaNode(const Node* node) {
    return node && node->tag == "textarea";
}

Node* inputTarget(Node* leaf) {
    for (Node* current = leaf; current; current = current->parent) {
        if (isEditableNode(current)) {
            return current;
        }
    }
    return nullptr;
}

Node* selectableTextTarget(Node* leaf) {
    for (Node* current = leaf; current; current = current->parent) {
        if (isSelectableTextNode(current)) {
            return current;
        }
    }
    return nullptr;
}

Node* actionTarget(Node* leaf) {
    for (Node* current = leaf; current; current = current->parent) {
        if (!current->action.empty()) {
            return current;
        }
    }
    return nullptr;
}

float maxScrollX(const Node& node) {
    return std::max(0.0f, node.scrollContentWidth - node.layout.w);
}

float maxScrollY(const Node& node) {
    return std::max(0.0f, node.scrollContentHeight - node.layout.h);
}

bool canScrollX(const Node& node) {
    return (node.style.overflowX == Overflow::Auto || node.style.overflowX == Overflow::Scroll) && maxScrollX(node) > 0.0f;
}

bool canScrollY(const Node& node) {
    return (node.style.overflowY == Overflow::Auto || node.style.overflowY == Overflow::Scroll) && maxScrollY(node) > 0.0f;
}

enum class ScrollbarAxis {
    None,
    Horizontal,
    Vertical
};

struct ScrollbarGeometry {
    float trackStart = 0.0f;
    float trackCross = 0.0f;
    float trackLength = 0.0f;
    float thumbStart = 0.0f;
    float thumbLength = 0.0f;
    float maxScroll = 0.0f;
};

struct ScrollbarHit {
    Node* node = nullptr;
    ScrollbarAxis axis = ScrollbarAxis::None;
    float dragOffset = 0.0f;
};

constexpr float kScrollbarThickness = 6.0f;
constexpr float kScrollbarInset = 4.0f;
constexpr float kScrollbarMinThumb = 24.0f;

bool wantsScrollbarX(const Node& node) {
    return node.style.overflowX == Overflow::Scroll ||
           (node.style.overflowX == Overflow::Auto && maxScrollX(node) > 0.0f);
}

bool wantsScrollbarY(const Node& node) {
    return node.style.overflowY == Overflow::Scroll ||
           (node.style.overflowY == Overflow::Auto && maxScrollY(node) > 0.0f);
}

std::optional<ScrollbarGeometry> scrollbarGeometry(const Node& node, ScrollbarAxis axis) {
    if (node.layout.w <= 0.0f || node.layout.h <= 0.0f) {
        return std::nullopt;
    }

    const bool showX = wantsScrollbarX(node);
    const bool showY = wantsScrollbarY(node);
    if ((axis == ScrollbarAxis::Horizontal && !showX) || (axis == ScrollbarAxis::Vertical && !showY)) {
        return std::nullopt;
    }

    ScrollbarGeometry geometry;
    if (axis == ScrollbarAxis::Vertical) {
        geometry.maxScroll = maxScrollY(node);
        geometry.trackStart = node.layout.y + kScrollbarInset;
        geometry.trackCross = node.layout.x + node.layout.w - kScrollbarInset - kScrollbarThickness;
        geometry.trackLength = node.layout.h - kScrollbarInset * 2.0f - (showX ? kScrollbarThickness + kScrollbarInset : 0.0f);
        const float ratio = node.scrollContentHeight <= 0.0f ? 1.0f : node.layout.h / node.scrollContentHeight;
        geometry.thumbLength = clampf(geometry.trackLength * ratio,
                                      std::min(kScrollbarMinThumb, geometry.trackLength),
                                      geometry.trackLength);
        const float travel = std::max(0.0f, geometry.trackLength - geometry.thumbLength);
        geometry.thumbStart = geometry.trackStart + (geometry.maxScroll <= 0.0f ? 0.0f : node.scrollY / geometry.maxScroll * travel);
    } else {
        geometry.maxScroll = maxScrollX(node);
        geometry.trackStart = node.layout.x + kScrollbarInset;
        geometry.trackCross = node.layout.y + node.layout.h - kScrollbarInset - kScrollbarThickness;
        geometry.trackLength = node.layout.w - kScrollbarInset * 2.0f - (showY ? kScrollbarThickness + kScrollbarInset : 0.0f);
        const float ratio = node.scrollContentWidth <= 0.0f ? 1.0f : node.layout.w / node.scrollContentWidth;
        geometry.thumbLength = clampf(geometry.trackLength * ratio,
                                      std::min(kScrollbarMinThumb, geometry.trackLength),
                                      geometry.trackLength);
        const float travel = std::max(0.0f, geometry.trackLength - geometry.thumbLength);
        geometry.thumbStart = geometry.trackStart + (geometry.maxScroll <= 0.0f ? 0.0f : node.scrollX / geometry.maxScroll * travel);
    }

    if (geometry.trackLength <= 0.0f || geometry.thumbLength <= 0.0f) {
        return std::nullopt;
    }
    return geometry;
}

std::optional<ScrollbarHit> scrollbarHitSelf(Node& node, float x, float y) {
    if (std::optional<ScrollbarGeometry> vertical = scrollbarGeometry(node, ScrollbarAxis::Vertical)) {
        const bool inTrack = x >= vertical->trackCross &&
                             x <= vertical->trackCross + kScrollbarThickness &&
                             y >= vertical->trackStart &&
                             y <= vertical->trackStart + vertical->trackLength;
        if (inTrack) {
            const bool inThumb = y >= vertical->thumbStart && y <= vertical->thumbStart + vertical->thumbLength;
            return ScrollbarHit{&node, ScrollbarAxis::Vertical, inThumb ? y - vertical->thumbStart : vertical->thumbLength * 0.5f};
        }
    }

    if (std::optional<ScrollbarGeometry> horizontal = scrollbarGeometry(node, ScrollbarAxis::Horizontal)) {
        const bool inTrack = x >= horizontal->trackStart &&
                             x <= horizontal->trackStart + horizontal->trackLength &&
                             y >= horizontal->trackCross &&
                             y <= horizontal->trackCross + kScrollbarThickness;
        if (inTrack) {
            const bool inThumb = x >= horizontal->thumbStart && x <= horizontal->thumbStart + horizontal->thumbLength;
            return ScrollbarHit{&node, ScrollbarAxis::Horizontal, inThumb ? x - horizontal->thumbStart : horizontal->thumbLength * 0.5f};
        }
    }

    return std::nullopt;
}

std::optional<ScrollbarHit> scrollbarHitTest(Node& node, float x, float y) {
    if (!isRenderableNode(node) || !node.layout.contains(x, y)) {
        return std::nullopt;
    }
    if (std::optional<ScrollbarHit> hit = scrollbarHitSelf(node, x, y)) {
        return hit;
    }

    const float childX = x + node.scrollX;
    const float childY = y + node.scrollY;
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (std::optional<ScrollbarHit> hit = scrollbarHitTest(**it, childX, childY)) {
            return hit;
        }
    }
    return std::nullopt;
}

bool updateScrollFromScrollbar(Node& node, ScrollbarAxis axis, float pointer, float dragOffset) {
    std::optional<ScrollbarGeometry> geometry = scrollbarGeometry(node, axis);
    if (!geometry || geometry->maxScroll <= 0.0f) {
        return false;
    }

    const float travel = std::max(0.0f, geometry->trackLength - geometry->thumbLength);
    if (travel <= 0.0f) {
        return false;
    }

    const float thumbStart = clampf(pointer - dragOffset,
                                    geometry->trackStart,
                                    geometry->trackStart + travel);
    const float next = (thumbStart - geometry->trackStart) / travel * geometry->maxScroll;
    if (axis == ScrollbarAxis::Vertical) {
        const float clamped = clampf(next, 0.0f, geometry->maxScroll);
        const bool changed = clamped != node.scrollY;
        node.scrollY = clamped;
        return changed;
    }

    const float clamped = clampf(next, 0.0f, geometry->maxScroll);
    const bool changed = clamped != node.scrollX;
    node.scrollX = clamped;
    return changed;
}

bool scrollNode(Node& node, float dx, float dy) {
    bool changed = false;
    if (dx != 0.0f && canScrollX(node)) {
        const float next = clampf(node.scrollX + dx, 0.0f, maxScrollX(node));
        changed = next != node.scrollX || changed;
        node.scrollX = next;
    }
    if (dy != 0.0f && canScrollY(node)) {
        const float next = clampf(node.scrollY + dy, 0.0f, maxScrollY(node));
        changed = next != node.scrollY || changed;
        node.scrollY = next;
    }
    return changed;
}

bool scrollNearest(Node* leaf, float dx, float dy) {
    for (Node* current = leaf; current; current = current->parent) {
        if (scrollNode(*current, dx, dy)) {
            return true;
        }
    }
    return false;
}

ElementEvent makeElementEvent(ElementEventType type, const Node& node, const Event& source, float x, float y) {
    ElementEvent event;
    event.type = type;
    event.tag = node.tag;
    event.id = node.id;
    event.classes = node.classes;
    event.action = node.action;
    event.text = node.text;
    event.value = node.value;
    event.x = x;
    event.y = y;
    event.button = source.button;
    return event;
}

bool isUtf8Continuation(unsigned char ch) {
    return (ch & 0xC0) == 0x80;
}

size_t clampUtf8Index(const std::string& value, size_t index) {
    index = std::min(index, value.size());
    while (index > 0 && index < value.size() && isUtf8Continuation(static_cast<unsigned char>(value[index]))) {
        --index;
    }
    return index;
}

size_t previousUtf8Index(const std::string& value, size_t index) {
    index = clampUtf8Index(value, index);
    if (index == 0) {
        return 0;
    }
    --index;
    while (index > 0 && isUtf8Continuation(static_cast<unsigned char>(value[index]))) {
        --index;
    }
    return index;
}

size_t nextUtf8Index(const std::string& value, size_t index) {
    index = clampUtf8Index(value, index);
    if (index >= value.size()) {
        return value.size();
    }
    ++index;
    while (index < value.size() && isUtf8Continuation(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

struct TextLine {
    size_t start = 0;
    size_t end = 0;
};

std::vector<TextLine> editableLines(std::string_view value) {
    std::vector<TextLine> lines;
    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n') {
            lines.push_back({start, i});
            start = i + 1;
        }
    }
    lines.push_back({start, value.size()});
    return lines;
}

float editableLineHeight(const Node& node) {
    return std::max(12.0f, node.style.fontSize * 1.38f);
}

size_t currentLineStart(const std::string& value, size_t cursor) {
    cursor = clampUtf8Index(value, cursor);
    if (cursor == 0) {
        return 0;
    }
    const size_t newline = value.rfind('\n', cursor - 1);
    return newline == std::string::npos ? 0 : newline + 1;
}

size_t currentLineEnd(const std::string& value, size_t cursor) {
    cursor = clampUtf8Index(value, cursor);
    const size_t newline = value.find('\n', cursor);
    return newline == std::string::npos ? value.size() : newline;
}

void clampInputCursor(Node& node) {
    node.cursorIndex = clampUtf8Index(node.value, node.cursorIndex);
    node.selectionAnchor = clampUtf8Index(node.value, node.selectionAnchor);
    node.selectionStart = clampUtf8Index(node.value, node.selectionStart);
    node.selectionEnd = clampUtf8Index(node.value, node.selectionEnd);
    if (node.selectionStart > node.selectionEnd) {
        std::swap(node.selectionStart, node.selectionEnd);
    }
}

bool hasInputSelection(const Node& node) {
    return node.selectionStart != node.selectionEnd;
}

void pushInputUndo(Node& node) {
    if (!isEditableNode(&node)) {
        return;
    }
    constexpr size_t kMaxUndoSteps = 128;
    clampInputCursor(node);
    Node::InputSnapshot snapshot;
    snapshot.value = node.value;
    snapshot.cursorIndex = node.cursorIndex;
    snapshot.selectionAnchor = node.selectionAnchor;
    snapshot.selectionStart = node.selectionStart;
    snapshot.selectionEnd = node.selectionEnd;
    node.undoStack.push_back(std::move(snapshot));
    if (node.undoStack.size() > kMaxUndoSteps) {
        node.undoStack.erase(node.undoStack.begin());
    }
}

bool restoreInputUndo(Node& node) {
    if (!isEditableNode(&node) || node.undoStack.empty()) {
        return false;
    }
    Node::InputSnapshot snapshot = std::move(node.undoStack.back());
    node.undoStack.pop_back();
    node.value = std::move(snapshot.value);
    node.cursorIndex = snapshot.cursorIndex;
    node.selectionAnchor = snapshot.selectionAnchor;
    node.selectionStart = snapshot.selectionStart;
    node.selectionEnd = snapshot.selectionEnd;
    node.compositionText.clear();
    clampInputCursor(node);
    return true;
}

void clearInputSelection(Node& node) {
    clampInputCursor(node);
    node.selectionAnchor = node.cursorIndex;
    node.selectionStart = node.cursorIndex;
    node.selectionEnd = node.cursorIndex;
}

void setInputCursor(Node& node, size_t index) {
    node.cursorIndex = clampUtf8Index(node.value, index);
    clearInputSelection(node);
}

void setInputSelection(Node& node, size_t anchor, size_t cursor) {
    node.selectionAnchor = clampUtf8Index(node.value, anchor);
    node.cursorIndex = clampUtf8Index(node.value, cursor);
    node.selectionStart = std::min(node.selectionAnchor, node.cursorIndex);
    node.selectionEnd = std::max(node.selectionAnchor, node.cursorIndex);
}

bool eraseInputSelection(Node& node) {
    if (!isEditableNode(&node)) {
        return false;
    }
    clampInputCursor(node);
    if (!hasInputSelection(node)) {
        return false;
    }
    pushInputUndo(node);
    node.value.erase(node.selectionStart, node.selectionEnd - node.selectionStart);
    node.cursorIndex = node.selectionStart;
    node.compositionText.clear();
    clearInputSelection(node);
    return true;
}

bool insertInputText(Node& node, std::string_view text) {
    if (!isEditableNode(&node) || text.empty()) {
        return false;
    }
    clampInputCursor(node);
    pushInputUndo(node);
    if (hasInputSelection(node)) {
        node.value.erase(node.selectionStart, node.selectionEnd - node.selectionStart);
        node.cursorIndex = node.selectionStart;
        clearInputSelection(node);
    }
    node.compositionText.clear();
    node.value.insert(node.cursorIndex, text.data(), text.size());
    node.cursorIndex += text.size();
    clearInputSelection(node);
    return true;
}

bool erasePreviousInputChar(Node& node) {
    if (!isEditableNode(&node) || node.value.empty()) {
        return false;
    }
    clampInputCursor(node);
    if (eraseInputSelection(node)) {
        return true;
    }
    const size_t previous = previousUtf8Index(node.value, node.cursorIndex);
    if (previous == node.cursorIndex) {
        return false;
    }
    pushInputUndo(node);
    node.value.erase(previous, node.cursorIndex - previous);
    node.cursorIndex = previous;
    node.compositionText.clear();
    clearInputSelection(node);
    return true;
}

bool eraseNextInputChar(Node& node) {
    if (!isEditableNode(&node) || node.value.empty()) {
        return false;
    }
    clampInputCursor(node);
    if (eraseInputSelection(node)) {
        return true;
    }
    const size_t next = nextUtf8Index(node.value, node.cursorIndex);
    if (next == node.cursorIndex) {
        return false;
    }
    pushInputUndo(node);
    node.value.erase(node.cursorIndex, next - node.cursorIndex);
    node.compositionText.clear();
    clearInputSelection(node);
    return true;
}

std::string selectedInputText(const Node& node) {
    if (!isEditableNode(&node) || node.selectionStart >= node.selectionEnd || node.selectionEnd > node.value.size()) {
        return {};
    }
    return node.value.substr(node.selectionStart, node.selectionEnd - node.selectionStart);
}

const std::string& selectableTextValue(const Node& node) {
    return !node.value.empty() ? node.value : node.text;
}

bool hasSelectableSelection(const Node& node) {
    const std::string& value = selectableTextValue(node);
    return isSelectableTextNode(&node) &&
           node.selectionStart < node.selectionEnd &&
           node.selectionEnd <= value.size();
}

std::string selectedSelectableText(const Node& node) {
    if (!hasSelectableSelection(node)) {
        return {};
    }
    const std::string& value = selectableTextValue(node);
    return value.substr(node.selectionStart, node.selectionEnd - node.selectionStart);
}

bool selectAllInput(Node& node) {
    if (!isEditableNode(&node) || node.value.empty()) {
        return false;
    }
    node.selectionAnchor = 0;
    node.selectionStart = 0;
    node.selectionEnd = node.value.size();
    node.cursorIndex = node.value.size();
    return true;
}

bool clearSelectableSelection(Node& node) {
    if (!isSelectableTextNode(&node) ||
        (node.cursorIndex == 0 && node.selectionAnchor == 0 && node.selectionStart == 0 && node.selectionEnd == 0)) {
        return false;
    }
    node.cursorIndex = 0;
    node.selectionAnchor = 0;
    node.selectionStart = 0;
    node.selectionEnd = 0;
    return true;
}

void setSelectableCursor(Node& node, size_t index) {
    const std::string& value = selectableTextValue(node);
    node.cursorIndex = clampUtf8Index(value, index);
    node.selectionAnchor = node.cursorIndex;
    node.selectionStart = node.cursorIndex;
    node.selectionEnd = node.cursorIndex;
}

void setSelectableSelection(Node& node, size_t anchor, size_t cursor) {
    const std::string& value = selectableTextValue(node);
    node.selectionAnchor = clampUtf8Index(value, anchor);
    node.cursorIndex = clampUtf8Index(value, cursor);
    node.selectionStart = std::min(node.selectionAnchor, node.cursorIndex);
    node.selectionEnd = std::max(node.selectionAnchor, node.cursorIndex);
}

bool selectAllSelectable(Node& node) {
    if (!isSelectableTextNode(&node)) {
        return false;
    }
    const std::string& value = selectableTextValue(node);
    if (value.empty()) {
        return false;
    }
    node.selectionAnchor = 0;
    node.selectionStart = 0;
    node.selectionEnd = value.size();
    node.cursorIndex = value.size();
    return true;
}

bool isAsciiWordByte(unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_';
}

bool isAsciiSpaceByte(unsigned char ch) {
    return std::isspace(ch) != 0;
}

void selectInputWordAt(Node& node, size_t index) {
    if (!isEditableNode(&node) || node.value.empty()) {
        return;
    }
    index = clampUtf8Index(node.value, index);
    if (index == node.value.size() && index > 0) {
        index = previousUtf8Index(node.value, index);
    }
    const unsigned char ch = static_cast<unsigned char>(node.value[index]);
    size_t start = index;
    size_t end = nextUtf8Index(node.value, index);
    if (ch < 0x80 && isAsciiWordByte(ch)) {
        while (start > 0) {
            const size_t previous = previousUtf8Index(node.value, start);
            const unsigned char prev = static_cast<unsigned char>(node.value[previous]);
            if (prev >= 0x80 || !isAsciiWordByte(prev)) {
                break;
            }
            start = previous;
        }
        while (end < node.value.size()) {
            const unsigned char next = static_cast<unsigned char>(node.value[end]);
            if (next >= 0x80 || !isAsciiWordByte(next)) {
                break;
            }
            end = nextUtf8Index(node.value, end);
        }
    } else if (ch < 0x80 && isAsciiSpaceByte(ch)) {
        while (start > 0) {
            const size_t previous = previousUtf8Index(node.value, start);
            const unsigned char prev = static_cast<unsigned char>(node.value[previous]);
            if (prev >= 0x80 || !isAsciiSpaceByte(prev)) {
                break;
            }
            start = previous;
        }
        while (end < node.value.size()) {
            const unsigned char next = static_cast<unsigned char>(node.value[end]);
            if (next >= 0x80 || !isAsciiSpaceByte(next)) {
                break;
            }
            end = nextUtf8Index(node.value, end);
        }
    }
    setInputSelection(node, start, end);
}

void selectSelectableWordAt(Node& node, size_t index) {
    const std::string& value = selectableTextValue(node);
    if (!isSelectableTextNode(&node) || value.empty()) {
        return;
    }
    index = clampUtf8Index(value, index);
    if (index == value.size() && index > 0) {
        index = previousUtf8Index(value, index);
    }
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    size_t start = index;
    size_t end = nextUtf8Index(value, index);
    if (ch < 0x80 && isAsciiWordByte(ch)) {
        while (start > 0) {
            const size_t previous = previousUtf8Index(value, start);
            const unsigned char prev = static_cast<unsigned char>(value[previous]);
            if (prev >= 0x80 || !isAsciiWordByte(prev)) {
                break;
            }
            start = previous;
        }
        while (end < value.size()) {
            const unsigned char next = static_cast<unsigned char>(value[end]);
            if (next >= 0x80 || !isAsciiWordByte(next)) {
                break;
            }
            end = nextUtf8Index(value, end);
        }
    } else if (ch < 0x80 && isAsciiSpaceByte(ch)) {
        while (start > 0) {
            const size_t previous = previousUtf8Index(value, start);
            const unsigned char prev = static_cast<unsigned char>(value[previous]);
            if (prev >= 0x80 || !isAsciiSpaceByte(prev)) {
                break;
            }
            start = previous;
        }
        while (end < value.size()) {
            const unsigned char next = static_cast<unsigned char>(value[end]);
            if (next >= 0x80 || !isAsciiSpaceByte(next)) {
                break;
            }
            end = nextUtf8Index(value, end);
        }
    }
    setSelectableSelection(node, start, end);
}

std::string singleLineText(std::string text) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    return text;
}

std::string multilineText(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue;
            }
            out.push_back('\n');
        } else if (ch == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string editableText(Node& node, std::string text) {
    return isTextareaNode(&node) ? multilineText(std::move(text)) : singleLineText(std::move(text));
}

void syncNodeAttribute(Node& node, const std::string& name) {
    auto it = node.attributes.find(name);
    const bool hasValue = it != node.attributes.end();
    std::string value = hasValue ? it->second : std::string{};

    if (name == "id") {
        node.id = value;
    } else if (name == "class") {
        node.classes = splitWhitespace(value);
    } else if (name == "value") {
        value = editableText(node, std::move(value));
        node.value = value;
        node.numericValue = 0.0f;
        const char* begin = value.data();
        const char* end = value.data() + value.size();
        std::from_chars(begin, end, node.numericValue);
        node.cursorIndex = std::min(node.cursorIndex, node.value.size());
        node.selectionAnchor = std::min(node.selectionAnchor, node.value.size());
        node.selectionStart = std::min(node.selectionStart, node.value.size());
        node.selectionEnd = std::min(node.selectionEnd, node.value.size());
        node.undoStack.clear();
        clampInputCursor(node);
    } else if (name == "placeholder") {
        node.placeholder = value;
    } else if (name == "max") {
        node.numericMax = 1.0f;
        const char* begin = value.data();
        const char* end = value.data() + value.size();
        std::from_chars(begin, end, node.numericMax);
        node.numericMax = std::max(0.0001f, node.numericMax);
    } else if (name == "src") {
        node.src = value;
    } else if (name == "data-action") {
        node.action = value;
    } else if (name == "style") {
        node.inlineStyle = {};
        if (hasValue) {
            parseInlineStyle(value, node.inlineStyle);
        }
    }
}

}  // namespace

class Runtime::Impl {
public:
    explicit Impl(RuntimeOptions runtimeOptions)
        : options(std::move(runtimeOptions)), parser(options), renderer(options) {}

    float logicalWidth() const {
        return static_cast<float>(width) / std::max(0.1f, dpiScale);
    }

    float logicalHeight() const {
        return static_cast<float>(height) / std::max(0.1f, dpiScale);
    }

    void recomputeAndLayout() {
        const float viewportWidth = logicalWidth();
        const float viewportHeight = logicalHeight();
        recomputeStyles(document, options, viewportWidth, viewportHeight);
        layoutEngine.layout(document, viewportWidth, viewportHeight);
    }

    bool setFocusedNode(Node* next) {
        if (focusedNode == next) {
            return false;
        }
        if (focusedNode) {
            focusedNode->compositionText.clear();
            clearInputSelection(*focusedNode);
            focusedNode->focused = false;
        }
        focusedNode = next;
        if (focusedNode) {
            focusedNode->focused = true;
            focusedNode->cursorIndex = focusedNode->value.size();
            clampInputCursor(*focusedNode);
            clearInputSelection(*focusedNode);
        }
        return true;
    }

    static float visualX(const Node& node) {
        float x = node.layout.x;
        for (const Node* current = node.parent; current; current = current->parent) {
            x -= current->scrollX;
        }
        return x;
    }

    static float visualY(const Node& node) {
        float y = node.layout.y;
        for (const Node* current = node.parent; current; current = current->parent) {
            y -= current->scrollY;
        }
        return y;
    }

    size_t inputIndexAtX(const Node& input, float x) {
        const float textStart = renderer.textStartX(input, input.value) - input.layout.x + visualX(input);
        return renderer.textIndexAtOffset(input.value, input.style.fontSize, input.style.fontBold, x - textStart);
    }

    size_t selectableIndexAtX(const Node& node, float x) {
        const std::string& value = selectableTextValue(node);
        const float textStart = renderer.textStartX(node, value) - node.layout.x + visualX(node);
        return renderer.textIndexAtOffset(value, node.style.fontSize, node.style.fontBold, x - textStart);
    }

    size_t editableIndexAtPoint(const Node& input, float x, float y) {
        if (!isTextareaNode(&input)) {
            return inputIndexAtX(input, x);
        }
        const std::vector<TextLine> lines = editableLines(input.value);
        const float lineHeight = editableLineHeight(input);
        const float relativeY = std::max(0.0f, y - visualY(input));
        const size_t lineIndex = std::min(lines.size() - 1,
                                          static_cast<size_t>(relativeY / std::max(1.0f, lineHeight)));
        const TextLine line = lines[lineIndex];
        const std::string_view text(input.value.data() + line.start, line.end - line.start);
        const float offset = x - visualX(input);
        return line.start + renderer.textIndexAtOffset(text, input.style.fontSize, input.style.fontBold, offset);
    }

    RuntimeOptions options;
    DocumentParser parser;
    LayoutEngine layoutEngine;
    SkiaRenderer renderer;
    Document document;
    int width = 1;
    int height = 1;
    float dpiScale = 1.0f;
    bool hasDocument = false;
    bool dirty = true;
    bool mousePressed = false;
    MouseButton pressedButton = MouseButton::None;
    Node* hoveredLeaf = nullptr;
    Node* pressedLeaf = nullptr;
    Node* focusedNode = nullptr;
    Node* selectingInput = nullptr;
    Node* selectingText = nullptr;
    Node* selectedText = nullptr;
    Node* scrollingNode = nullptr;
    ScrollbarAxis scrollingAxis = ScrollbarAxis::None;
    float scrollbarDragOffset = 0.0f;
    std::string lastError;
};

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Runtime::~Runtime() {
    if (impl_) {
        impl_->renderer.clearCaches();
    }
}

bool Runtime::loadDocument(const std::string& path) {
    Document next;
    std::string error;
    if (!impl_->parser.loadFile(path, next, error)) {
        impl_->lastError = std::move(error);
        return false;
    }
    impl_->document = std::move(next);
    if (impl_->document.root) {
        rebindParents(*impl_->document.root, nullptr);
    }
    impl_->hasDocument = true;
    impl_->dirty = true;
    impl_->mousePressed = false;
    impl_->pressedButton = MouseButton::None;
    impl_->hoveredLeaf = nullptr;
    impl_->pressedLeaf = nullptr;
    impl_->focusedNode = nullptr;
    impl_->selectingInput = nullptr;
    impl_->selectingText = nullptr;
    impl_->selectedText = nullptr;
    impl_->scrollingNode = nullptr;
    impl_->scrollingAxis = ScrollbarAxis::None;
    impl_->scrollbarDragOffset = 0.0f;
    impl_->lastError.clear();
    impl_->recomputeAndLayout();
    return true;
}

bool Runtime::loadDocumentFromString(std::string_view html, std::string_view basePath) {
    Document next;
    std::string error;
    if (!impl_->parser.loadString(html, basePath, next, error)) {
        impl_->lastError = std::move(error);
        return false;
    }
    impl_->document = std::move(next);
    if (impl_->document.root) {
        rebindParents(*impl_->document.root, nullptr);
    }
    impl_->hasDocument = true;
    impl_->dirty = true;
    impl_->mousePressed = false;
    impl_->pressedButton = MouseButton::None;
    impl_->hoveredLeaf = nullptr;
    impl_->pressedLeaf = nullptr;
    impl_->focusedNode = nullptr;
    impl_->selectingInput = nullptr;
    impl_->selectingText = nullptr;
    impl_->selectedText = nullptr;
    impl_->scrollingNode = nullptr;
    impl_->scrollingAxis = ScrollbarAxis::None;
    impl_->scrollbarDragOffset = 0.0f;
    impl_->lastError.clear();
    impl_->recomputeAndLayout();
    return true;
}

void Runtime::resize(int width, int height, float dpiScale) {
    width = std::max(1, width);
    height = std::max(1, height);
    dpiScale = std::max(0.1f, dpiScale);
    if (width == impl_->width && height == impl_->height && dpiScale == impl_->dpiScale) {
        return;
    }

    impl_->width = width;
    impl_->height = height;
    impl_->dpiScale = dpiScale;
    impl_->dirty = true;
    if (impl_->hasDocument) {
        impl_->recomputeAndLayout();
    }
}

bool Runtime::handleEvent(const Event& event) {
    if (!impl_->hasDocument || !impl_->document.root) {
        return false;
    }

    const float scale = std::max(0.1f, impl_->dpiScale);
    const float x = event.x / scale;
    const float y = event.y / scale;
    const bool pointerEvent = isPointerEvent(event.type);
    Node* hit = pointerEvent && event.type != EventType::MouseLeave ? hitTest(*impl_->document.root, x, y) : nullptr;
    std::optional<ScrollbarHit> scrollbarHit = pointerEvent && event.type != EventType::MouseLeave
        ? scrollbarHitTest(*impl_->document.root, x, y)
        : std::nullopt;
    bool consumed = false;
    bool stateChanged = false;
    bool textChanged = false;

    switch (event.type) {
    case EventType::MouseMove:
        if (impl_->scrollingNode && impl_->mousePressed) {
            const float pointer = impl_->scrollingAxis == ScrollbarAxis::Vertical ? y : x;
            stateChanged = updateScrollFromScrollbar(*impl_->scrollingNode,
                                                     impl_->scrollingAxis,
                                                     pointer,
                                                     impl_->scrollbarDragOffset) || stateChanged;
            consumed = true;
        } else if (impl_->selectingInput && impl_->mousePressed) {
            const size_t index = impl_->editableIndexAtPoint(*impl_->selectingInput, x, y);
            setInputSelection(*impl_->selectingInput, impl_->selectingInput->selectionAnchor, index);
            stateChanged = true;
            consumed = true;
        } else if (impl_->selectingText && impl_->mousePressed) {
            const size_t index = impl_->selectableIndexAtX(*impl_->selectingText, x);
            setSelectableSelection(*impl_->selectingText, impl_->selectingText->selectionAnchor, index);
            stateChanged = true;
            consumed = true;
        }
        if (hit != impl_->hoveredLeaf) {
            impl_->hoveredLeaf = hit;
            stateChanged = true;
        }
        break;
    case EventType::MouseLeave:
        if (hit != impl_->hoveredLeaf) {
            impl_->hoveredLeaf = hit;
            stateChanged = true;
        }
        consumed = hit != nullptr;
        break;
    case EventType::MouseDown:
        if (event.button == MouseButton::None) {
            return false;
        }
        impl_->mousePressed = true;
        impl_->pressedButton = event.button;
        impl_->pressedLeaf = hit;
        impl_->hoveredLeaf = hit;
        if (scrollbarHit) {
            impl_->scrollingNode = scrollbarHit->node;
            impl_->scrollingAxis = scrollbarHit->axis;
            impl_->scrollbarDragOffset = scrollbarHit->dragOffset;
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            const float pointer = impl_->scrollingAxis == ScrollbarAxis::Vertical ? y : x;
            stateChanged = updateScrollFromScrollbar(*impl_->scrollingNode,
                                                     impl_->scrollingAxis,
                                                     pointer,
                                                     impl_->scrollbarDragOffset) || stateChanged;
            consumed = true;
        } else if (Node* input = inputTarget(hit)) {
            const bool wasFocused = impl_->focusedNode == input;
            const size_t selectionAnchor = wasFocused
                ? (hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex)
                : input->value.size();
            stateChanged = impl_->setFocusedNode(input) || stateChanged;
            const size_t index = impl_->editableIndexAtPoint(*input, x, y);
            if (event.shiftKey) {
                setInputSelection(*input, selectionAnchor, index);
            } else {
                setInputCursor(*input, index);
            }
            impl_->selectingInput = input;
            impl_->selectingText = nullptr;
            if (impl_->selectedText) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
                impl_->selectedText = nullptr;
            }
            stateChanged = true;
        } else if (Node* selectable = selectableTextTarget(hit)) {
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            if (impl_->selectedText && impl_->selectedText != selectable) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
            }
            const size_t selectionAnchor = (event.shiftKey && hasSelectableSelection(*selectable))
                ? selectable->selectionAnchor
                : impl_->selectableIndexAtX(*selectable, x);
            const size_t index = impl_->selectableIndexAtX(*selectable, x);
            if (event.shiftKey) {
                setSelectableSelection(*selectable, selectionAnchor, index);
            } else {
                setSelectableCursor(*selectable, index);
            }
            impl_->selectingInput = nullptr;
            impl_->selectingText = selectable;
            impl_->selectedText = selectable;
            stateChanged = true;
        } else {
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            if (impl_->selectedText) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
                impl_->selectedText = nullptr;
            }
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
        }
        if (Node* target = actionTarget(hit); target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseDown, *target, event, x, y));
        }
        consumed = hit != nullptr;
        stateChanged = true;
        break;
    case EventType::MouseDoubleClick:
        if (Node* input = inputTarget(hit)) {
            stateChanged = impl_->setFocusedNode(input) || stateChanged;
            const size_t index = impl_->editableIndexAtPoint(*input, x, y);
            selectInputWordAt(*input, index);
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            consumed = true;
            stateChanged = true;
        } else if (Node* selectable = selectableTextTarget(hit)) {
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            if (impl_->selectedText && impl_->selectedText != selectable) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
            }
            const size_t index = impl_->selectableIndexAtX(*selectable, x);
            selectSelectableWordAt(*selectable, index);
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            impl_->selectedText = selectable;
            consumed = true;
            stateChanged = true;
        } else {
            consumed = hit != nullptr;
        }
        break;
    case EventType::MouseUp: {
        if (event.button == MouseButton::None) {
            return false;
        }
        Node* pressed = impl_->pressedLeaf;
        Node* pressedAction = actionTarget(pressed);
        Node* releasedAction = actionTarget(hit);
        const bool click = pressedAction && pressedAction == releasedAction && event.button == impl_->pressedButton;
        if (impl_->selectingInput) {
            const size_t index = impl_->editableIndexAtPoint(*impl_->selectingInput, x, y);
            setInputSelection(*impl_->selectingInput, impl_->selectingInput->selectionAnchor, index);
            impl_->selectingInput = nullptr;
            stateChanged = true;
        }
        if (impl_->selectingText) {
            const size_t index = impl_->selectableIndexAtX(*impl_->selectingText, x);
            setSelectableSelection(*impl_->selectingText, impl_->selectingText->selectionAnchor, index);
            impl_->selectingText = nullptr;
            stateChanged = true;
        }
        if (Node* target = releasedAction ? releasedAction : pressedAction; target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseUp, *target, event, x, y));
        }
        if (click) {
            if (impl_->options.onElementEvent) {
                impl_->options.onElementEvent(makeElementEvent(ElementEventType::Click, *pressedAction, event, x, y));
            }
        }
        consumed = hit != nullptr || pressed != nullptr;
        impl_->mousePressed = false;
        impl_->pressedButton = MouseButton::None;
        impl_->pressedLeaf = nullptr;
        impl_->selectingText = nullptr;
        impl_->scrollingNode = nullptr;
        impl_->scrollingAxis = ScrollbarAxis::None;
        impl_->scrollbarDragOffset = 0.0f;
        impl_->hoveredLeaf = hit;
        stateChanged = true;
        break;
    }
    case EventType::MouseWheel: {
        const float step = event.wheelDelta == 0.0f ? 0.0f : -event.wheelDelta / 120.0f * 48.0f;
        const float dx = event.shiftKey ? step : 0.0f;
        const float dy = event.shiftKey ? 0.0f : step;
        stateChanged = scrollNearest(hit, dx, dy);
        consumed = stateChanged;
        break;
    }
    case EventType::KeyDown: {
        Node* input = impl_->focusedNode;
        if (!isEditableNode(input)) {
            if (event.ctrlKey && impl_->selectedText) {
                switch (event.key) {
                case 'A':
                    stateChanged = selectAllSelectable(*impl_->selectedText) || stateChanged;
                    consumed = stateChanged;
                    break;
                case 'C':
                    if (impl_->options.writeClipboardText && hasSelectableSelection(*impl_->selectedText)) {
                        impl_->options.writeClipboardText(selectedSelectableText(*impl_->selectedText));
                    }
                    consumed = hasSelectableSelection(*impl_->selectedText);
                    break;
                default:
                    break;
                }
                break;
            }
            return false;
        }

        constexpr unsigned kBackspace = 0x08;
        constexpr unsigned kEnter = 0x0D;
        constexpr unsigned kEscape = 0x1B;
        constexpr unsigned kEnd = 0x23;
        constexpr unsigned kHome = 0x24;
        constexpr unsigned kLeft = 0x25;
        constexpr unsigned kRight = 0x27;
        constexpr unsigned kDelete = 0x2E;

        if (event.ctrlKey) {
            switch (event.key) {
            case 'A':
                stateChanged = selectAllInput(*input) || stateChanged;
                consumed = true;
                break;
            case 'C':
                if (impl_->options.writeClipboardText && hasInputSelection(*input)) {
                    impl_->options.writeClipboardText(selectedInputText(*input));
                }
                consumed = hasInputSelection(*input);
                break;
            case 'X':
                if (impl_->options.writeClipboardText && hasInputSelection(*input)) {
                    impl_->options.writeClipboardText(selectedInputText(*input));
                }
                textChanged = eraseInputSelection(*input);
                consumed = true;
                break;
            case 'V':
                if (impl_->options.readClipboardText) {
                    const std::string text = editableText(*input, impl_->options.readClipboardText());
                    textChanged = insertInputText(*input, text);
                }
                consumed = true;
                break;
            case 'Z':
                textChanged = restoreInputUndo(*input);
                consumed = true;
                break;
            default:
                break;
            }
            if (consumed) {
                break;
            }
        }

        switch (event.key) {
        case kBackspace:
            textChanged = erasePreviousInputChar(*input);
            consumed = true;
            break;
        case kEnter:
            if (isTextareaNode(input)) {
                textChanged = insertInputText(*input, "\n");
                consumed = true;
                break;
            }
            return false;
        case kDelete:
            textChanged = eraseNextInputChar(*input);
            consumed = true;
            break;
        case kLeft: {
            const size_t previous = event.shiftKey
                ? previousUtf8Index(input->value, input->cursorIndex)
                : (hasInputSelection(*input) ? input->selectionStart : previousUtf8Index(input->value, input->cursorIndex));
            if (previous != input->cursorIndex || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    setInputSelection(*input, anchor, previous);
                } else {
                    setInputCursor(*input, previous);
                }
                stateChanged = true;
            }
            consumed = true;
            break;
        }
        case kRight: {
            const size_t next = event.shiftKey
                ? nextUtf8Index(input->value, input->cursorIndex)
                : (hasInputSelection(*input) ? input->selectionEnd : nextUtf8Index(input->value, input->cursorIndex));
            if (next != input->cursorIndex || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    setInputSelection(*input, anchor, next);
                } else {
                    setInputCursor(*input, next);
                }
                stateChanged = true;
            }
            consumed = true;
            break;
        }
        case kHome:
            if (const size_t target = isTextareaNode(input) ? currentLineStart(input->value, input->cursorIndex) : 0;
                input->cursorIndex != target || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    setInputSelection(*input, anchor, target);
                } else {
                    setInputCursor(*input, target);
                }
                stateChanged = true;
            }
            consumed = true;
            break;
        case kEnd:
            if (const size_t target = isTextareaNode(input) ? currentLineEnd(input->value, input->cursorIndex) : input->value.size();
                input->cursorIndex != target || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    setInputSelection(*input, anchor, target);
                } else {
                    setInputCursor(*input, target);
                }
                stateChanged = true;
            }
            consumed = true;
            break;
        case kEscape:
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            consumed = true;
            break;
        default:
            return false;
        }
        break;
    }
    case EventType::TextInput:
        if (!isEditableNode(impl_->focusedNode) || event.text.empty()) {
            return false;
        }
        textChanged = insertInputText(*impl_->focusedNode, editableText(*impl_->focusedNode, event.text));
        consumed = true;
        break;
    case EventType::ImeComposition:
        if (!isEditableNode(impl_->focusedNode)) {
            return false;
        }
        if (impl_->focusedNode->compositionText != event.text) {
            impl_->focusedNode->compositionText = event.text;
            stateChanged = true;
        }
        consumed = true;
        break;
    case EventType::ImeEnd:
        if (!isEditableNode(impl_->focusedNode)) {
            return false;
        }
        if (!impl_->focusedNode->compositionText.empty()) {
            impl_->focusedNode->compositionText.clear();
            stateChanged = true;
        }
        consumed = true;
        break;
    default:
        return false;
    }

    if (pointerEvent) {
        std::vector<Node*> hovered;
        std::vector<Node*> active;
        collectChain(impl_->hoveredLeaf, hovered);
        if (impl_->mousePressed && impl_->pressedLeaf) {
            collectChain(impl_->pressedLeaf, active);
        }
        if (updateStateTree(*impl_->document.root, hovered, active)) {
            stateChanged = true;
        }
    }
    if (textChanged && impl_->focusedNode && impl_->options.onElementEvent) {
        impl_->options.onElementEvent(makeElementEvent(ElementEventType::Input, *impl_->focusedNode, event, x, y));
    }
    if (stateChanged || textChanged) {
        impl_->recomputeAndLayout();
        impl_->dirty = true;
    }
    return consumed || stateChanged;
}

void Runtime::render(SkCanvas& canvas) {
    const auto traceStart = perf::Trace::now();
    if (!impl_->hasDocument || !impl_->document.root) {
        canvas.clear(impl_->options.clearColor);
        return;
    }

    impl_->renderer.draw(impl_->document, canvas, impl_->width, impl_->height, impl_->dpiScale);
    impl_->dirty = false;
    perf::Trace::write("skui", "runtime_render", impl_->width, impl_->height, perf::Trace::elapsedMs(traceStart));
}

bool Runtime::addClassById(std::string_view id, std::string_view className) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() || className.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node || hasClass(*node, className)) {
        return false;
    }
    node->classes.emplace_back(className);
    node->attributes["class"] = classAttributeValue(node->classes);
    impl_->recomputeAndLayout();
    impl_->dirty = true;
    return true;
}

bool Runtime::removeClassById(std::string_view id, std::string_view className) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() || className.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    auto it = std::find(node->classes.begin(), node->classes.end(), className);
    if (it == node->classes.end()) {
        return false;
    }
    node->classes.erase(it);
    if (node->classes.empty()) {
        node->attributes.erase("class");
    } else {
        node->attributes["class"] = classAttributeValue(node->classes);
    }
    impl_->recomputeAndLayout();
    impl_->dirty = true;
    return true;
}

bool Runtime::setStyleById(std::string_view id, std::string_view declarations) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    node->inlineStyle = {};
    parseInlineStyle(declarations, node->inlineStyle);
    node->attributes["style"] = std::string(declarations);
    impl_->recomputeAndLayout();
    impl_->dirty = true;
    return true;
}

bool Runtime::setAttributeById(std::string_view id, std::string_view name, std::string_view value) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() || name.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    const std::string normalizedName = lowerAscii(trim(name));
    if (normalizedName.empty()) {
        return false;
    }
    node->attributes[normalizedName] = std::string(value);
    syncNodeAttribute(*node, normalizedName);
    impl_->recomputeAndLayout();
    impl_->dirty = true;
    return true;
}

bool Runtime::removeAttributeById(std::string_view id, std::string_view name) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() || name.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    const std::string normalizedName = lowerAscii(trim(name));
    if (normalizedName.empty()) {
        return false;
    }
    const auto erased = node->attributes.erase(normalizedName);
    if (erased == 0) {
        return false;
    }
    syncNodeAttribute(*node, normalizedName);
    impl_->recomputeAndLayout();
    impl_->dirty = true;
    return true;
}

bool Runtime::hasClassById(std::string_view id, std::string_view className) const {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() || className.empty()) {
        return false;
    }
    const Node* node = findById(*impl_->document.root, id);
    return node && hasClass(*node, className);
}

void Runtime::setElementEventCallback(ElementEventCallback callback) {
    impl_->options.onElementEvent = std::move(callback);
}

bool Runtime::renderToBgraPixels(uint32_t* pixels, int width, int height, size_t rowBytes, float dpiScale) {
    width = std::max(1, width);
    height = std::max(1, height);
    dpiScale = std::max(0.1f, dpiScale);
    if (!pixels || rowBytes < static_cast<size_t>(width) * sizeof(uint32_t)) {
        impl_->lastError = "invalid renderToBgraPixels arguments";
        return false;
    }

    const SkImageInfo info = SkImageInfo::Make(width,
                                               height,
                                               kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
    if (!surface) {
        impl_->lastError = "failed to create offscreen Skia surface";
        return false;
    }

    resize(width, height, dpiScale);
    render(*surface->getCanvas());
    return true;
}

int Runtime::width() const {
    return impl_->width;
}

int Runtime::height() const {
    return impl_->height;
}

float Runtime::dpiScale() const {
    return impl_->dpiScale;
}

bool Runtime::dirty() const {
    return impl_->dirty;
}

std::string Runtime::lastError() const {
    return impl_->lastError;
}

void Runtime::clearDirty() {
    impl_->dirty = false;
}

}  // namespace skui
