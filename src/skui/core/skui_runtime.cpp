#include "skui_internal.h"

#include "perf_trace.h"
#include "skui_runtime_helpers.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skui {
namespace {

const std::string& selectableTextValue(const Node& node);

void rebindParents(Node& node, Node* parent) {
    node.parent = parent;
    for (auto& child : node.children) {
        rebindParents(*child, &node);
    }
}

bool isRenderableNode(const Node& node) {
    return node.style.display != Display::None &&
           node.style.visibility != Visibility::Hidden &&
           node.layout.w > 0.0f &&
           node.layout.h > 0.0f;
}

Node* hitTest(Node& node, float x, float y) {
    const float stickyOffsetY = stickyVisualOffsetY(node);
    Rect visualLayout = node.layout;
    visualLayout.y += stickyOffsetY;
    if (!isRenderableNode(node) || !visualLayout.contains(x, y)) {
        return nullptr;
    }
    const Rect contentClip = scrollContentClipRect(node);
    if (!contentClip.contains(x, y) &&
        (node.style.scrollbarGutterStable || shouldShowScrollbarX(node) || shouldShowScrollbarY(node))) {
        return node.style.pointerEvents == PointerEvents::None ? nullptr : &node;
    }
    const float childX = x + node.scrollX;
    const float childY = y + node.scrollY - stickyOffsetY;
    if (requiresZIndexOrdering(node)) {
        const std::vector<Node*> orderedChildren = childrenInPaintOrder(node);
        for (auto it = orderedChildren.rbegin(); it != orderedChildren.rend(); ++it) {
            if (Node* hit = hitTest(**it, childX, childY)) {
                return hit;
            }
        }
    } else {
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            if (Node* hit = hitTest(**it, childX, childY)) {
                return hit;
            }
        }
    }
    return node.style.pointerEvents == PointerEvents::None ? nullptr : &node;
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

bool isNodeDisabled(const Node& node) {
    return node.attributes.contains("disabled");
}

Node* disabledTarget(Node* leaf) {
    for (Node* current = leaf; current; current = current->parent) {
        if (isNodeDisabled(*current)) {
            return current;
        }
    }
    return nullptr;
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

bool containsNode(const Node& root, const Node* node) {
    if (!node) {
        return false;
    }
    if (&root == node) {
        return true;
    }
    for (const auto& child : root.children) {
        if (containsNode(*child, node)) {
            return true;
        }
    }
    return false;
}

std::optional<size_t> childIndex(Node& parent, const Node& child) {
    for (size_t i = 0; i < parent.children.size(); ++i) {
        if (parent.children[i].get() == &child) {
            return i;
        }
    }
    return std::nullopt;
}

bool updateStateTree(Node& node,
                     const std::vector<Node*>& hovered,
                     const std::vector<Node*>& active,
                     bool ancestorDisabled = false) {
    bool changed = false;
    const bool disabled = ancestorDisabled || isNodeDisabled(node);
    const bool nextHovered = !disabled && containsNode(hovered, node);
    const bool nextActive = !disabled && containsNode(active, node);
    if (node.hovered != nextHovered) {
        node.hovered = nextHovered;
        changed = true;
    }
    if (node.active != nextActive) {
        node.active = nextActive;
        changed = true;
    }
    for (auto& child : node.children) {
        changed = updateStateTree(*child, hovered, active, disabled) || changed;
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
    return node && isTextEditingNode(*node);
}

bool isSelectableTextNode(const Node* node) {
    return node && node->tag == "selectable";
}

bool isTextareaNode(const Node* node) {
    return node && node->tag == "textarea";
}

Node* inputTarget(Node* leaf) {
    if (disabledTarget(leaf)) {
        return nullptr;
    }
    for (Node* current = leaf; current; current = current->parent) {
        if (isEditableNode(current)) {
            return current;
        }
        if (contentEditableState(*current) == ContentEditableState::False) {
            return nullptr;
        }
    }
    return nullptr;
}

Node* inputEventTarget(Node& node) {
    if (Node* host = contentEditableEditingHost(&node)) {
        return host;
    }
    return &node;
}

Node* selectableTextTarget(Node* leaf) {
    if (disabledTarget(leaf)) {
        return nullptr;
    }
    for (Node* current = leaf; current; current = current->parent) {
        if (isSelectableTextNode(current)) {
            return current;
        }
    }
    return nullptr;
}

Node* actionTarget(Node* leaf) {
    if (disabledTarget(leaf)) {
        return nullptr;
    }
    for (Node* current = leaf; current; current = current->parent) {
        if (!current->action.empty()) {
            return current;
        }
    }
    return nullptr;
}

Node* mouseEventTarget(Node* leaf) {
    if (disabledTarget(leaf)) {
        return nullptr;
    }
    if (Node* target = actionTarget(leaf)) {
        return target;
    }
    if (Node* target = inputTarget(leaf)) {
        return target;
    }
    if (Node* target = selectableTextTarget(leaf)) {
        return target;
    }
    return leaf;
}

bool isPointerConsumingTarget(Node* leaf) {
    return disabledTarget(leaf) ||
           actionTarget(leaf) ||
           inputTarget(leaf) ||
           selectableTextTarget(leaf);
}

const Node::TextLink* selectableLinkAtIndex(const Node& node, size_t index) {
    if (!isSelectableTextNode(&node)) {
        return nullptr;
    }
    for (const Node::TextLink& link : node.textLinks) {
        if (index >= link.start && index < link.end && !link.action.empty()) {
            return &link;
        }
    }
    return nullptr;
}

std::optional<Cursor> explicitCursorForNode(Node* leaf) {
    for (Node* current = leaf; current; current = current->parent) {
        if (current->style.cursor != Cursor::Auto) {
            return current->style.cursor;
        }
    }
    return std::nullopt;
}

Cursor defaultCursorForNode(Node* leaf) {
    if (inputTarget(leaf) || selectableTextTarget(leaf)) {
        return Cursor::Text;
    }
    return Cursor::Default;
}

Cursor cursorForNode(Node* leaf) {
    if (const std::optional<Cursor> cursor = explicitCursorForNode(leaf)) {
        return *cursor;
    }
    return defaultCursorForNode(leaf);
}

bool canScrollX(const Node& node) {
    return shouldShowScrollbarX(node) && scrollMaxX(node) > 0.0f;
}

bool canScrollY(const Node& node) {
    return shouldShowScrollbarY(node) && scrollMaxY(node) > 0.0f;
}

bool allowsProgrammaticScrollX(const Node& node) {
    return node.tag == "textarea" ||
           node.style.overflowX == Overflow::Auto ||
           node.style.overflowX == Overflow::Scroll;
}

bool allowsProgrammaticScrollY(const Node& node) {
    return node.tag == "textarea" ||
           node.style.overflowY == Overflow::Auto ||
           node.style.overflowY == Overflow::Scroll;
}

bool isProgrammaticScrollContainer(const Node& node) {
    return allowsProgrammaticScrollX(node) || allowsProgrammaticScrollY(node);
}

ScrollState scrollStateForNode(const Node& node) {
    return {
        node.scrollX,
        node.scrollY,
        scrollMaxX(node),
        scrollMaxY(node),
        scrollViewportWidth(node),
        scrollViewportHeight(node),
        node.scrollContentWidth,
        node.scrollContentHeight,
    };
}

bool setNodeScrollOffset(Node& node, float scrollX, float scrollY) {
    const float nextX = allowsProgrammaticScrollX(node)
                            ? clampf(scrollX, 0.0f, scrollMaxX(node))
                            : node.scrollX;
    const float nextY = allowsProgrammaticScrollY(node)
                            ? clampf(scrollY, 0.0f, scrollMaxY(node))
                            : node.scrollY;
    const bool changed = nextX != node.scrollX || nextY != node.scrollY;
    node.scrollX = nextX;
    node.scrollY = nextY;
    return changed;
}

float nearestVisibilityDelta(float start,
                             float end,
                             float viewportStart,
                             float viewportEnd) {
    const bool before = start < viewportStart;
    const bool after = end > viewportEnd;
    if (before == after) {
        return 0.0f;
    }
    return before ? start - viewportStart : end - viewportEnd;
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

bool wantsScrollbarX(const Node& node) {
    return shouldShowScrollbarX(node);
}

bool wantsScrollbarY(const Node& node) {
    return shouldShowScrollbarY(node);
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
        geometry.maxScroll = scrollMaxY(node);
        geometry.trackStart = node.layout.y + kSkuiScrollbarInset;
        geometry.trackCross = node.layout.x + node.layout.w - kSkuiScrollbarInset - kSkuiScrollbarThickness;
        geometry.trackLength = node.layout.h - kSkuiScrollbarInset * 2.0f - (showX ? kSkuiScrollbarThickness + kSkuiScrollbarInset : 0.0f);
        const float viewportHeight = scrollViewportHeight(node);
        const float ratio = node.scrollContentHeight <= 0.0f ? 1.0f : viewportHeight / node.scrollContentHeight;
        geometry.thumbLength = clampf(geometry.trackLength * ratio,
                                      std::min(kSkuiScrollbarMinThumb, geometry.trackLength),
                                      geometry.trackLength);
        const float travel = std::max(0.0f, geometry.trackLength - geometry.thumbLength);
        geometry.thumbStart = geometry.trackStart + (geometry.maxScroll <= 0.0f ? 0.0f : node.scrollY / geometry.maxScroll * travel);
    } else {
        geometry.maxScroll = scrollMaxX(node);
        geometry.trackStart = node.layout.x + kSkuiScrollbarInset;
        geometry.trackCross = node.layout.y + node.layout.h - kSkuiScrollbarInset - kSkuiScrollbarThickness;
        geometry.trackLength = node.layout.w - kSkuiScrollbarInset * 2.0f - (showY ? kSkuiScrollbarThickness + kSkuiScrollbarInset : 0.0f);
        const float viewportWidth = scrollViewportWidth(node);
        const float ratio = node.scrollContentWidth <= 0.0f ? 1.0f : viewportWidth / node.scrollContentWidth;
        geometry.thumbLength = clampf(geometry.trackLength * ratio,
                                      std::min(kSkuiScrollbarMinThumb, geometry.trackLength),
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
                             x <= vertical->trackCross + kSkuiScrollbarThickness &&
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
                             y <= horizontal->trackCross + kSkuiScrollbarThickness;
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
    const Rect contentClip = scrollContentClipRect(node);
    if (!contentClip.contains(x, y) &&
        (node.style.scrollbarGutterStable || shouldShowScrollbarX(node) || shouldShowScrollbarY(node))) {
        return std::nullopt;
    }

    const float childX = x + node.scrollX;
    const float childY = y + node.scrollY;
    if (requiresZIndexOrdering(node)) {
        const std::vector<Node*> orderedChildren = childrenInPaintOrder(node);
        for (auto it = orderedChildren.rbegin(); it != orderedChildren.rend(); ++it) {
            if (std::optional<ScrollbarHit> hit = scrollbarHitTest(**it, childX, childY)) {
                return hit;
            }
        }
    } else {
        for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
            if (std::optional<ScrollbarHit> hit = scrollbarHitTest(**it, childX, childY)) {
                return hit;
            }
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

ElementEvent makeElementEvent(ElementEventType type, const Node& node, const Event& source, float x, float y) {
    ElementEvent event;
    event.type = type;
    event.tag = node.tag;
    event.id = node.id;
    event.classes = node.classes;
    event.action = node.action;
    event.text = node.text;
    event.value = isContentEditableEditingHost(node)
        ? editableTextContent(node)
        : node.value;
    event.x = x;
    event.y = y;
    event.scrollX = node.scrollX;
    event.scrollY = node.scrollY;
    event.button = source.button;
    return event;
}

ElementEvent makeSelectableLinkEvent(const Node& node,
                                     const Node::TextLink& link,
                                     const Event& source,
                                     float x,
                                     float y) {
    ElementEvent event = makeElementEvent(ElementEventType::Click, node, source, x, y);
    event.action = link.action;
    event.value = selectableTextValue(node);
    if (link.start < link.end && link.end <= event.value.size()) {
        event.text = event.value.substr(link.start, link.end - link.start);
    }
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

struct EditableLineCacheEntry {
    uint64_t revision = 0;
    size_t size = 0;
    std::vector<TextLine> lines;
};

void buildEditableLines(std::string_view value, std::vector<TextLine>& lines) {
    lines.clear();
    size_t start = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n') {
            lines.push_back({start, i});
            start = i + 1;
        }
    }
    lines.push_back({start, value.size()});
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

void markTextChanged(Node& node) {
    syncContentEditablePlaceholder(node);
    ++node.textRevision;
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
    markTextChanged(node);
    return true;
}

bool clearInputSelection(Node& node) {
    const size_t oldCursor = node.cursorIndex;
    const size_t oldAnchor = node.selectionAnchor;
    const size_t oldStart = node.selectionStart;
    const size_t oldEnd = node.selectionEnd;
    clampInputCursor(node);
    node.selectionAnchor = node.cursorIndex;
    node.selectionStart = node.cursorIndex;
    node.selectionEnd = node.cursorIndex;
    return oldCursor != node.cursorIndex ||
           oldAnchor != node.selectionAnchor ||
           oldStart != node.selectionStart ||
           oldEnd != node.selectionEnd;
}

bool setInputCursor(Node& node, size_t index) {
    const size_t oldCursor = node.cursorIndex;
    const size_t oldAnchor = node.selectionAnchor;
    const size_t oldStart = node.selectionStart;
    const size_t oldEnd = node.selectionEnd;
    node.cursorIndex = clampUtf8Index(node.value, index);
    clearInputSelection(node);
    return oldCursor != node.cursorIndex ||
           oldAnchor != node.selectionAnchor ||
           oldStart != node.selectionStart ||
           oldEnd != node.selectionEnd;
}

bool setInputSelection(Node& node, size_t anchor, size_t cursor) {
    const size_t nextAnchor = clampUtf8Index(node.value, anchor);
    const size_t nextCursor = clampUtf8Index(node.value, cursor);
    const size_t nextStart = std::min(nextAnchor, nextCursor);
    const size_t nextEnd = std::max(nextAnchor, nextCursor);
    const bool changed = node.selectionAnchor != nextAnchor ||
                         node.cursorIndex != nextCursor ||
                         node.selectionStart != nextStart ||
                         node.selectionEnd != nextEnd;
    node.selectionAnchor = nextAnchor;
    node.cursorIndex = nextCursor;
    node.selectionStart = nextStart;
    node.selectionEnd = nextEnd;
    return changed;
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
    markTextChanged(node);
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
    markTextChanged(node);
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
    markTextChanged(node);
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
    markTextChanged(node);
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

bool setSelectableCursor(Node& node, size_t index) {
    const std::string& value = selectableTextValue(node);
    const size_t nextCursor = clampUtf8Index(value, index);
    const bool changed = node.cursorIndex != nextCursor ||
                         node.selectionAnchor != nextCursor ||
                         node.selectionStart != nextCursor ||
                         node.selectionEnd != nextCursor;
    node.cursorIndex = nextCursor;
    node.selectionAnchor = nextCursor;
    node.selectionStart = nextCursor;
    node.selectionEnd = nextCursor;
    return changed;
}

bool setSelectableSelection(Node& node, size_t anchor, size_t cursor) {
    const std::string& value = selectableTextValue(node);
    const size_t nextAnchor = clampUtf8Index(value, anchor);
    const size_t nextCursor = clampUtf8Index(value, cursor);
    const size_t nextStart = std::min(nextAnchor, nextCursor);
    const size_t nextEnd = std::max(nextAnchor, nextCursor);
    const bool changed = node.selectionAnchor != nextAnchor ||
                         node.cursorIndex != nextCursor ||
                         node.selectionStart != nextStart ||
                         node.selectionEnd != nextEnd;
    node.selectionAnchor = nextAnchor;
    node.cursorIndex = nextCursor;
    node.selectionStart = nextStart;
    node.selectionEnd = nextEnd;
    return changed;
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

bool nearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

float mix(float from, float to, float t) {
    return from + (to - from) * t;
}

Length mixLength(const Length& from, const Length& to, float amount);

bool sameLength(const Length& lhs, const Length& rhs) {
    return lhs.unit == rhs.unit && nearlyEqual(lhs.value, rhs.value);
}

bool sameOptionalLength(const std::optional<Length>& lhs,
                        const std::optional<Length>& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || sameLength(*lhs, *rhs);
}

bool canInterpolateLength(const std::optional<Length>& from,
                          const std::optional<Length>& to) {
    return from &&
           to &&
           from->unit != LengthUnit::Auto &&
           from->unit == to->unit;
}

bool sameTransformOperation(const TransformOperation& lhs,
                            const TransformOperation& rhs) {
    return lhs.kind == rhs.kind &&
           sameLength(lhs.translateX, rhs.translateX) &&
           sameLength(lhs.translateY, rhs.translateY) &&
           nearlyEqual(lhs.scaleX, rhs.scaleX) &&
           nearlyEqual(lhs.scaleY, rhs.scaleY) &&
           nearlyEqual(lhs.rotateDeg, rhs.rotateDeg);
}

bool sameTransform(const Transform& lhs, const Transform& rhs) {
    if (lhs.operations.size() != rhs.operations.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.operations.size(); ++i) {
        if (!sameTransformOperation(lhs.operations[i], rhs.operations[i])) {
            return false;
        }
    }
    return true;
}

TransformOperation identityOperationFor(const TransformOperation& operation) {
    TransformOperation identity;
    identity.kind = operation.kind;
    if (operation.kind == TransformOperationKind::Translate) {
        identity.translateX.unit = operation.translateX.unit;
        identity.translateY.unit = operation.translateY.unit;
    }
    return identity;
}

TransformOperation mixTransformOperation(const TransformOperation& from,
                                         const TransformOperation& to,
                                         float amount) {
    TransformOperation mixed = to;
    mixed.translateX = mixLength(from.translateX, to.translateX, amount);
    mixed.translateY = mixLength(from.translateY, to.translateY, amount);
    mixed.scaleX = mix(from.scaleX, to.scaleX, amount);
    mixed.scaleY = mix(from.scaleY, to.scaleY, amount);
    mixed.rotateDeg = mix(from.rotateDeg, to.rotateDeg, amount);
    return mixed;
}

bool compatibleTransformOperations(const Transform& from, const Transform& to) {
    if (from.operations.empty() || to.operations.empty()) {
        return true;
    }
    if (from.operations.size() != to.operations.size()) {
        return false;
    }
    for (size_t i = 0; i < from.operations.size(); ++i) {
        if (from.operations[i].kind != to.operations[i].kind) {
            return false;
        }
    }
    return true;
}

Transform mixTransform(const Transform& from, const Transform& to, float t) {
    Transform mixed;
    if (!compatibleTransformOperations(from, to)) {
        return t < 1.0f ? from : to;
    }

    const std::vector<TransformOperation>& operations =
        from.operations.empty() ? to.operations : from.operations;
    mixed.operations.reserve(operations.size());
    for (size_t i = 0; i < operations.size(); ++i) {
        const TransformOperation& fromOperation =
            from.operations.empty() ? identityOperationFor(to.operations[i])
                                    : from.operations[i];
        const TransformOperation& toOperation =
            to.operations.empty() ? identityOperationFor(from.operations[i])
                                  : to.operations[i];
        mixed.operations.push_back(
            mixTransformOperation(fromOperation, toOperation, t));
    }
    return mixed;
}

CubicBezier bezierForKeyword(Easing easing) {
    switch (easing) {
    case Easing::Linear:
        return CubicBezier{0.0f, 0.0f, 1.0f, 1.0f};
    case Easing::EaseIn:
        return CubicBezier{0.42f, 0.0f, 1.0f, 1.0f};
    case Easing::EaseOut:
        return CubicBezier{0.0f, 0.0f, 0.58f, 1.0f};
    case Easing::EaseInOut:
        return CubicBezier{0.42f, 0.0f, 0.58f, 1.0f};
    case Easing::Ease:
    default:
        return CubicBezier{0.25f, 0.1f, 0.25f, 1.0f};
    }
}

float cubicBezierComponent(float a, float b, float t) {
    const float inv = 1.0f - t;
    return 3.0f * inv * inv * t * a +
           3.0f * inv * t * t * b +
           t * t * t;
}

float cubicBezierDerivative(float a, float b, float t) {
    const float inv = 1.0f - t;
    return 3.0f * inv * inv * a +
           6.0f * inv * t * (b - a) +
           3.0f * t * t * (1.0f - b);
}

float cubicBezierValue(const CubicBezier& curve, float progress) {
    float t = progress;
    for (int i = 0; i < 8; ++i) {
        const float x = cubicBezierComponent(curve.x1, curve.x2, t) - progress;
        const float dx = cubicBezierDerivative(curve.x1, curve.x2, t);
        if (std::abs(x) < 0.000001f || std::abs(dx) < 0.000001f) {
            break;
        }
        t = clampf(t - x / dx, 0.0f, 1.0f);
    }

    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 16; ++i) {
        const float x = cubicBezierComponent(curve.x1, curve.x2, t);
        if (std::abs(x - progress) < 0.000001f) {
            break;
        }
        if (x < progress) {
            lo = t;
        } else {
            hi = t;
        }
        t = (lo + hi) * 0.5f;
    }
    return cubicBezierComponent(curve.y1, curve.y2, t);
}

float easingValue(const EasingFunction& easing, float t) {
    t = clampf(t, 0.0f, 1.0f);
    return cubicBezierValue(easing.cubicBezier.value_or(
                                bezierForKeyword(easing.keyword)),
                            t);
}

bool transitionMatches(const TransitionDefinition& transition, TransitionProperty property) {
    return transition.property == TransitionProperty::All || transition.property == property;
}

std::optional<TransitionDefinition> transitionFor(const Style& style, TransitionProperty property) {
    for (const TransitionDefinition& transition : style.transitions) {
        if (transitionMatches(transition, property)) {
            return transition;
        }
    }
    return std::nullopt;
}

struct ActiveAnimation {
    Node* node = nullptr;
    TransitionProperty property = TransitionProperty::Opacity;
    Length fromHeight;
    Length toHeight;
    float fromOpacity = 1.0f;
    float toOpacity = 1.0f;
    Transform fromTransform;
    Transform toTransform;
    double startSeconds = 0.0;
    float durationSeconds = 0.0f;
    float delaySeconds = 0.0f;
    EasingFunction easing;
};

struct ActiveKeyframeAnimation {
    Node* node = nullptr;
    size_t animationIndex = 0;
    AnimationDefinition definition;
    double startSeconds = 0.0;
    std::optional<double> pauseStartedSeconds;
    double pausedSeconds = 0.0;
};

struct KeyframeAnimationSample {
    bool applies = false;
    bool needsFrame = false;
    bool hasBackgroundPosition = false;
    bool hasOpacity = false;
    bool hasTransform = false;
    BackgroundPosition position;
    float opacity = 1.0f;
    Transform transform;
};

struct StyleSnapshot {
    Node* node = nullptr;
    std::optional<Length> renderedHeight;
    std::optional<Length> targetHeight;
    float renderedOpacity = 1.0f;
    float targetOpacity = 1.0f;
    Transform renderedTransform;
    Transform targetTransform;
};

bool sameAnimationTiming(const AnimationTimingFunction& lhs,
                         const AnimationTimingFunction& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.easing == rhs.easing &&
           lhs.steps == rhs.steps &&
           lhs.stepPosition == rhs.stepPosition;
}

bool sameAnimationDefinition(const AnimationDefinition& lhs,
                             const AnimationDefinition& rhs) {
    return lhs.name == rhs.name &&
           nearlyEqual(lhs.durationSeconds, rhs.durationSeconds) &&
           nearlyEqual(lhs.delaySeconds, rhs.delaySeconds) &&
           nearlyEqual(lhs.iterationCount, rhs.iterationCount) &&
           sameAnimationTiming(lhs.timing, rhs.timing) &&
           lhs.direction == rhs.direction &&
           lhs.fillMode == rhs.fillMode;
}

bool fillsBackwards(AnimationFillMode fillMode) {
    return fillMode == AnimationFillMode::Backwards ||
           fillMode == AnimationFillMode::Both;
}

bool fillsForwards(AnimationFillMode fillMode) {
    return fillMode == AnimationFillMode::Forwards ||
           fillMode == AnimationFillMode::Both;
}

float directedAnimationProgress(float progress,
                                size_t iterationIndex,
                                AnimationDirection direction) {
    bool reverse = direction == AnimationDirection::Reverse;
    if (direction == AnimationDirection::Alternate) {
        reverse = iterationIndex % 2 == 1;
    } else if (direction == AnimationDirection::AlternateReverse) {
        reverse = iterationIndex % 2 == 0;
    }
    return reverse ? 1.0f - progress : progress;
}

float animationTimingValue(const AnimationTimingFunction& timing, float progress) {
    progress = clampf(progress, 0.0f, 1.0f);
    if (timing.kind == AnimationTimingKind::Easing) {
        return easingValue(timing.easing, progress);
    }

    const float stepCount = static_cast<float>(std::max(1, timing.steps));
    if (timing.stepPosition == AnimationStepPosition::Start) {
        return clampf((std::floor(progress * stepCount) + 1.0f) / stepCount,
                      0.0f,
                      1.0f);
    }
    if (progress >= 1.0f) {
        return 1.0f;
    }
    return clampf(std::floor(progress * stepCount) / stepCount, 0.0f, 1.0f);
}

Length mixLength(const Length& from, const Length& to, float amount) {
    if (from.unit != to.unit) {
        return amount < 1.0f ? from : to;
    }
    return Length{mix(from.value, to.value, amount), from.unit};
}

BackgroundPosition mixBackgroundPosition(const BackgroundPosition& from,
                                         const BackgroundPosition& to,
                                         float amount) {
    return {
        mixLength(from.x, to.x, amount),
        mixLength(from.y, to.y, amount),
    };
}

BackgroundPosition keyframePosition(const Keyframe& frame,
                                    const BackgroundPosition& fallback) {
    return frame.style.flags.backgroundPosition
               ? frame.style.backgroundPosition
               : fallback;
}

float keyframeOpacity(const Keyframe& frame, float fallback) {
    return frame.style.flags.opacity ? frame.style.opacity : fallback;
}

Transform keyframeTransform(const Keyframe& frame, const Transform& fallback) {
    return frame.style.flags.transform ? frame.style.transform : fallback;
}

bool hasBackgroundPositionFrames(const KeyframesDefinition& keyframes) {
    return std::any_of(keyframes.frames.begin(),
                       keyframes.frames.end(),
                       [](const Keyframe& frame) {
                           return frame.style.flags.backgroundPosition;
                       });
}

bool hasOpacityFrames(const KeyframesDefinition& keyframes) {
    return std::any_of(keyframes.frames.begin(),
                       keyframes.frames.end(),
                       [](const Keyframe& frame) {
                           return frame.style.flags.opacity;
                       });
}

bool hasTransformFrames(const KeyframesDefinition& keyframes) {
    return std::any_of(keyframes.frames.begin(),
                       keyframes.frames.end(),
                       [](const Keyframe& frame) {
                           return frame.style.flags.transform;
                       });
}

std::pair<const Keyframe*, const Keyframe*> keyframeSpan(
    const KeyframesDefinition& keyframes,
    float progress) {
    if (keyframes.frames.empty()) {
        return {nullptr, nullptr};
    }
    if (progress <= keyframes.frames.front().offset) {
        const Keyframe& frame = keyframes.frames.front();
        return {&frame, &frame};
    }
    if (progress >= keyframes.frames.back().offset) {
        const Keyframe& frame = keyframes.frames.back();
        return {&frame, &frame};
    }

    const auto upper = std::upper_bound(
        keyframes.frames.begin(),
        keyframes.frames.end(),
        progress,
        [](float value, const Keyframe& frame) {
            return value < frame.offset;
        });
    return {&*(upper - 1), &*upper};
}

float localKeyframeProgress(const Keyframe& from,
                            const Keyframe& to,
                            const AnimationTimingFunction& timing,
                            float progress) {
    if (&from == &to || nearlyEqual(from.offset, to.offset)) {
        return 1.0f;
    }
    const float span = std::max(0.000001f, to.offset - from.offset);
    const float localProgress = (progress - from.offset) / span;
    return animationTimingValue(timing, localProgress);
}

BackgroundPosition evaluateBackgroundPositionKeyframes(
    const KeyframesDefinition& keyframes,
    const BackgroundPosition& fallback,
    const AnimationTimingFunction& timing,
    float progress) {
    if (keyframes.frames.empty()) {
        return fallback;
    }
    const auto [fromFrame, toFrame] = keyframeSpan(keyframes, progress);
    if (!fromFrame || !toFrame) {
        return fallback;
    }
    const float timedProgress =
        localKeyframeProgress(*fromFrame, *toFrame, timing, progress);
    return mixBackgroundPosition(keyframePosition(*fromFrame, fallback),
                                 keyframePosition(*toFrame, fallback),
                                 timedProgress);
}

float evaluateOpacityKeyframes(const KeyframesDefinition& keyframes,
                               float fallback,
                               const AnimationTimingFunction& timing,
                               float progress) {
    if (keyframes.frames.empty()) {
        return fallback;
    }
    const auto [fromFrame, toFrame] = keyframeSpan(keyframes, progress);
    if (!fromFrame || !toFrame) {
        return fallback;
    }
    const float timedProgress =
        localKeyframeProgress(*fromFrame, *toFrame, timing, progress);
    return mix(keyframeOpacity(*fromFrame, fallback),
               keyframeOpacity(*toFrame, fallback),
               timedProgress);
}

Transform evaluateTransformKeyframes(const KeyframesDefinition& keyframes,
                                     const Transform& fallback,
                                     const AnimationTimingFunction& timing,
                                     float progress) {
    if (keyframes.frames.empty()) {
        return fallback;
    }
    const auto [fromFrame, toFrame] = keyframeSpan(keyframes, progress);
    if (!fromFrame || !toFrame) {
        return fallback;
    }
    const float timedProgress =
        localKeyframeProgress(*fromFrame, *toFrame, timing, progress);
    return mixTransform(keyframeTransform(*fromFrame, fallback),
                        keyframeTransform(*toFrame, fallback),
                        timedProgress);
}

void clearAnimatedStyle(Node& node) {
    node.animatedStyle = {};
    node.animatedStyleFlags = {};
    node.hasAnimatedStyle = false;
}

void clearAnimatedStyleTree(Node& node) {
    clearAnimatedStyle(node);
    for (auto& child : node.children) {
        clearAnimatedStyleTree(*child);
    }
}

Style renderedStyle(const Node& node) {
    Style style = node.style;
    if (node.hasAnimatedStyle) {
        if (node.animatedStyleFlags.height) {
            style.height = node.animatedStyle.height;
            style.flags.height = true;
        }
        if (node.animatedStyleFlags.opacity) {
            style.opacity = node.animatedStyle.opacity;
            style.flags.opacity = true;
        }
        if (node.animatedStyleFlags.transform) {
            style.transform = node.animatedStyle.transform;
            style.flags.transform = true;
        }
        if (node.animatedStyleFlags.backgroundPosition) {
            style.backgroundPosition = node.animatedStyle.backgroundPosition;
            style.flags.backgroundPosition = true;
        }
    }
    return style;
}

const ActiveAnimation* activeAnimationFor(const std::vector<ActiveAnimation>& animations,
                                          const Node* node,
                                          TransitionProperty property) {
    const auto it = std::find_if(animations.begin(),
                                 animations.end(),
                                 [node, property](const ActiveAnimation& animation) {
                                     return animation.node == node && animation.property == property;
                                 });
    return it == animations.end() ? nullptr : &*it;
}

void collectStyleSnapshots(const Node& node,
                           const std::vector<ActiveAnimation>& animations,
                           std::vector<StyleSnapshot>& snapshots) {
    const Style currentStyle = renderedStyle(node);
    StyleSnapshot snapshot;
    snapshot.node = const_cast<Node*>(&node);
    snapshot.renderedHeight = currentStyle.height;
    snapshot.renderedOpacity = currentStyle.opacity;
    snapshot.renderedTransform = currentStyle.transform;

    if (const ActiveAnimation* animation =
            activeAnimationFor(animations, &node, TransitionProperty::Height)) {
        snapshot.targetHeight = animation->toHeight;
    } else {
        snapshot.targetHeight = currentStyle.height;
    }
    if (const ActiveAnimation* animation =
            activeAnimationFor(animations, &node, TransitionProperty::Opacity)) {
        snapshot.targetOpacity = animation->toOpacity;
    } else {
        snapshot.targetOpacity = currentStyle.opacity;
    }
    if (const ActiveAnimation* animation =
            activeAnimationFor(animations, &node, TransitionProperty::Transform)) {
        snapshot.targetTransform = animation->toTransform;
    } else {
        snapshot.targetTransform = currentStyle.transform;
    }

    snapshots.push_back(snapshot);
    for (const auto& child : node.children) {
        collectStyleSnapshots(*child, animations, snapshots);
    }
}

const StyleSnapshot* findStyleSnapshot(const std::vector<StyleSnapshot>& snapshots, const Node* node) {
    const auto it = std::find_if(snapshots.begin(),
                                 snapshots.end(),
                                 [node](const StyleSnapshot& snapshot) {
                                     return snapshot.node == node;
                                 });
    return it == snapshots.end() ? nullptr : &*it;
}

void writeAnimatedOpacity(Node& node, float opacity) {
    node.animatedStyle.opacity = clampf(opacity, 0.0f, 1.0f);
    node.animatedStyleFlags.opacity = true;
    node.hasAnimatedStyle = true;
}

void writeAnimatedTransform(Node& node, const Transform& transform) {
    node.animatedStyle.transform = transform;
    node.animatedStyleFlags.transform = true;
    node.hasAnimatedStyle = true;
}

void writeAnimatedHeight(Node& node, const Length& height) {
    node.animatedStyle.height = height;
    node.animatedStyleFlags.height = true;
    node.hasAnimatedStyle = true;
}

void writeAnimatedBackgroundPosition(Node& node,
                                     const BackgroundPosition& position) {
    node.animatedStyle.backgroundPosition = position;
    node.animatedStyleFlags.backgroundPosition = true;
    node.hasAnimatedStyle = true;
}

void removeAnimationFor(std::vector<ActiveAnimation>& animations, const Node* node, TransitionProperty property) {
    animations.erase(std::remove_if(animations.begin(),
                                    animations.end(),
                                    [node, property](const ActiveAnimation& animation) {
                                        return animation.node == node && animation.property == property;
                                    }),
                     animations.end());
}

void clearAnimationsForNode(std::vector<ActiveAnimation>& animations, Node* node) {
    if (!node) {
        return;
    }
    animations.erase(std::remove_if(animations.begin(),
                                    animations.end(),
                                    [node](const ActiveAnimation& animation) {
                                        for (Node* current = animation.node; current; current = current->parent) {
                                            if (current == node) {
                                                return true;
                                            }
                                        }
                                        return false;
                                    }),
                     animations.end());
}

void clearAnimationsForMissingNodes(std::vector<ActiveAnimation>& animations, const Document& document) {
    if (!document.root) {
        animations.clear();
        return;
    }

    animations.erase(std::remove_if(animations.begin(),
                                    animations.end(),
                                    [&document](const ActiveAnimation& animation) {
                                        return !animation.node || !containsNode(*document.root, animation.node);
                                    }),
                     animations.end());
}

void clearKeyframeAnimationsForNode(
    std::vector<ActiveKeyframeAnimation>& animations,
    Node* node) {
    if (!node) {
        return;
    }
    animations.erase(std::remove_if(animations.begin(),
                                    animations.end(),
                                    [node](const ActiveKeyframeAnimation& animation) {
                                        for (Node* current = animation.node;
                                             current;
                                             current = current->parent) {
                                            if (current == node) {
                                                return true;
                                            }
                                        }
                                        return false;
                                    }),
                     animations.end());
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

bool isDisplayDeclaration(std::string_view declaration) {
    const size_t colon = declaration.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    return lowerAscii(trim(declaration.substr(0, colon))) == "display";
}

bool isPointerEventsDeclaration(std::string_view declaration) {
    const size_t colon = declaration.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    return lowerAscii(trim(declaration.substr(0, colon))) == "pointer-events";
}

std::string styleWithoutDeclaration(std::string_view declarations,
                                    bool (*matches)(std::string_view)) {
    std::string updated;
    size_t start = 0;
    while (start < declarations.size()) {
        const size_t end = declarations.find(';', start);
        const std::string_view declaration =
            end == std::string_view::npos
                ? declarations.substr(start)
                : declarations.substr(start, end - start);
        const std::string trimmed = trim(declaration);
        if (!trimmed.empty() && !matches(trimmed)) {
            if (!updated.empty() && updated.back() != ';') {
                updated += "; ";
            }
            updated += trimmed;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return updated;
}

std::string styleWithDisplay(std::string_view declarations, bool visible) {
    std::string updated = styleWithoutDeclaration(declarations, isDisplayDeclaration);
    if (!updated.empty() && updated.back() != ';') {
        updated += "; ";
    }
    updated += visible ? "display: flex" : "display: none";
    return updated;
}

std::string styleWithPointerEvents(std::string_view declarations, bool consumesEvents) {
    std::string updated = styleWithoutDeclaration(declarations, isPointerEventsDeclaration);
    if (!updated.empty() && updated.back() != ';') {
        updated += "; ";
    }
    updated += consumesEvents ? "pointer-events: auto" : "pointer-events: none";
    return updated;
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
        if (node.value != value) {
            node.value = value;
            markTextChanged(node);
        }
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
    } else if (name == "data-links") {
        node.textLinks.clear();
        size_t start = 0;
        while (start <= value.size()) {
            const size_t end = value.find('\n', start);
            const std::string_view row(value.data() + start,
                                       (end == std::string::npos ? value.size() : end) - start);
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
                const std::string& textValue = selectableTextValue(node);
                if (startResult.ec == std::errc{} &&
                    endResult.ec == std::errc{} &&
                    linkStart < linkEnd &&
                    linkEnd <= textValue.size()) {
                    node.textLinks.push_back(Node::TextLink{
                        linkStart,
                        linkEnd,
                        std::string(row.substr(second + 1)),
                    });
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    } else if (name == "data-virtual-width") {
        node.virtualContentWidth = 0.0f;
        const char* begin = value.data();
        const char* end = value.data() + value.size();
        std::from_chars(begin, end, node.virtualContentWidth);
        node.virtualContentWidth = std::max(0.0f, node.virtualContentWidth);
    } else if (name == "data-virtual-height") {
        node.virtualContentHeight = 0.0f;
        const char* begin = value.data();
        const char* end = value.data() + value.size();
        std::from_chars(begin, end, node.virtualContentHeight);
        node.virtualContentHeight = std::max(0.0f, node.virtualContentHeight);
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
        return static_cast<float>(width) / effectiveScale();
    }

    float logicalHeight() const {
        return static_cast<float>(height) / effectiveScale();
    }

    float userScale() const {
        return std::max(0.1f, options.scale);
    }

    float textScale() const {
        return std::max(0.1f, options.textScale);
    }

    float effectiveScale() const {
        return std::max(
            0.1f,
            std::max(0.1f, dpiScale) * userScale() * textScale());
    }

    bool hasAnimatedKeyframes(const KeyframesDefinition& keyframes) const {
        return std::any_of(keyframes.frames.begin(),
                           keyframes.frames.end(),
                           [](const Keyframe& frame) {
                               return frame.style.flags.backgroundPosition ||
                                      frame.style.flags.opacity ||
                                      frame.style.flags.transform;
                           });
    }

    void collectKeyframeAnimations(
        Node& node,
        double currentTimeSeconds,
        std::vector<ActiveKeyframeAnimation>& nextAnimations) {
        for (size_t index = 0; index < node.style.animations.size(); ++index) {
            const AnimationDefinition& definition = node.style.animations[index];
            const auto keyframesIt = document.keyframes.find(definition.name);
            if (keyframesIt == document.keyframes.end() ||
                !hasAnimatedKeyframes(keyframesIt->second)) {
                continue;
            }

            const auto existing = std::find_if(
                keyframeAnimations.begin(),
                keyframeAnimations.end(),
                [&node, index, &definition](
                    const ActiveKeyframeAnimation& animation) {
                    return animation.node == &node &&
                           animation.animationIndex == index &&
                           sameAnimationDefinition(animation.definition,
                                                   definition);
                });

            ActiveKeyframeAnimation animation;
            if (existing != keyframeAnimations.end()) {
                animation = *existing;
                const bool shouldPause =
                    definition.playState == AnimationPlayState::Paused;
                if (shouldPause && !animation.pauseStartedSeconds) {
                    animation.pauseStartedSeconds = currentTimeSeconds;
                } else if (!shouldPause &&
                           animation.pauseStartedSeconds) {
                    animation.pausedSeconds +=
                        currentTimeSeconds -
                        *animation.pauseStartedSeconds;
                    animation.pauseStartedSeconds.reset();
                }
                animation.definition = definition;
            } else {
                animation.node = &node;
                animation.animationIndex = index;
                animation.definition = definition;
                animation.startSeconds = currentTimeSeconds;
                if (definition.playState == AnimationPlayState::Paused) {
                    animation.pauseStartedSeconds = currentTimeSeconds;
                }
            }
            nextAnimations.push_back(std::move(animation));
        }

        for (auto& child : node.children) {
            collectKeyframeAnimations(
                *child,
                currentTimeSeconds,
                nextAnimations);
        }
    }

    void syncKeyframeAnimations(double currentTimeSeconds) {
        if (!document.root) {
            keyframeAnimations.clear();
            keyframeFramePending = false;
            return;
        }

        std::vector<ActiveKeyframeAnimation> nextAnimations;
        collectKeyframeAnimations(
            *document.root,
            currentTimeSeconds,
            nextAnimations);
        keyframeAnimations = std::move(nextAnimations);
    }

    KeyframeAnimationSample sampleKeyframeAnimation(
        const ActiveKeyframeAnimation& animation,
        double currentTimeSeconds) const {
        KeyframeAnimationSample sample;
        const auto keyframesIt =
            document.keyframes.find(animation.definition.name);
        if (!animation.node ||
            keyframesIt == document.keyframes.end() ||
            animation.definition.durationSeconds <= 0.0f) {
            return sample;
        }

        const double sampleTimeSeconds =
            animation.pauseStartedSeconds.value_or(currentTimeSeconds);
        const float elapsed = static_cast<float>(
            sampleTimeSeconds -
            animation.startSeconds -
            animation.pausedSeconds);
        const float activeTime = elapsed - animation.definition.delaySeconds;
        const bool running =
            animation.definition.playState == AnimationPlayState::Running;

        float progress = 0.0f;
        size_t iterationIndex = 0;
        if (activeTime < 0.0f) {
            sample.needsFrame = running;
            if (!fillsBackwards(animation.definition.fillMode)) {
                return sample;
            }
        } else {
            const float duration = animation.definition.durationSeconds;
            const bool finite = animation.definition.iterationCount >= 0.0f;
            const float totalDuration =
                finite ? duration * animation.definition.iterationCount
                       : 0.0f;
            if (finite && activeTime >= totalDuration) {
                if (!fillsForwards(animation.definition.fillMode)) {
                    return sample;
                }

                const float completedIterations =
                    animation.definition.iterationCount;
                const float wholeIterations = std::floor(completedIterations);
                const float fractional =
                    completedIterations - wholeIterations;
                if (fractional <= 0.000001f &&
                    completedIterations > 0.0f) {
                    iterationIndex =
                        static_cast<size_t>(wholeIterations - 1.0f);
                    progress = 1.0f;
                } else {
                    iterationIndex = static_cast<size_t>(wholeIterations);
                    progress = fractional;
                }
            } else {
                sample.needsFrame = running;
                const float iteration = activeTime / duration;
                const float wholeIteration = std::floor(iteration);
                iterationIndex =
                    static_cast<size_t>(std::max(0.0f, wholeIteration));
                progress = iteration - wholeIteration;
            }
        }

        progress = directedAnimationProgress(
            progress,
            iterationIndex,
            animation.definition.direction);
        const KeyframesDefinition& keyframes = keyframesIt->second;
        if (hasBackgroundPositionFrames(keyframes)) {
            sample.position = evaluateBackgroundPositionKeyframes(
                keyframes,
                animation.node->style.backgroundPosition,
                animation.definition.timing,
                progress);
            sample.hasBackgroundPosition = true;
        }
        if (hasOpacityFrames(keyframes)) {
            sample.opacity = evaluateOpacityKeyframes(
                keyframes,
                animation.node->style.opacity,
                animation.definition.timing,
                progress);
            sample.hasOpacity = true;
        }
        if (hasTransformFrames(keyframes)) {
            sample.transform = evaluateTransformKeyframes(
                keyframes,
                animation.node->style.transform,
                animation.definition.timing,
                progress);
            sample.hasTransform = true;
        }
        sample.applies = true;
        return sample;
    }

    bool applyKeyframeAnimations(double currentTimeSeconds) {
        keyframeFramePending = false;
        bool changed = false;
        for (const ActiveKeyframeAnimation& animation : keyframeAnimations) {
            const KeyframeAnimationSample sample =
                sampleKeyframeAnimation(animation, currentTimeSeconds);
            keyframeFramePending =
                keyframeFramePending || sample.needsFrame;
            if (!sample.applies) {
                continue;
            }
            if (sample.hasBackgroundPosition) {
                writeAnimatedBackgroundPosition(*animation.node,
                                                sample.position);
            }
            if (sample.hasOpacity) {
                writeAnimatedOpacity(*animation.node, sample.opacity);
            }
            if (sample.hasTransform) {
                writeAnimatedTransform(*animation.node, sample.transform);
            }
            changed = true;
        }
        return changed;
    }

    void recomputeAndLayout() {
        const float viewportWidth = logicalWidth();
        const float viewportHeight = logicalHeight();
        const bool traceEnabled = perf::Trace::enabled();
        const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        const auto styleStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        std::vector<StyleSnapshot> snapshots;
        if (document.root) {
            collectStyleSnapshots(*document.root, activeAnimations, snapshots);
            clearAnimatedStyleTree(*document.root);
        }
        recomputeStyles(document, options, viewportWidth, viewportHeight);
        startTransitions(snapshots);
        syncKeyframeAnimations(animationTimeSeconds);
        applyKeyframeAnimations(animationTimeSeconds);
        renderer.requestBitmapImages(document);
        if (traceEnabled) {
            perf::Trace::write("skui", "style_recompute", width, height, perf::Trace::elapsedMs(styleStart));
        }
        const auto layoutStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        if (document.root) {
            applyAnimatedStyles(*document.root);
        }
        layoutEngine.layout(document, viewportWidth, viewportHeight);
        requestAnimationFrame();
        if (traceEnabled) {
            perf::Trace::write("skui", "layout", width, height, perf::Trace::elapsedMs(layoutStart));
            perf::Trace::write("skui", "recompute_layout_total", width, height, perf::Trace::elapsedMs(traceStart));
        }
    }

    void startTransitions(const std::vector<StyleSnapshot>& snapshots) {
        if (!document.root) {
            activeAnimations.clear();
            return;
        }
        clearAnimationsForMissingNodes(activeAnimations, document);
        startTransitionsRecursive(*document.root, snapshots);
    }

    void startTransitionsRecursive(Node& node, const std::vector<StyleSnapshot>& snapshots) {
        const StyleSnapshot* snapshot = findStyleSnapshot(snapshots, &node);
        if (snapshot) {
            startHeightTransition(node, *snapshot);
            startOpacityTransition(node, *snapshot);
            startTransformTransition(node, *snapshot);
        }
        for (auto& child : node.children) {
            startTransitionsRecursive(*child, snapshots);
        }
    }

    void startHeightTransition(Node& node, const StyleSnapshot& snapshot) {
        const std::optional<Length>& nextHeight = node.style.height;
        if (sameOptionalLength(snapshot.targetHeight, nextHeight)) {
            if (snapshot.renderedHeight &&
                activeAnimationFor(activeAnimations,
                                   &node,
                                   TransitionProperty::Height)) {
                writeAnimatedHeight(node, *snapshot.renderedHeight);
            }
            return;
        }

        removeAnimationFor(activeAnimations, &node, TransitionProperty::Height);
        if (!canInterpolateLength(snapshot.renderedHeight, nextHeight)) {
            return;
        }

        std::optional<TransitionDefinition> transition =
            transitionFor(node.style, TransitionProperty::Height);
        if (!transition) {
            return;
        }

        ActiveAnimation animation;
        animation.node = &node;
        animation.property = TransitionProperty::Height;
        animation.fromHeight = *snapshot.renderedHeight;
        animation.toHeight = *nextHeight;
        animation.startSeconds = animationTimeSeconds;
        animation.durationSeconds = transition->durationSeconds;
        animation.delaySeconds = transition->delaySeconds;
        animation.easing = transition->easing;
        activeAnimations.push_back(animation);
        writeAnimatedHeight(node, animation.fromHeight);
    }

    void startOpacityTransition(Node& node, const StyleSnapshot& snapshot) {
        const float nextOpacity = node.style.opacity;
        if (nearlyEqual(snapshot.targetOpacity, nextOpacity)) {
            if (activeAnimationFor(activeAnimations,
                                   &node,
                                   TransitionProperty::Opacity)) {
                writeAnimatedOpacity(node, snapshot.renderedOpacity);
            }
            return;
        }

        removeAnimationFor(activeAnimations, &node, TransitionProperty::Opacity);
        std::optional<TransitionDefinition> transition =
            transitionFor(node.style, TransitionProperty::Opacity);
        if (!transition) {
            return;
        }

        ActiveAnimation animation;
        animation.node = &node;
        animation.property = TransitionProperty::Opacity;
        animation.fromOpacity = snapshot.renderedOpacity;
        animation.toOpacity = nextOpacity;
        animation.startSeconds = animationTimeSeconds;
        animation.durationSeconds = transition->durationSeconds;
        animation.delaySeconds = transition->delaySeconds;
        animation.easing = transition->easing;
        activeAnimations.push_back(animation);
        writeAnimatedOpacity(node, animation.fromOpacity);
    }

    void startTransformTransition(Node& node, const StyleSnapshot& snapshot) {
        const Transform nextTransform = node.style.transform;
        if (sameTransform(snapshot.targetTransform, nextTransform)) {
            if (activeAnimationFor(activeAnimations,
                                   &node,
                                   TransitionProperty::Transform)) {
                writeAnimatedTransform(node, snapshot.renderedTransform);
            }
            return;
        }

        removeAnimationFor(activeAnimations, &node, TransitionProperty::Transform);
        std::optional<TransitionDefinition> transition =
            transitionFor(node.style, TransitionProperty::Transform);
        if (!transition) {
            return;
        }

        ActiveAnimation animation;
        animation.node = &node;
        animation.property = TransitionProperty::Transform;
        animation.fromTransform = snapshot.renderedTransform;
        animation.toTransform = nextTransform;
        animation.startSeconds = animationTimeSeconds;
        animation.durationSeconds = transition->durationSeconds;
        animation.delaySeconds = transition->delaySeconds;
        animation.easing = transition->easing;
        activeAnimations.push_back(animation);
        writeAnimatedTransform(node, animation.fromTransform);
    }

    bool scrollNode(Node& node,
                    float dx,
                    float dy,
                    Node** scrolled = nullptr) {
        bool changed = false;
        if (dx != 0.0f && canScrollX(node)) {
            const float next =
                clampf(node.scrollX + dx, 0.0f, scrollMaxX(node));
            changed = next != node.scrollX || changed;
            node.scrollX = next;
        }
        if (dy != 0.0f && canScrollY(node)) {
            const float next =
                clampf(node.scrollY + dy, 0.0f, scrollMaxY(node));
            changed = next != node.scrollY || changed;
            node.scrollY = next;
        }
        if (changed && scrolled) {
            *scrolled = &node;
        }
        return changed;
    }

    bool scrollNearest(Node* leaf,
                       float dx,
                       float dy,
                       Node** scrolled = nullptr) {
        for (Node* current = leaf; current; current = current->parent) {
            if (scrollNode(*current, dx, dy, scrolled)) {
                return true;
            }
        }
        return false;
    }

    void finishProgrammaticScroll(const std::vector<Node*>& scrolledNodes) {
        if (scrolledNodes.empty()) {
            return;
        }

        dirty = true;
        if (!options.onElementEvent) {
            return;
        }

        std::vector<ElementEvent> events;
        events.reserve(scrolledNodes.size());
        const Event source;
        for (const Node* node : scrolledNodes) {
            events.push_back(makeElementEvent(
                ElementEventType::Scroll,
                *node,
                source,
                0.0f,
                0.0f));
        }
        for (const ElementEvent& event : events) {
            options.onElementEvent(event);
        }
    }

    void scrollElementIntoView(Node& target, std::vector<Node*>& scrolledNodes) {
        for (Node* ancestor = target.parent; ancestor; ancestor = ancestor->parent) {
            if (!isProgrammaticScrollContainer(*ancestor)) {
                continue;
            }

            const float targetLeft = visualX(target);
            const float targetTop = visualY(target);
            const float viewportLeft = visualX(*ancestor);
            const float viewportTop = visualY(*ancestor);
            const float deltaX = allowsProgrammaticScrollX(*ancestor)
                                     ? nearestVisibilityDelta(
                                           targetLeft,
                                           targetLeft + target.layout.w,
                                           viewportLeft,
                                           viewportLeft + scrollViewportWidth(*ancestor))
                                     : 0.0f;
            const float deltaY = allowsProgrammaticScrollY(*ancestor)
                                     ? nearestVisibilityDelta(
                                           targetTop,
                                           targetTop + target.layout.h,
                                           viewportTop,
                                           viewportTop + scrollViewportHeight(*ancestor))
                                     : 0.0f;
            if (setNodeScrollOffset(
                    *ancestor,
                    ancestor->scrollX + deltaX,
                    ancestor->scrollY + deltaY)) {
                scrolledNodes.push_back(ancestor);
            }
        }
    }

    bool advanceAnimations() {
        if ((activeAnimations.empty() && keyframeAnimations.empty()) ||
            !document.root) {
            return false;
        }

        const float viewportWidth = logicalWidth();
        const float viewportHeight = logicalHeight();
        recomputeStyles(document, options, viewportWidth, viewportHeight);
        clearAnimatedStyleTree(*document.root);
        clearAnimationsForMissingNodes(activeAnimations, document);

        bool changed = false;
        bool layoutChanged = false;
        for (ActiveAnimation& animation : activeAnimations) {
            const float elapsed = static_cast<float>(
                                      animationTimeSeconds -
                                      animation.startSeconds) -
                                  animation.delaySeconds;
            const float progress =
                animation.durationSeconds <= 0.0f
                    ? 1.0f
                    : clampf(elapsed / animation.durationSeconds, 0.0f, 1.0f);
            const float eased = easingValue(animation.easing, progress);
            if (animation.property == TransitionProperty::Height) {
                writeAnimatedHeight(
                    *animation.node,
                    mixLength(animation.fromHeight,
                              animation.toHeight,
                              eased));
                layoutChanged = true;
            } else if (animation.property == TransitionProperty::Opacity) {
                writeAnimatedOpacity(
                    *animation.node,
                    mix(animation.fromOpacity, animation.toOpacity, eased));
            } else if (animation.property == TransitionProperty::Transform) {
                writeAnimatedTransform(
                    *animation.node,
                    mixTransform(animation.fromTransform,
                                 animation.toTransform,
                                 eased));
            }
            changed = true;
        }

        activeAnimations.erase(
            std::remove_if(
                activeAnimations.begin(),
                activeAnimations.end(),
                [this](const ActiveAnimation& animation) {
                    return animationTimeSeconds -
                               animation.startSeconds >=
                           static_cast<double>(
                               animation.delaySeconds +
                               animation.durationSeconds);
                }),
            activeAnimations.end());
        changed = applyKeyframeAnimations(animationTimeSeconds) || changed;
        applyAnimatedStyles(*document.root);
        if (layoutChanged) {
            layoutEngine.layout(document, viewportWidth, viewportHeight);
        }
        if (hasPendingAnimationFrame()) {
            requestAnimationFrame();
        }
        dirty = changed || dirty;
        return changed;
    }

    bool hasPendingAnimationFrame() const {
        return !activeAnimations.empty() || keyframeFramePending;
    }

    void requestAnimationFrame() const {
        if (hasPendingAnimationFrame() && options.requestRedraw) {
            options.requestRedraw();
        }
    }

    void requestLayout() {
        dirty = true;
        layoutPending = true;
        if (updateDepth == 0) {
            flushLayout();
        }
    }

    void flushLayout() {
        if (!layoutPending) {
            return;
        }
        layoutPending = false;
        if (hasDocument) {
            recomputeAndLayout();
        }
        dirty = true;
    }

    bool setFocusedNode(Node* next) {
        if (focusedNode == next) {
            return false;
        }
        if (focusedNode) {
            if (!focusedNode->compositionText.empty()) {
                focusedNode->compositionText.clear();
                markTextChanged(*focusedNode);
            }
            clearInputSelection(*focusedNode);
            focusedNode->editingFocused = false;
            if (Node* host = contentEditableEditingHost(focusedNode)) {
                host->focused = false;
            } else {
                focusedNode->focused = false;
            }
        }
        focusedNode = next;
        if (focusedNode) {
            focusedNode->editingFocused = true;
            if (Node* host = contentEditableEditingHost(focusedNode)) {
                host->focused = true;
            } else {
                focusedNode->focused = true;
            }
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

    SkiaRenderer::TextHitResult selectableHitAtPoint(const Node& node,
                                                     float x,
                                                     float y) {
        const std::string& value = selectableTextValue(node);
        const float layoutX = x - visualX(node) + node.layout.x;
        const float layoutY = y - visualY(node) + node.layout.y;
        return renderer.textHitAtPoint(node, value, layoutX, layoutY);
    }

    size_t selectableIndexAtPoint(const Node& node, float x, float y) {
        return selectableHitAtPoint(node, x, y).index;
    }

    size_t editableIndexAtPoint(const Node& input, float x, float y) {
        if (!isTextareaNode(&input)) {
            return inputIndexAtX(input, x);
        }
        const std::vector<TextLine>& lines = editableLinesFor(input);
        if (lines.empty()) {
            return 0;
        }
        const float lineHeight = editableLineHeight(input);
        const float contentX = visualX(input) + input.resolvedPadding.left;
        const float contentY = visualY(input) + input.resolvedPadding.top;
        const float relativeY = std::max(0.0f, y - contentY + input.scrollY);
        const size_t lineIndex = std::min(lines.size() - 1,
                                          static_cast<size_t>(relativeY / std::max(1.0f, lineHeight)));
        const TextLine line = lines[lineIndex];
        const std::string_view text(input.value.data() + line.start, line.end - line.start);
        const float offset = x - contentX + input.scrollX;
        return line.start + renderer.textIndexAtOffset(text, input.style.fontSize, input.style.fontBold, offset);
    }

    const std::vector<TextLine>& editableLinesFor(const Node& input) {
        EditableLineCacheEntry& entry = editableLineCache[&input];
        if (entry.revision == input.textRevision && entry.size == input.value.size() &&
            !entry.lines.empty()) {
            return entry.lines;
        }

        entry.revision = input.textRevision;
        entry.size = input.value.size();
        buildEditableLines(input.value, entry.lines);
        return entry.lines;
    }

    Node* editingTargetAtPoint(Node* hit, float y) {
        if (Node* target = inputTarget(hit)) {
            return target;
        }
        if (!hit || !isContentEditableEditingHost(*hit)) {
            return nullptr;
        }

        Node* closest = nullptr;
        float closestDistance = std::numeric_limits<float>::max();
        const auto visit = [&](const auto& self, Node& node) -> void {
            if (&node != hit && !isContentEditable(node)) {
                return;
            }
            if (isContentEditableTextNode(node)) {
                const float top = visualY(node);
                const float bottom = top + node.layout.h;
                const float distance = y < top
                    ? top - y
                    : (y > bottom ? y - bottom : 0.0f);
                if (distance < closestDistance) {
                    closest = &node;
                    closestDistance = distance;
                }
                return;
            }
            for (auto& child : node.children) {
                self(self, *child);
            }
        };
        visit(visit, *hit);
        return closest;
    }

    std::string nextContentEditableNodeId(const Node& host) {
        std::string prefix = host.id.empty()
            ? "skui-contenteditable-paragraph"
            : host.id + "-paragraph";
        std::string candidate;
        do {
            candidate = prefix + "-" +
                        std::to_string(++contentEditableNodeSerial);
        } while (document.root && findById(*document.root, candidate));
        return candidate;
    }

    std::unique_ptr<Node> makeTrailingTextNode(const Node& source,
                                               Node& host,
                                               std::string value) {
        auto trailing = std::make_unique<Node>();
        trailing->tag = source.tag;
        trailing->id = nextContentEditableNodeId(host);
        trailing->classes = source.classes;
        trailing->attributes = source.attributes;
        trailing->attributes["id"] = trailing->id;
        trailing->inlineStyle = source.inlineStyle;
        trailing->presentationStyle = source.presentationStyle;
        trailing->value = std::move(value);
        trailing->parent = source.parent;
        syncContentEditablePlaceholder(*trailing);
        return trailing;
    }

    bool splitContentEditableTextNode(
        Node& node,
        std::vector<std::unique_ptr<Node>> insertedNodes = {}) {
        Node* host = contentEditableEditingHost(&node);
        if (!host || !node.parent || host == &node) {
            return false;
        }
        Node* parent = node.parent;
        const std::optional<size_t> index = childIndex(*parent, node);
        if (!index) {
            return false;
        }

        if (!eraseInputSelection(node)) {
            pushInputUndo(node);
        }
        const size_t cursor = std::min(node.cursorIndex, node.value.size());
        std::string trailingValue = node.value.substr(cursor);
        node.value.erase(cursor);
        setInputCursor(node, node.value.size());
        markTextChanged(node);

        std::unique_ptr<Node> trailing =
            makeTrailingTextNode(node, *host, std::move(trailingValue));
        Node* trailingPointer = trailing.get();
        auto position = parent->children.begin() +
                        static_cast<ptrdiff_t>(*index + 1);
        for (auto& inserted : insertedNodes) {
            rebindParents(*inserted, parent);
            prepareContentEditableTree(*inserted);
            position = parent->children.insert(position, std::move(inserted));
            ++position;
        }
        parent->children.insert(position, std::move(trailing));

        setFocusedNode(trailingPointer);
        setInputCursor(*trailingPointer, 0);
        finishDocumentMutation();
        return true;
    }

    bool erasePreviousContentEditableUnit(Node& node) {
        Node* host = contentEditableEditingHost(&node);
        if (!host || hasInputSelection(node) || node.cursorIndex != 0 ||
            !node.parent) {
            return false;
        }
        Node* parent = node.parent;
        const std::optional<size_t> index = childIndex(*parent, node);
        if (!index || *index == 0) {
            return false;
        }
        Node* previous = parent->children[*index - 1].get();
        if (!isContentEditable(*previous)) {
            clearReferencesTo(*previous);
            parent->children.erase(
                parent->children.begin() + static_cast<ptrdiff_t>(*index - 1));
            finishDocumentMutation();
            return true;
        }
        if (!isContentEditableTextNode(*previous) ||
            contentEditableEditingHost(previous) != host) {
            return false;
        }

        pushInputUndo(*previous);
        const size_t joinOffset = previous->value.size();
        previous->value += node.value;
        markTextChanged(*previous);
        setFocusedNode(previous);
        setInputCursor(*previous, joinOffset);
        clearReferencesTo(node);
        parent->children.erase(
            parent->children.begin() + static_cast<ptrdiff_t>(*index));
        finishDocumentMutation();
        return true;
    }

    bool eraseNextContentEditableUnit(Node& node) {
        Node* host = contentEditableEditingHost(&node);
        if (!host || hasInputSelection(node) ||
            node.cursorIndex != node.value.size() || !node.parent) {
            return false;
        }
        Node* parent = node.parent;
        const std::optional<size_t> index = childIndex(*parent, node);
        if (!index || *index + 1 >= parent->children.size()) {
            return false;
        }
        Node* next = parent->children[*index + 1].get();
        if (!isContentEditable(*next)) {
            clearReferencesTo(*next);
            parent->children.erase(
                parent->children.begin() + static_cast<ptrdiff_t>(*index + 1));
            finishDocumentMutation();
            return true;
        }
        if (!isContentEditableTextNode(*next) ||
            contentEditableEditingHost(next) != host) {
            return false;
        }

        pushInputUndo(node);
        node.value += next->value;
        markTextChanged(node);
        clearReferencesTo(*next);
        parent->children.erase(
            parent->children.begin() + static_cast<ptrdiff_t>(*index + 1));
        finishDocumentMutation();
        return true;
    }

    bool moveAcrossContentEditableBoundary(Node& node, bool forward) {
        Node* host = contentEditableEditingHost(&node);
        if (!host || !node.parent || hasInputSelection(node)) {
            return false;
        }
        if ((!forward && node.cursorIndex != 0) ||
            (forward && node.cursorIndex != node.value.size())) {
            return false;
        }
        Node* parent = node.parent;
        const std::optional<size_t> index = childIndex(*parent, node);
        if (!index) {
            return false;
        }
        ptrdiff_t candidate = static_cast<ptrdiff_t>(*index) +
                              (forward ? 1 : -1);
        const ptrdiff_t end = static_cast<ptrdiff_t>(parent->children.size());
        while (candidate >= 0 && candidate < end) {
            Node* sibling = parent->children[static_cast<size_t>(candidate)].get();
            if (isContentEditableTextNode(*sibling) &&
                contentEditableEditingHost(sibling) == host) {
                setFocusedNode(sibling);
                setInputCursor(*sibling, forward ? 0 : sibling->value.size());
                return true;
            }
            candidate += forward ? 1 : -1;
        }
        return false;
    }

    bool loadFragment(std::string_view html,
                      std::vector<std::unique_ptr<Node>>& nodes,
                      std::vector<StyleRule>& rules) {
        std::string error;
        if (!parser.loadFragment(html, document.basePath, nodes, rules, error)) {
            lastError = std::move(error);
            return false;
        }
        if (nodes.empty() && rules.empty()) {
            lastError = "HTML fragment did not contain elements";
            return false;
        }
        return true;
    }

    void appendStyleRules(std::vector<StyleRule> rules) {
        unsigned nextOrder = 0;
        for (const StyleRule& rule : document.rules) {
            nextOrder = std::max(nextOrder, rule.order + 1);
        }
        for (StyleRule& rule : rules) {
            rule.order += nextOrder;
            document.rules.push_back(std::move(rule));
        }
    }

    void clearReferencesTo(const Node& subtree) {
        clearAnimationsForNode(activeAnimations, const_cast<Node*>(&subtree));
        clearKeyframeAnimationsForNode(keyframeAnimations,
                                       const_cast<Node*>(&subtree));
        if (containsNode(subtree, focusedNode)) {
            setFocusedNode(nullptr);
        }
        if (containsNode(subtree, hoveredLeaf)) {
            hoveredLeaf = nullptr;
            currentCursor = Cursor::Default;
        }
        if (containsNode(subtree, pressedLeaf)) {
            pressedLeaf = nullptr;
            mousePressed = false;
            pressedButton = MouseButton::None;
        }
        if (containsNode(subtree, selectingInput)) {
            selectingInput = nullptr;
        }
        if (containsNode(subtree, selectingText)) {
            selectingText = nullptr;
        }
        if (containsNode(subtree, selectedText)) {
            selectedText = nullptr;
        }
        if (containsNode(subtree, scrollingNode)) {
            scrollingNode = nullptr;
            scrollingAxis = ScrollbarAxis::None;
            scrollbarDragOffset = 0.0f;
        }
        for (auto it = editableLineCache.begin(); it != editableLineCache.end();) {
            if (containsNode(subtree, it->first)) {
                it = editableLineCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clearInteractionForDisabledSubtree(const Node& subtree) {
        if (containsNode(subtree, focusedNode)) {
            setFocusedNode(nullptr);
        }
        if (containsNode(subtree, hoveredLeaf)) {
            hoveredLeaf = nullptr;
            currentCursor = Cursor::Default;
        }
        if (containsNode(subtree, pressedLeaf)) {
            pressedLeaf = nullptr;
            mousePressed = false;
            pressedButton = MouseButton::None;
        }
        if (containsNode(subtree, selectingInput)) {
            selectingInput = nullptr;
        }
        if (containsNode(subtree, selectingText)) {
            selectingText = nullptr;
        }
        if (containsNode(subtree, selectedText)) {
            selectedText = nullptr;
        }
        if (containsNode(subtree, scrollingNode)) {
            scrollingNode = nullptr;
            scrollingAxis = ScrollbarAxis::None;
            scrollbarDragOffset = 0.0f;
        }
        if (document.root) {
            std::vector<Node*> hovered;
            std::vector<Node*> active;
            collectChain(hoveredLeaf, hovered);
            collectChain(pressedLeaf, active);
            updateStateTree(*document.root, hovered, active);
        }
    }

    void finishDocumentMutation() {
        renderer.clearNodeCaches();
        editableLineCache.clear();
        if (document.root) {
            std::vector<Node*> hovered;
            std::vector<Node*> active;
            collectChain(hoveredLeaf, hovered);
            collectChain(pressedLeaf, active);
            updateStateTree(*document.root, hovered, active);
        }
        requestLayout();
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
    int updateDepth = 0;
    bool layoutPending = false;
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
    Cursor currentCursor = Cursor::Default;
    std::unordered_map<const Node*, EditableLineCacheEntry> editableLineCache;
    std::vector<ActiveAnimation> activeAnimations;
    std::vector<ActiveKeyframeAnimation> keyframeAnimations;
    double animationTimeSeconds = 0.0;
    bool keyframeFramePending = false;
    std::string lastError;
    size_t contentEditableNodeSerial = 0;
};

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Runtime::~Runtime() {
    if (impl_) {
        impl_->renderer.shutdownCaches();
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
    impl_->renderer.clearCaches();
    impl_->editableLineCache.clear();
    impl_->activeAnimations.clear();
    impl_->keyframeAnimations.clear();
    impl_->keyframeFramePending = false;
    if (impl_->document.root) {
        rebindParents(*impl_->document.root, nullptr);
        prepareContentEditableTree(*impl_->document.root);
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
    impl_->currentCursor = Cursor::Default;
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
    impl_->renderer.clearCaches();
    impl_->editableLineCache.clear();
    impl_->activeAnimations.clear();
    impl_->keyframeAnimations.clear();
    impl_->keyframeFramePending = false;
    if (impl_->document.root) {
        rebindParents(*impl_->document.root, nullptr);
        prepareContentEditableTree(*impl_->document.root);
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
    impl_->currentCursor = Cursor::Default;
    impl_->lastError.clear();
    impl_->recomputeAndLayout();
    return true;
}

void Runtime::resize(int width, int height, float dpiScale) {
    width = std::max(1, width);
    height = std::max(1, height);
    dpiScale = std::max(0.1f, dpiScale);
    if (width == impl_->width && height == impl_->height && dpiScale == impl_->dpiScale) {
        impl_->flushLayout();
        return;
    }

    impl_->width = width;
    impl_->height = height;
    impl_->dpiScale = dpiScale;
    impl_->requestLayout();
}

void Runtime::beginUpdate() {
    ++impl_->updateDepth;
}

void Runtime::endUpdate() {
    if (impl_->updateDepth <= 0) {
        return;
    }
    --impl_->updateDepth;
    if (impl_->updateDepth == 0) {
        impl_->flushLayout();
    }
}

bool Runtime::handleEvent(const Event& event) {
    if (!impl_->hasDocument || !impl_->document.root) {
        return false;
    }

    RuntimeUpdateBatch eventUpdate(*this);
    const float scale = impl_->effectiveScale();
    const float x = event.x / scale;
    const float y = event.y / scale;
    const bool pointerEvent = isPointerEvent(event.type);
    Node* hit = pointerEvent && event.type != EventType::MouseLeave ? hitTest(*impl_->document.root, x, y) : nullptr;
    std::optional<ScrollbarHit> scrollbarHit = pointerEvent && event.type != EventType::MouseLeave
        ? scrollbarHitTest(*impl_->document.root, x, y)
        : std::nullopt;
    const auto cursorAtPoint = [&](Node* leaf) {
        if (const std::optional<Cursor> cursor = explicitCursorForNode(leaf)) {
            return *cursor;
        }
        if (Node* selectable = selectableTextTarget(leaf)) {
            const size_t index = impl_->selectableIndexAtPoint(*selectable, x, y);
            if (selectableLinkAtIndex(*selectable, index)) {
                return Cursor::Pointer;
            }
        }
        return defaultCursorForNode(leaf);
    };
    if (pointerEvent) {
        if (event.type == EventType::MouseLeave) {
            impl_->currentCursor = Cursor::Default;
        } else if (impl_->mousePressed && impl_->pressedLeaf) {
            impl_->currentCursor = cursorForNode(impl_->pressedLeaf);
        } else if (scrollbarHit) {
            impl_->currentCursor = scrollbarHit->axis == ScrollbarAxis::Horizontal ? Cursor::EWResize : Cursor::NSResize;
        } else {
            impl_->currentCursor = cursorAtPoint(hit);
        }
    }
    bool consumed = false;
    bool stateChanged = false;
    bool textChanged = false;
    bool layoutNeeded = false;
    bool scrollChanged = false;
    Node* scrolledNode = nullptr;

    switch (event.type) {
    case EventType::MouseMove: {
        if (impl_->scrollingNode && impl_->mousePressed) {
            const float pointer = impl_->scrollingAxis == ScrollbarAxis::Vertical ? y : x;
            scrollChanged = updateScrollFromScrollbar(*impl_->scrollingNode,
                                                      impl_->scrollingAxis,
                                                      pointer,
                                                      impl_->scrollbarDragOffset) || scrollChanged;
            scrolledNode = scrollChanged ? impl_->scrollingNode : scrolledNode;
            consumed = true;
        } else if (impl_->selectingInput && impl_->mousePressed) {
            const size_t index = impl_->editableIndexAtPoint(*impl_->selectingInput, x, y);
            stateChanged = setInputSelection(*impl_->selectingInput,
                                             impl_->selectingInput->selectionAnchor,
                                             index) || stateChanged;
            consumed = true;
        } else if (impl_->selectingText && impl_->mousePressed) {
            const size_t index = impl_->selectableIndexAtPoint(*impl_->selectingText, x, y);
            stateChanged = setSelectableSelection(*impl_->selectingText,
                                                  impl_->selectingText->selectionAnchor,
                                                  index) || stateChanged;
            consumed = true;
        }
        if (hit != impl_->hoveredLeaf) {
            impl_->hoveredLeaf = hit;
            stateChanged = true;
        }
        Node* actionSource = impl_->mousePressed && impl_->pressedLeaf ? impl_->pressedLeaf : hit;
        if (Node* target = actionTarget(actionSource); target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseMove, *target, event, x, y));
        }
        break;
    }
    case EventType::MouseLeave:
        if (hit != impl_->hoveredLeaf) {
            impl_->hoveredLeaf = hit;
            stateChanged = true;
        }
        consumed = impl_->mousePressed && isPointerConsumingTarget(impl_->pressedLeaf);
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
            scrollChanged = updateScrollFromScrollbar(*impl_->scrollingNode,
                                                      impl_->scrollingAxis,
                                                      pointer,
                                                      impl_->scrollbarDragOffset) || scrollChanged;
            scrolledNode = scrollChanged ? impl_->scrollingNode : scrolledNode;
            consumed = true;
        } else if (Node* input = impl_->editingTargetAtPoint(hit, y)) {
            const bool wasFocused = impl_->focusedNode == input;
            const size_t selectionAnchor = wasFocused
                ? (hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex)
                : input->value.size();
            stateChanged = impl_->setFocusedNode(input) || stateChanged;
            layoutNeeded = true;
            const size_t index = impl_->editableIndexAtPoint(*input, x, y);
            if (event.shiftKey) {
                stateChanged = setInputSelection(*input, selectionAnchor, index) || stateChanged;
            } else {
                stateChanged = setInputCursor(*input, index) || stateChanged;
            }
            impl_->selectingInput = input;
            impl_->selectingText = nullptr;
            if (impl_->selectedText) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
                layoutNeeded = true;
                impl_->selectedText = nullptr;
            }
            stateChanged = true;
            layoutNeeded = true;
        } else if (Node* selectable = selectableTextTarget(hit)) {
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            layoutNeeded = true;
            if (impl_->selectedText && impl_->selectedText != selectable) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
                layoutNeeded = true;
            }
            const size_t selectionAnchor = (event.shiftKey && hasSelectableSelection(*selectable))
                ? selectable->selectionAnchor
                : impl_->selectableIndexAtPoint(*selectable, x, y);
            const size_t index = impl_->selectableIndexAtPoint(*selectable, x, y);
            if (event.shiftKey) {
                setSelectableSelection(*selectable, selectionAnchor, index);
            } else {
                setSelectableCursor(*selectable, index);
            }
            impl_->selectingInput = nullptr;
            impl_->selectingText = selectable;
            impl_->selectedText = selectable;
            stateChanged = true;
            layoutNeeded = true;
        } else {
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            if (impl_->selectedText) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
                layoutNeeded = true;
                impl_->selectedText = nullptr;
            }
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            layoutNeeded = layoutNeeded || stateChanged;
        }
        if (Node* target = mouseEventTarget(hit); target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseDown, *target, event, x, y));
        }
        consumed = consumed || isPointerConsumingTarget(hit);
        stateChanged = true;
        layoutNeeded = true;
        break;
    case EventType::MouseDoubleClick:
        if (event.button == MouseButton::None) {
            return false;
        }
        impl_->mousePressed = true;
        impl_->pressedButton = event.button;
        impl_->pressedLeaf = hit;
        impl_->hoveredLeaf = hit;
        if (Node* input = impl_->editingTargetAtPoint(hit, y)) {
            stateChanged = impl_->setFocusedNode(input) || stateChanged;
            layoutNeeded = true;
            const size_t index = impl_->editableIndexAtPoint(*input, x, y);
            selectInputWordAt(*input, index);
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            consumed = true;
            stateChanged = true;
            layoutNeeded = true;
        } else if (Node* selectable = selectableTextTarget(hit)) {
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            layoutNeeded = true;
            if (impl_->selectedText && impl_->selectedText != selectable) {
                stateChanged = clearSelectableSelection(*impl_->selectedText) || stateChanged;
                layoutNeeded = true;
            }
            const SkiaRenderer::TextHitResult textHit =
                impl_->selectableHitAtPoint(*selectable, x, y);
            if (textHit.insideText) {
                selectSelectableWordAt(*selectable, textHit.index);
            } else {
                selectAllSelectable(*selectable);
            }
            impl_->selectingInput = nullptr;
            impl_->selectingText = nullptr;
            impl_->selectedText = selectable;
            consumed = true;
            stateChanged = true;
            layoutNeeded = true;
        } else {
            consumed = isPointerConsumingTarget(hit);
        }
        if (Node* target = mouseEventTarget(hit); target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseDown, *target, event, x, y));
        }
        stateChanged = true;
        layoutNeeded = true;
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
            stateChanged = setInputSelection(*impl_->selectingInput,
                                             impl_->selectingInput->selectionAnchor,
                                             index) || stateChanged;
            impl_->selectingInput = nullptr;
        }
        if (impl_->selectingText) {
            const size_t index = impl_->selectableIndexAtPoint(*impl_->selectingText, x, y);
            stateChanged = setSelectableSelection(*impl_->selectingText,
                                                  impl_->selectingText->selectionAnchor,
                                                  index) || stateChanged;
            impl_->selectingText = nullptr;
        }
        if (Node* target = releasedAction ? releasedAction : pressedAction; target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseUp, *target, event, x, y));
        } else if (event.button == MouseButton::Right && impl_->options.onElementEvent) {
            Node* selectable = selectableTextTarget(pressed);
            if (selectable && selectable == selectableTextTarget(hit)) {
                impl_->options.onElementEvent(
                    makeElementEvent(ElementEventType::MouseUp,
                                     *selectable,
                                     event,
                                     x,
                                     y));
            }
        }
        if (click) {
            if (impl_->options.onElementEvent) {
                impl_->options.onElementEvent(makeElementEvent(ElementEventType::Click, *pressedAction, event, x, y));
            }
        } else if (event.button == MouseButton::Left) {
            Node* selectable = selectableTextTarget(pressed);
            if (selectable &&
                selectable == selectableTextTarget(hit) &&
                impl_->options.onElementEvent) {
                const bool selectedText =
                    selectable->selectionStart != selectable->selectionEnd;
                const size_t index = impl_->selectableIndexAtPoint(*selectable, x, y);
                if (!selectedText) {
                    if (const Node::TextLink* link = selectableLinkAtIndex(*selectable, index)) {
                        impl_->options.onElementEvent(makeSelectableLinkEvent(*selectable, *link, event, x, y));
                    }
                }
            } else {
                consumed = isPointerConsumingTarget(pressed) || isPointerConsumingTarget(hit);
            }
        }
        consumed = consumed ||
                   isPointerConsumingTarget(hit) ||
                   isPointerConsumingTarget(pressed);
        impl_->mousePressed = false;
        impl_->pressedButton = MouseButton::None;
        impl_->pressedLeaf = nullptr;
        impl_->selectingText = nullptr;
        impl_->scrollingNode = nullptr;
        impl_->scrollingAxis = ScrollbarAxis::None;
        impl_->scrollbarDragOffset = 0.0f;
        impl_->hoveredLeaf = hit;
        impl_->currentCursor = scrollbarHit
            ? (scrollbarHit->axis == ScrollbarAxis::Horizontal ? Cursor::EWResize : Cursor::NSResize)
            : cursorAtPoint(hit);
        stateChanged = true;
        layoutNeeded = true;
        break;
    }
    case EventType::MouseWheel: {
        const float step = event.wheelDelta == 0.0f ? 0.0f : -event.wheelDelta / 120.0f * 48.0f;
        const float dx = event.shiftKey ? step : 0.0f;
        const float dy = event.shiftKey ? 0.0f : step;
        scrollChanged = impl_->scrollNearest(hit, dx, dy, &scrolledNode);
        consumed = scrollChanged || scrollbarHit.has_value() || isPointerConsumingTarget(hit);
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
            if (!textChanged && contentEditableEditingHost(input)) {
                textChanged = impl_->erasePreviousContentEditableUnit(*input);
            }
            consumed = true;
            break;
        case kEnter:
            if (isTextareaNode(input)) {
                textChanged = insertInputText(*input, "\n");
                consumed = true;
                break;
            }
            if (contentEditableEditingHost(input)) {
                textChanged = impl_->splitContentEditableTextNode(*input);
                consumed = true;
                break;
            }
            return false;
        case kDelete:
            textChanged = eraseNextInputChar(*input);
            if (!textChanged && contentEditableEditingHost(input)) {
                textChanged = impl_->eraseNextContentEditableUnit(*input);
            }
            consumed = true;
            break;
        case kLeft: {
            const size_t previous = event.shiftKey
                ? previousUtf8Index(input->value, input->cursorIndex)
                : (hasInputSelection(*input) ? input->selectionStart : previousUtf8Index(input->value, input->cursorIndex));
            if (previous != input->cursorIndex || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    stateChanged = setInputSelection(*input, anchor, previous) || stateChanged;
                } else {
                    stateChanged = setInputCursor(*input, previous) || stateChanged;
                }
            } else if (!event.shiftKey && contentEditableEditingHost(input)) {
                stateChanged =
                    impl_->moveAcrossContentEditableBoundary(*input, false) ||
                    stateChanged;
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
                    stateChanged = setInputSelection(*input, anchor, next) || stateChanged;
                } else {
                    stateChanged = setInputCursor(*input, next) || stateChanged;
                }
            } else if (!event.shiftKey && contentEditableEditingHost(input)) {
                stateChanged =
                    impl_->moveAcrossContentEditableBoundary(*input, true) ||
                    stateChanged;
            }
            consumed = true;
            break;
        }
        case kHome:
            if (const size_t target = isTextareaNode(input) ? currentLineStart(input->value, input->cursorIndex) : 0;
                input->cursorIndex != target || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    stateChanged = setInputSelection(*input, anchor, target) || stateChanged;
                } else {
                    stateChanged = setInputCursor(*input, target) || stateChanged;
                }
            }
            consumed = true;
            break;
        case kEnd:
            if (const size_t target = isTextareaNode(input) ? currentLineEnd(input->value, input->cursorIndex) : input->value.size();
                input->cursorIndex != target || hasInputSelection(*input)) {
                if (event.shiftKey) {
                    const size_t anchor = hasInputSelection(*input) ? input->selectionAnchor : input->cursorIndex;
                    stateChanged = setInputSelection(*input, anchor, target) || stateChanged;
                } else {
                    stateChanged = setInputCursor(*input, target) || stateChanged;
                }
            }
            consumed = true;
            break;
        case kEscape:
            stateChanged = impl_->setFocusedNode(nullptr) || stateChanged;
            layoutNeeded = layoutNeeded || stateChanged;
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
        layoutNeeded = textChanged;
        consumed = true;
        break;
    case EventType::ImeComposition:
        if (!isEditableNode(impl_->focusedNode)) {
            return false;
        }
        if (impl_->focusedNode->compositionText != event.text) {
            impl_->focusedNode->compositionText = event.text;
            markTextChanged(*impl_->focusedNode);
            stateChanged = true;
            layoutNeeded = true;
        }
        consumed = true;
        break;
    case EventType::ImeEnd:
        if (!isEditableNode(impl_->focusedNode)) {
            return false;
        }
        if (!impl_->focusedNode->compositionText.empty()) {
            impl_->focusedNode->compositionText.clear();
            markTextChanged(*impl_->focusedNode);
            stateChanged = true;
            layoutNeeded = true;
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
            layoutNeeded = true;
        }
    }
    if (textChanged && impl_->focusedNode && impl_->options.onElementEvent) {
        Node* target = inputEventTarget(*impl_->focusedNode);
        impl_->options.onElementEvent(
            makeElementEvent(ElementEventType::Input, *target, event, x, y));
    }
    if (scrollChanged && scrolledNode && impl_->options.onElementEvent) {
        impl_->options.onElementEvent(makeElementEvent(ElementEventType::Scroll, *scrolledNode, event, x, y));
    }
    layoutNeeded = layoutNeeded || textChanged;
    if ((stateChanged || textChanged) && layoutNeeded) {
        impl_->requestLayout();
    } else if (scrollChanged || stateChanged || textChanged) {
        impl_->dirty = true;
    }
    return consumed;
}

bool Runtime::tick(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f) {
        return impl_->hasPendingAnimationFrame();
    }

    impl_->animationTimeSeconds += static_cast<double>(deltaSeconds);
    impl_->advanceAnimations();
    return impl_->hasPendingAnimationFrame();
}

void Runtime::render(SkCanvas& canvas) {
    const auto traceStart = perf::Trace::now();
    if (!impl_->hasDocument || !impl_->document.root) {
        canvas.clear(impl_->options.clearColor);
        return;
    }

    impl_->renderer.draw(impl_->document, canvas, impl_->width, impl_->height, impl_->effectiveScale());
    impl_->dirty = false;
    perf::Trace::write("skui", "runtime_render", impl_->width, impl_->height, perf::Trace::elapsedMs(traceStart));
}

void Runtime::setScale(float scale) {
    scale = std::max(0.1f, scale);
    if (std::abs(scale - impl_->options.scale) <= 0.001f) {
        impl_->flushLayout();
        return;
    }

    impl_->options.scale = scale;
    impl_->requestLayout();
}

void Runtime::setTextScale(float textScale) {
    textScale = std::max(0.1f, textScale);
    if (std::abs(textScale - impl_->options.textScale) <= 0.001f) {
        impl_->flushLayout();
        return;
    }

    impl_->options.textScale = textScale;
    impl_->requestLayout();
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
    impl_->requestLayout();
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
    impl_->requestLayout();
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
    impl_->requestLayout();
    return true;
}

bool Runtime::setStylesById(const std::vector<StyleUpdate>& updates) {
    return applyUpdates({updates, {}, {}});
}

bool Runtime::setTextById(std::string_view id, std::string_view text) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    if (isContentEditableTextNode(*node)) {
        node->value = std::string(text);
        node->text.clear();
        clampInputCursor(*node);
    } else {
        node->text = std::string(text);
    }
    markTextChanged(*node);
    impl_->requestLayout();
    return true;
}

bool Runtime::setValueById(std::string_view id, std::string_view value) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!isEditableNode(node) && !isSelectableTextNode(node)) {
        return false;
    }

    if (isSelectableTextNode(node) && !isEditableNode(node)) {
        std::string nextValue(value);
        if (node->value == nextValue) {
            return true;
        }

        node->value = std::move(nextValue);
        markTextChanged(*node);
        node->cursorIndex = std::min(node->cursorIndex, node->value.size());
        node->selectionAnchor = std::min(node->selectionAnchor, node->value.size());
        node->selectionStart = std::min(node->selectionStart, node->value.size());
        node->selectionEnd = std::min(node->selectionEnd, node->value.size());
        impl_->requestLayout();
        return true;
    }

    std::string normalizedValue = editableText(*node, std::string(value));
    if (node->value == normalizedValue) {
        return true;
    }

    node->value = std::move(normalizedValue);
    markTextChanged(*node);
    node->numericValue = 0.0f;
    const char* begin = node->value.data();
    const char* end = node->value.data() + node->value.size();
    std::from_chars(begin, end, node->numericValue);
    node->cursorIndex = std::min(node->cursorIndex, node->value.size());
    node->selectionAnchor = std::min(node->selectionAnchor, node->value.size());
    node->selectionStart = std::min(node->selectionStart, node->value.size());
    node->selectionEnd = std::min(node->selectionEnd, node->value.size());
    node->undoStack.clear();
    clampInputCursor(*node);
    impl_->requestLayout();
    return true;
}

bool Runtime::setTextsById(const std::vector<TextUpdate>& updates) {
    return applyUpdates({{}, updates, {}});
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
    if (normalizedName == "contenteditable") {
        prepareContentEditableTree(*node);
    }
    if (normalizedName == "disabled") {
        impl_->clearInteractionForDisabledSubtree(*node);
    }
    impl_->requestLayout();
    return true;
}

bool Runtime::setAttributesById(const std::vector<AttributeUpdate>& updates) {
    return applyUpdates({{}, {}, updates});
}

bool Runtime::applyUpdates(const RuntimeUpdates& updates) {
    if (!impl_->hasDocument || !impl_->document.root ||
        (updates.styles.empty() && updates.texts.empty() && updates.attributes.empty())) {
        return false;
    }
    bool changed = false;

    for (const StyleUpdate& update : updates.styles) {
        if (update.id.empty()) {
            continue;
        }
        Node* node = findById(*impl_->document.root, update.id);
        if (!node) {
            continue;
        }
        node->inlineStyle = {};
        parseInlineStyle(update.declarations, node->inlineStyle);
        node->attributes["style"] = update.declarations;
        changed = true;
    }

    for (const TextUpdate& update : updates.texts) {
        if (update.id.empty()) {
            continue;
        }
        Node* node = findById(*impl_->document.root, update.id);
        if (!node) {
            continue;
        }
        if (isContentEditableTextNode(*node)) {
            node->value = update.text;
            node->text.clear();
            clampInputCursor(*node);
        } else {
            node->text = update.text;
        }
        markTextChanged(*node);
        changed = true;
    }

    for (const AttributeUpdate& update : updates.attributes) {
        if (update.id.empty() || update.name.empty()) {
            continue;
        }
        Node* node = findById(*impl_->document.root, update.id);
        if (!node) {
            continue;
        }
        const std::string normalizedName = lowerAscii(trim(update.name));
        if (normalizedName.empty()) {
            continue;
        }
        node->attributes[normalizedName] = update.value;
        syncNodeAttribute(*node, normalizedName);
        if (normalizedName == "contenteditable") {
            prepareContentEditableTree(*node);
        }
        if (normalizedName == "disabled") {
            impl_->clearInteractionForDisabledSubtree(*node);
        }
        changed = true;
    }
    if (!changed) {
        return false;
    }
    impl_->requestLayout();
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
    if (normalizedName == "contenteditable") {
        prepareContentEditableTree(*node);
    }
    impl_->requestLayout();
    return true;
}

bool Runtime::appendHtmlById(std::string_view parentId, std::string_view html) {
    if (!impl_->hasDocument || !impl_->document.root || parentId.empty() || html.empty()) {
        return false;
    }
    Node* parent = findById(*impl_->document.root, parentId);
    if (!parent) {
        return false;
    }

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<StyleRule> rules;
    if (!impl_->loadFragment(html, nodes, rules)) {
        return false;
    }
    for (auto& node : nodes) {
        rebindParents(*node, parent);
        prepareContentEditableTree(*node);
        parent->children.push_back(std::move(node));
    }
    impl_->appendStyleRules(std::move(rules));
    impl_->lastError.clear();
    impl_->finishDocumentMutation();
    return true;
}

bool Runtime::prependHtmlById(std::string_view parentId, std::string_view html) {
    if (!impl_->hasDocument || !impl_->document.root || parentId.empty() || html.empty()) {
        return false;
    }
    Node* parent = findById(*impl_->document.root, parentId);
    if (!parent) {
        return false;
    }

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<StyleRule> rules;
    if (!impl_->loadFragment(html, nodes, rules)) {
        return false;
    }
    const auto insertAt = parent->children.begin();
    auto current = insertAt;
    for (auto& node : nodes) {
        rebindParents(*node, parent);
        prepareContentEditableTree(*node);
        current = parent->children.insert(current, std::move(node));
        ++current;
    }
    impl_->appendStyleRules(std::move(rules));
    impl_->lastError.clear();
    impl_->finishDocumentMutation();
    return true;
}

bool Runtime::replaceHtmlById(std::string_view id, std::string_view html) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() || html.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node || !node->parent) {
        return false;
    }
    Node* parent = node->parent;
    const std::optional<size_t> index = childIndex(*parent, *node);
    if (!index) {
        return false;
    }

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<StyleRule> rules;
    if (!impl_->loadFragment(html, nodes, rules)) {
        return false;
    }
    impl_->clearReferencesTo(*node);
    auto insertAt = parent->children.erase(parent->children.begin() + static_cast<ptrdiff_t>(*index));
    for (auto& replacement : nodes) {
        rebindParents(*replacement, parent);
        prepareContentEditableTree(*replacement);
        insertAt = parent->children.insert(insertAt, std::move(replacement));
        ++insertAt;
    }
    impl_->appendStyleRules(std::move(rules));
    impl_->lastError.clear();
    impl_->finishDocumentMutation();
    return true;
}

