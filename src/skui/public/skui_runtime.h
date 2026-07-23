#pragma once

#include "skui_media.h"

#include "include/core/SkColor.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class SkCanvas;

namespace skui {

struct Theme {
    SkColor text = SkColorSetARGB(255, 239, 247, 253);
    SkColor mutedText = SkColorSetARGB(210, 192, 207, 220);
    SkColor accent = SkColorSetARGB(255, 48, 234, 220);

    static Theme dark();
};

enum class EventType {
    None,
    MouseMove,
    MouseLeave,
    MouseDown,
    MouseUp,
    MouseDoubleClick,
    MouseWheel,
    KeyDown,
    KeyUp,
    TextInput,
    ImeComposition,
    ImeEnd
};

enum class MouseButton {
    None,
    Left,
    Middle,
    Right
};

enum class Cursor {
    Auto,
    Default,
    Pointer,
    Text,
    EWResize,
    NSResize,
    Move,
    Crosshair,
    NotAllowed
};

enum class ElementEventType {
    MouseDown,
    MouseMove,
    MouseUp,
    Click,
    KeyDown,
    Input,
    Scroll
};

struct ElementEvent {
    ElementEventType type = ElementEventType::Click;
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::string action;
    std::string text;
    std::string value;
    float x = 0.0f;
    float y = 0.0f;
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    MouseButton button = MouseButton::None;
    unsigned key = 0;
    bool shiftKey = false;
    bool ctrlKey = false;
};

enum class ClipboardItemType {
    Text,
    Image,
    File
};

struct ClipboardItem {
    ClipboardItemType type = ClipboardItemType::Text;
    std::string text;
    std::string source;
};

struct ClipboardContent {
    std::string text;
    std::string html;
    std::vector<std::string> filePaths;
    std::vector<ClipboardItem> items;
};

using ElementEventCallback = std::function<void(const ElementEvent&)>;
using ElementKeyDownCallback = std::function<bool(const ElementEvent&)>;
using ClipboardReadCallback = std::function<std::string()>;
using ClipboardWriteCallback = std::function<void(std::string_view)>;
using ClipboardContentReadCallback = std::function<ClipboardContent()>;
using ClipboardContentWriteCallback =
    std::function<void(const ClipboardContent&)>;
using RequestRedrawCallback = std::function<void()>;

struct StyleUpdate {
    std::string id;
    std::string declarations;
};

struct TextUpdate {
    std::string id;
    std::string text;
};

struct AttributeUpdate {
    std::string id;
    std::string name;
    std::string value;
};

struct RuntimeUpdates {
    std::vector<StyleUpdate> styles;
    std::vector<TextUpdate> texts;
    std::vector<AttributeUpdate> attributes;
};

struct BitmapMemoryStats {
    size_t budgetBytes = 0;
    size_t cacheBytes = 0;
    size_t peakCacheBytes = 0;
    size_t displayedBytes = 0;
    size_t cacheEntryCount = 0;
    size_t displayedImageCount = 0;
    size_t loadingImageCount = 0;
    size_t readyImageCount = 0;
    size_t failedImageCount = 0;
    uint64_t cacheHitCount = 0;
    uint64_t cacheMissCount = 0;
    uint64_t decodeCount = 0;
    uint64_t evictionCount = 0;
};

struct MemoryStats {
    BitmapMemoryStats bitmapImages;
    size_t domNodeCount = 0;
    size_t textCacheEntryCount = 0;
    size_t textLineCacheEntryCount = 0;
    size_t svgFileCacheEntryCount = 0;
    size_t svgDomCacheEntryCount = 0;
};

struct ScrollState {
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    float maxScrollX = 0.0f;
    float maxScrollY = 0.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
};

struct Range {
    std::string startContainerId;
    size_t startOffset = 0;
    std::string endContainerId;
    size_t endOffset = 0;
    bool collapsed = true;
};

struct Selection {
    std::string anchorNodeId;
    size_t anchorOffset = 0;
    std::string focusNodeId;
    size_t focusOffset = 0;
    size_t rangeCount = 0;
    std::optional<Range> range;
};

struct RuntimeOptions {
    std::string assetRoot;
    float scale = 1.0f;
    float textScale = 1.0f;
    SkColor clearColor = SkColorSetRGB(7, 12, 18);
    size_t bitmapCacheBudgetBytes = 192u * 1024u * 1024u;
    size_t bitmapLoadWorkerCount = 4;
    float lazyImagePreloadMarginViewports = 1.0f;
    size_t videoPredecodeFrames = 3;
    Theme theme = Theme::dark();
    MediaPlayerFactory mediaPlayerFactory;
    ElementEventCallback onElementEvent;
    ElementKeyDownCallback onElementKeyDown;
    ClipboardReadCallback readClipboardText;
    ClipboardWriteCallback writeClipboardText;
    ClipboardContentReadCallback readClipboardContent;
    ClipboardContentWriteCallback writeClipboardContent;
    RequestRedrawCallback requestRedraw;
};

struct Event {
    EventType type = EventType::None;
    float x = 0.0f;
    float y = 0.0f;
    float wheelDelta = 0.0f;
    MouseButton button = MouseButton::None;
    unsigned key = 0;
    bool shiftKey = false;
    bool ctrlKey = false;
    std::string text;
};

struct RuntimeRenderRegion {
    float x = 0.0f;
    float y = 0.0f;
    bool drawBackground = true;
};

enum class DocumentType {
    Page,
    Layout
};

struct LayoutRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct LayoutTransform {
    float m11 = 1.0f;
    float m12 = 0.0f;
    float m21 = 0.0f;
    float m22 = 1.0f;
    float translationX = 0.0f;
    float translationY = 0.0f;
};

struct LayoutPageSnapshot {
    std::string id;
    std::string source;
    std::string resolvedSource;
    LayoutRect rect;
    LayoutTransform transform;
    std::optional<LayoutRect> clipRect;
    float opacity = 1.0f;
    int zIndex = 0;
    size_t paintOrder = 0;
    bool visible = true;
    bool hitTestable = true;
};

class RendererBackend {
public:
    virtual ~RendererBackend() = default;
    virtual void render(SkCanvas& canvas, int width, int height, float dpiScale) = 0;
};

class Runtime {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool loadDocument(const std::string& path);
    bool loadDocument(const std::string& path, DocumentType expectedType);
    bool loadDocumentFromString(std::string_view html, std::string_view basePath = {});
    bool loadDocumentFromString(std::string_view html,
                                DocumentType expectedType,
                                std::string_view basePath = {});
    void resize(int width, int height, float dpiScale);
    void beginUpdate();
    void endUpdate();
    bool handleEvent(const Event& event);
    [[nodiscard]] bool tick(float deltaSeconds);
    void render(SkCanvas& canvas);
    void renderInto(SkCanvas& canvas, const RuntimeRenderRegion& region);
    bool renderToBgraPixels(uint32_t* pixels,
                            int width,
                            int height,
                            size_t rowBytes,
                            float dpiScale);
    void setScale(float scale);
    void setTextScale(float textScale);
    bool addClassById(std::string_view id, std::string_view className);
    bool removeClassById(std::string_view id, std::string_view className);
    bool setStyleById(std::string_view id, std::string_view declarations);
    bool setStylesById(const std::vector<StyleUpdate>& updates);
    bool setTextById(std::string_view id, std::string_view text);
    bool setValueById(std::string_view id, std::string_view value);
    bool setTextsById(const std::vector<TextUpdate>& updates);
    bool setAttributeById(std::string_view id, std::string_view name, std::string_view value);
    bool setAttributesById(const std::vector<AttributeUpdate>& updates);
    bool applyUpdates(const RuntimeUpdates& updates);
    bool removeAttributeById(std::string_view id, std::string_view name);
    bool appendHtmlById(std::string_view parentId, std::string_view html);
    bool prependHtmlById(std::string_view parentId, std::string_view html);
    bool replaceHtmlById(std::string_view id, std::string_view html);
    bool removeElementById(std::string_view id);
    bool prepareVideoById(std::string_view id);
    bool playVideoById(std::string_view id);
    bool pauseVideoById(std::string_view id);
    bool seekVideoById(std::string_view id, double seconds);
    bool setVideoMutedById(std::string_view id, bool muted);
    [[nodiscard]] std::optional<MediaPlaybackState> videoStateById(
        std::string_view id) const;
    bool insertHtmlAtSelection(std::string_view editingHostId,
                               std::string_view html);
    bool collapseSelection(std::string_view nodeId, size_t offset);
    bool setSelectionBaseAndExtent(std::string_view anchorNodeId,
                                   size_t anchorOffset,
                                   std::string_view focusNodeId,
                                   size_t focusOffset);
    bool setVisibleById(std::string_view id, bool visible);
    bool setConsumesEventsById(std::string_view id, bool consumesEvents);
    bool setScrollOffsetById(std::string_view id, float scrollX, float scrollY);
    bool scrollById(std::string_view id, float deltaX, float deltaY);
    bool scrollIntoViewById(std::string_view id);
    [[nodiscard]] std::optional<ScrollState> scrollStateById(std::string_view id) const;
    [[nodiscard]] std::optional<std::string> textContentById(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> childElementIdsById(std::string_view id) const;
    [[nodiscard]] Selection selection() const;
    [[nodiscard]] ClipboardContent readClipboardContent();
    [[nodiscard]] bool hasClassById(std::string_view id, std::string_view className) const;
    void setElementEventCallback(ElementEventCallback callback);
    void setElementKeyDownCallback(ElementKeyDownCallback callback);

    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] float dpiScale() const;
    [[nodiscard]] float scale() const;
    [[nodiscard]] float textScale() const;
    [[nodiscard]] float effectiveScale() const;
    [[nodiscard]] MemoryStats memoryStats() const;
    [[nodiscard]] std::string memoryReport() const;
    [[nodiscard]] Cursor cursor() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] std::optional<DocumentType> documentType() const;
    [[nodiscard]] std::vector<LayoutPageSnapshot> layoutPageSnapshots() const;
    [[nodiscard]] std::string lastError() const;
    void clearDirty();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace skui
