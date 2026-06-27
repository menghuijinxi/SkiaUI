#include "skui_internal.h"

#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace skui {
namespace {

void rebindParents(Node& node, Node* parent) {
    node.parent = parent;
    for (auto& child : node.children) {
        rebindParents(*child, &node);
    }
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
    if (event.type == EventType::MouseMove) {
        // 第一版只保留事件入口，后续在这里接 hover/active/focus。
        return false;
    }
    return false;
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

bool Runtime::renderToBgraPixels(uint32_t* pixels, int width, int height, size_t rowBytes, float dpiScale) {
    width = std::max(1, width);
    height = std::max(1, height);
    dpiScale = std::max(0.1f, dpiScale);
    if (!pixels || rowBytes < static_cast<size_t>(width) * sizeof(uint32_t)) {
        impl_->lastError = "renderToBgraPixels 参数无效";
        return false;
    }

    const SkImageInfo info = SkImageInfo::Make(width,
                                               height,
                                               kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
    if (!surface) {
        impl_->lastError = "Skia 无法创建离屏 Surface";
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