bool Runtime::removeElementById(std::string_view id) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node || !node->parent) {
        return false;
    }
    Node* parent = node->parent;
    const std::optional<size_t> index = childIndex(*parent, *node);
    if (!index) {
        return false;
    }

    impl_->clearReferencesTo(*node);
    parent->children.erase(parent->children.begin() + static_cast<ptrdiff_t>(*index));
    impl_->lastError.clear();
    impl_->finishDocumentMutation();
    return true;
}

bool Runtime::insertHtmlAtSelection(std::string_view editingHostId,
                                    std::string_view html) {
    if (!impl_->hasDocument || !impl_->document.root ||
        editingHostId.empty() || html.empty()) {
        return false;
    }
    Node* host = findById(*impl_->document.root, editingHostId);
    if (!host || !isContentEditableEditingHost(*host)) {
        impl_->lastError = "insertHtmlAtSelection target is not an editing host";
        return false;
    }

    Node* target = impl_->focusedNode;
    if (!target || contentEditableEditingHost(target) != host) {
        target = impl_->editingTargetAtPoint(
            host,
            Impl::visualY(*host) + host->layout.h);
    }
    if (!target || target == host || !target->parent) {
        impl_->lastError =
            "insertHtmlAtSelection requires a text container inside the editing host";
        return false;
    }

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<StyleRule> rules;
    if (!impl_->loadFragment(html, nodes, rules) || nodes.empty()) {
        return false;
    }
    impl_->appendStyleRules(std::move(rules));
    if (!impl_->splitContentEditableTextNode(*target, std::move(nodes))) {
        impl_->lastError = "failed to insert HTML at the current selection";
        return false;
    }
    impl_->lastError.clear();
    return true;
}

