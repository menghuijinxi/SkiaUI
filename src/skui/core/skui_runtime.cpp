#include "skui_internal.h"

#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cstdint>
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
    for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
        if (Node* hit = hitTest(**it, x, y)) {
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

Node* actionTarget(Node* leaf) {
    for (Node* current = leaf; current; current = current->parent) {
        if (!current->action.empty()) {
            return current;
        }
    }
    return nullptr;
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

}  // namespace

class Runtime::Impl {
public:
    explicit Impl(RuntimeOptions runtimeOptions)
        : options(std::move(runtimeOptions)), parser(options), renderer(options) {}

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
    impl_->lastError.clear();
    impl_->layoutEngine.layout(impl_->document,
                               static_cast<float>(impl_->width) / std::max(0.1f, impl_->dpiScale),
                               static_cast<float>(impl_->height) / std::max(0.1f, impl_->dpiScale));
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
    impl_->lastError.clear();
    impl_->layoutEngine.layout(impl_->document,
                               static_cast<float>(impl_->width) / std::max(0.1f, impl_->dpiScale),
                               static_cast<float>(impl_->height) / std::max(0.1f, impl_->dpiScale));
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
        impl_->layoutEngine.layout(impl_->document,
                                   static_cast<float>(impl_->width) / impl_->dpiScale,
                                   static_cast<float>(impl_->height) / impl_->dpiScale);
    }
}

bool Runtime::handleEvent(const Event& event) {
    if (!impl_->hasDocument || !impl_->document.root) {
        return false;
    }

    const float scale = std::max(0.1f, impl_->dpiScale);
    const float x = event.x / scale;
    const float y = event.y / scale;
    Node* hit = event.type == EventType::MouseLeave ? nullptr : hitTest(*impl_->document.root, x, y);
    bool consumed = false;
    bool stateChanged = false;

    switch (event.type) {
    case EventType::MouseMove:
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
        if (Node* target = actionTarget(hit); target && impl_->options.onElementEvent) {
            impl_->options.onElementEvent(makeElementEvent(ElementEventType::MouseDown, *target, event, x, y));
        }
        consumed = hit != nullptr;
        stateChanged = true;
        break;
    case EventType::MouseUp: {
        if (event.button == MouseButton::None) {
            return false;
        }
        Node* pressed = impl_->pressedLeaf;
        Node* pressedAction = actionTarget(pressed);
        Node* releasedAction = actionTarget(hit);
        const bool click = pressedAction && pressedAction == releasedAction && event.button == impl_->pressedButton;
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
        impl_->hoveredLeaf = hit;
        stateChanged = true;
        break;
    }
    default:
        return false;
    }

    std::vector<Node*> hovered;
    std::vector<Node*> active;
    collectChain(impl_->hoveredLeaf, hovered);
    if (impl_->mousePressed && impl_->pressedLeaf) {
        collectChain(impl_->pressedLeaf, active);
    }
    if (updateStateTree(*impl_->document.root, hovered, active)) {
        stateChanged = true;
    }
    if (stateChanged) {
        recomputeStyles(impl_->document, impl_->options);
        impl_->layoutEngine.layout(impl_->document,
                                   static_cast<float>(impl_->width) / impl_->dpiScale,
                                   static_cast<float>(impl_->height) / impl_->dpiScale);
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
    recomputeStyles(impl_->document, impl_->options);
    impl_->layoutEngine.layout(impl_->document,
                               static_cast<float>(impl_->width) / impl_->dpiScale,
                               static_cast<float>(impl_->height) / impl_->dpiScale);
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
    recomputeStyles(impl_->document, impl_->options);
    impl_->layoutEngine.layout(impl_->document,
                               static_cast<float>(impl_->width) / impl_->dpiScale,
                               static_cast<float>(impl_->height) / impl_->dpiScale);
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