bool Runtime::collapseSelection(std::string_view nodeId, size_t offset) {
    if (!impl_->hasDocument || !impl_->document.root || nodeId.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, nodeId);
    if (!isEditableNode(node)) {
        return false;
    }
    const bool focusChanged = impl_->setFocusedNode(node);
    const bool selectionChanged = setInputCursor(*node, offset);
    if (focusChanged || selectionChanged) {
        impl_->requestLayout();
    }
    return true;
}

bool Runtime::setSelectionBaseAndExtent(std::string_view anchorNodeId,
                                        size_t anchorOffset,
                                        std::string_view focusNodeId,
                                        size_t focusOffset) {
    if (!impl_->hasDocument || !impl_->document.root ||
        anchorNodeId.empty() || anchorNodeId != focusNodeId) {
        return false;
    }
    Node* node = findById(*impl_->document.root, anchorNodeId);
    if (!isEditableNode(node)) {
        return false;
    }
    const bool focusChanged = impl_->setFocusedNode(node);
    const bool selectionChanged =
        setInputSelection(*node, anchorOffset, focusOffset);
    if (focusChanged || selectionChanged) {
        impl_->requestLayout();
    }
    return true;
}

bool Runtime::setVisibleById(std::string_view id, bool visible) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    const std::string nextStyle = styleWithDisplay(node->attributes["style"], visible);
    node->attributes["style"] = nextStyle;
    syncNodeAttribute(*node, "style");
    impl_->lastError.clear();
    impl_->finishDocumentMutation();
    return true;
}

bool Runtime::setConsumesEventsById(std::string_view id, bool consumesEvents) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }
    const std::string nextStyle = styleWithPointerEvents(node->attributes["style"], consumesEvents);
    node->attributes["style"] = nextStyle;
    syncNodeAttribute(*node, "style");
    impl_->lastError.clear();
    impl_->finishDocumentMutation();
    return true;
}

bool Runtime::setScrollOffsetById(std::string_view id, float scrollX, float scrollY) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() ||
        !std::isfinite(scrollX) || !std::isfinite(scrollY)) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node || !isProgrammaticScrollContainer(*node)) {
        return false;
    }

    std::vector<Node*> scrolledNodes;
    if (setNodeScrollOffset(*node, scrollX, scrollY)) {
        scrolledNodes.push_back(node);
    }
    impl_->finishProgrammaticScroll(scrolledNodes);
    return true;
}

bool Runtime::scrollById(std::string_view id, float deltaX, float deltaY) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty() ||
        !std::isfinite(deltaX) || !std::isfinite(deltaY)) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node || !isProgrammaticScrollContainer(*node)) {
        return false;
    }

    std::vector<Node*> scrolledNodes;
    if (setNodeScrollOffset(
            *node,
            node->scrollX + deltaX,
            node->scrollY + deltaY)) {
        scrolledNodes.push_back(node);
    }
    impl_->finishProgrammaticScroll(scrolledNodes);
    return true;
}

bool Runtime::scrollIntoViewById(std::string_view id) {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return false;
    }
    Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return false;
    }

    std::vector<Node*> scrolledNodes;
    impl_->scrollElementIntoView(*node, scrolledNodes);
    impl_->finishProgrammaticScroll(scrolledNodes);
    return true;
}

std::optional<ScrollState> Runtime::scrollStateById(std::string_view id) const {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return std::nullopt;
    }
    const Node* node = findById(*impl_->document.root, id);
    if (!node || !isProgrammaticScrollContainer(*node)) {
        return std::nullopt;
    }
    return scrollStateForNode(*node);
}

std::optional<std::string> Runtime::textContentById(std::string_view id) const {
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return std::nullopt;
    }
    const Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return std::nullopt;
    }
    return textContent(*node);
}

std::vector<std::string> Runtime::childElementIdsById(
    std::string_view id) const {
    std::vector<std::string> ids;
    if (!impl_->hasDocument || !impl_->document.root || id.empty()) {
        return ids;
    }
    const Node* node = findById(*impl_->document.root, id);
    if (!node) {
        return ids;
    }
    ids.reserve(node->children.size());
    for (const auto& child : node->children) {
        ids.push_back(child->id);
    }
    return ids;
}

Selection Runtime::selection() const {
    Selection result;
    const Node* node = impl_->focusedNode;
    if (!isEditableNode(node)) {
        return result;
    }
    result.anchorNodeId = node->id;
    result.anchorOffset = node->selectionAnchor;
    result.focusNodeId = node->id;
    result.focusOffset = node->cursorIndex;
    result.rangeCount = 1;
    Range range;
    range.startContainerId = node->id;
    range.startOffset = node->selectionStart;
    range.endContainerId = node->id;
    range.endOffset = node->selectionEnd;
    range.collapsed = node->selectionStart == node->selectionEnd;
    result.range = std::move(range);
    return result;
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

float Runtime::scale() const {
    return impl_->userScale();
}

float Runtime::textScale() const {
    return impl_->textScale();
}

float Runtime::effectiveScale() const {
    return impl_->effectiveScale();
}

Cursor Runtime::cursor() const {
    return impl_->currentCursor;
}

bool Runtime::dirty() const {
    if (impl_->renderer.consumeImageDirty()) {
        impl_->dirty = true;
    }
    return impl_->dirty;
}

std::string Runtime::lastError() const {
    return impl_->lastError;
}

void Runtime::clearDirty() {
    impl_->dirty = false;
}

}  // namespace skui
