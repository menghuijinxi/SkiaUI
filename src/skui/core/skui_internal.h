#pragma once

#include "skui_runtime.h"

#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkImage.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include <yoga/Yoga.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

class SkSVGDOM;
namespace skui {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    [[nodiscard]] SkRect sk() const {
        return SkRect::MakeXYWH(x, y, w, h);
    }

    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && py >= y && px <= x + w && py <= y + h;
    }
};

enum class Display {
    Flex,
    None
};

enum class Visibility {
    Visible,
    Hidden
};

enum class Position {
    Relative,
    Absolute,
    Sticky
};

enum class GradientKind {
    None,
    LinearX,
    LinearY,
    Radial
};

enum class BorderStyle {
    None,
    Solid
};

enum class Overflow {
    Visible,
    Hidden,
    Auto,
    Scroll
};

enum class PointerEvents {
    Auto,
    None
};

enum class LengthUnit {
    Px,
    Percent,
    Auto
};

enum class TransitionProperty {
    All,
    Opacity,
    Transform
};

enum class Easing {
    Linear,
    Ease,
    EaseIn,
    EaseOut,
    EaseInOut
};

struct Length {
    float value = 0.0f;
    LengthUnit unit = LengthUnit::Px;
};

struct Gradient {
    GradientKind kind = GradientKind::None;
    std::vector<SkColor> colors;
    std::vector<float> positions;
};

struct EdgeValues {
    std::optional<Length> left;
    std::optional<Length> top;
    std::optional<Length> right;
    std::optional<Length> bottom;
};

struct CornerRadii {
    float topLeft = 0.0f;
    float topRight = 0.0f;
    float bottomRight = 0.0f;
    float bottomLeft = 0.0f;

    [[nodiscard]] bool any() const {
        return topLeft > 0.0f ||
               topRight > 0.0f ||
               bottomRight > 0.0f ||
               bottomLeft > 0.0f;
    }
};

struct Transform {
    float translateX = 0.0f;
    float translateY = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotateDeg = 0.0f;

    [[nodiscard]] bool isIdentity() const {
        return translateX == 0.0f &&
               translateY == 0.0f &&
               scaleX == 1.0f &&
               scaleY == 1.0f &&
               rotateDeg == 0.0f;
    }
};

struct TransitionDefinition {
    TransitionProperty property = TransitionProperty::All;
    float durationSeconds = 0.0f;
    float delaySeconds = 0.0f;
    Easing easing = Easing::Ease;
};

struct Style {
    struct Flags {
        bool display = false;
        bool visibility = false;
        bool position = false;
        bool flexDirection = false;
        bool flexWrap = false;
        bool alignItems = false;
        bool justifyContent = false;
        bool alignSelf = false;
        bool flexGrow = false;
        bool flexShrink = false;
        bool width = false;
        bool height = false;
        bool minWidth = false;
        bool minHeight = false;
        bool maxWidth = false;
        bool maxHeight = false;
        bool marginLeft = false;
        bool marginTop = false;
        bool marginRight = false;
        bool marginBottom = false;
        bool paddingLeft = false;
        bool paddingTop = false;
        bool paddingRight = false;
        bool paddingBottom = false;
        bool insetLeft = false;
        bool insetTop = false;
        bool insetRight = false;
        bool insetBottom = false;
        bool color = false;
        bool backgroundColor = false;
        bool borderColor = false;
        bool borderWidth = false;
        bool borderStyle = false;
        bool borderTopLeftRadius = false;
        bool borderTopRightRadius = false;
        bool borderBottomRightRadius = false;
        bool borderBottomLeftRadius = false;
        bool fontSize = false;
        bool fontBold = false;
        bool backgroundGradient = false;
        bool opacity = false;
        bool transform = false;
        bool transition = false;
        bool overflowX = false;
        bool overflowY = false;
        bool scrollbarGutter = false;
        bool pointerEvents = false;
        bool cursor = false;
    };

    Flags flags;
    Display display = Display::Flex;
    Visibility visibility = Visibility::Visible;
    Position position = Position::Relative;
    YGFlexDirection flexDirection = YGFlexDirectionColumn;
    YGWrap flexWrap = YGWrapNoWrap;
    YGAlign alignItems = YGAlignStretch;
    YGJustify justifyContent = YGJustifyFlexStart;
    YGAlign alignSelf = YGAlignAuto;
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    std::optional<Length> width;
    std::optional<Length> height;
    std::optional<Length> minWidth;
    std::optional<Length> minHeight;
    std::optional<Length> maxWidth;
    std::optional<Length> maxHeight;
    EdgeValues margin;
    EdgeValues padding;
    EdgeValues inset;
    SkColor color = SkColorSetARGB(235, 239, 247, 253);
    SkColor backgroundColor = SK_ColorTRANSPARENT;
    SkColor borderColor = SK_ColorTRANSPARENT;
    float borderWidth = 0.0f;
    BorderStyle borderStyle = BorderStyle::None;
    CornerRadii borderRadius;
    float fontSize = 16.0f;
    bool fontBold = false;
    Gradient backgroundGradient;
    float opacity = 1.0f;
    Transform transform;
    std::vector<TransitionDefinition> transitions;
    Overflow overflowX = Overflow::Visible;
    Overflow overflowY = Overflow::Visible;
    bool scrollbarGutterStable = false;
    PointerEvents pointerEvents = PointerEvents::Auto;
    Cursor cursor = Cursor::Auto;
};

struct Node {
    struct InputSnapshot {
        std::string value;
        size_t cursorIndex = 0;
        size_t selectionAnchor = 0;
        size_t selectionStart = 0;
        size_t selectionEnd = 0;
    };

    struct TextLink {
        size_t start = 0;
        size_t end = 0;
        std::string action;
    };

    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::string text;
    std::string value;
    std::string placeholder;
    std::string compositionText;
    uint64_t textRevision = 0;
    std::string src;
    std::string action;
    std::string svgMarkup;
    float numericValue = 0.0f;
    float numericMax = 1.0f;
    float virtualContentWidth = 0.0f;
    float virtualContentHeight = 0.0f;
    std::vector<TextLink> textLinks;
    std::unordered_map<std::string, std::string> attributes;
    Style style;
    Style inlineStyle;
    Style animatedStyle;
    Style::Flags animatedStyleFlags;
    bool hasAnimatedStyle = false;
    Rect layout;
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    float scrollContentWidth = 0.0f;
    float scrollContentHeight = 0.0f;
    bool hovered = false;
    bool active = false;
    bool focused = false;
    size_t cursorIndex = 0;
    size_t selectionAnchor = 0;
    size_t selectionStart = 0;
    size_t selectionEnd = 0;
    std::vector<InputSnapshot> undoStack;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
};

struct AttributeSelector {
    std::string name;
    std::optional<std::string> value;
};

struct CompoundSelector {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::vector<AttributeSelector> attributes;
    std::vector<std::string> pseudos;
};

enum class SelectorCombinator {
    None,
    Descendant,
    Child
};

struct SelectorPart {
    SelectorCombinator combinator = SelectorCombinator::None;
    CompoundSelector selector;
};

struct StyleRule {
    std::vector<SelectorPart> selector;
    Style style;
    std::optional<float> minViewportWidth;
    std::optional<float> maxViewportWidth;
    std::optional<float> minViewportHeight;
    std::optional<float> maxViewportHeight;
    unsigned order = 0;
    unsigned specificity = 0;
};

struct Document {
    std::unique_ptr<Node> root;
    std::vector<StyleRule> rules;
    std::string basePath;
};

class YogaNode {
public:
    YogaNode();
    ~YogaNode();

    YogaNode(const YogaNode&) = delete;
    YogaNode& operator=(const YogaNode&) = delete;

    [[nodiscard]] YGNodeRef get() const;

private:
    YGNodeRef node_ = nullptr;
};

class DocumentParser {
public:
    explicit DocumentParser(RuntimeOptions options);

    bool loadFile(const std::string& path, Document& outDocument, std::string& error);
    bool loadString(std::string_view html, std::string_view basePath, Document& outDocument, std::string& error);
    bool loadFragment(std::string_view html,
                      std::string_view basePath,
                      std::vector<std::unique_ptr<Node>>& outNodes,
                      std::vector<StyleRule>& outRules,
                      std::string& error);

private:
    Theme theme_;
};

class LayoutEngine {
public:
    void layout(Document& document, float width, float height);

private:
    void buildYoga(Node& node, YGNodeRef yogaNode);
    void readYoga(Node& node, YGNodeRef yogaNode, float offsetX, float offsetY);
};

class SkiaRenderer {
public:
    explicit SkiaRenderer(RuntimeOptions options);
    ~SkiaRenderer();
    void draw(Document& document, SkCanvas& canvas, int width, int height, float dpiScale);
    void clearCaches();
    void clearNodeCaches();
    void shutdownCaches();
    size_t textIndexAtOffset(std::string_view value, float size, bool bold, float offset);
    size_t textIndexAtPoint(const Node& node, const std::string& value, float x, float y);
    float textStartX(const Node& node, std::string_view value);
    [[nodiscard]] bool consumeImageDirty();

private:
    struct TextEntry {
        sk_sp<SkTextBlob> blob;
        float width = 0.0f;
        SkRect bounds = SkRect::MakeEmpty();
        SkFontMetrics metrics{};
    };

    struct TextLine {
        size_t start = 0;
        size_t end = 0;
    };

    struct TextLineCacheEntry {
        uint64_t revision = 0;
        const std::string* value = nullptr;
        size_t size = 0;
        float maxWidth = -1.0f;
        std::vector<TextLine> lines;
    };

    struct BoxCacheEntry {
        Rect layout;
        int imageWidth = 0;
        int imageHeight = 0;
        SkColor backgroundColor = SK_ColorTRANSPARENT;
        Gradient backgroundGradient;
        CornerRadii borderRadius;
        BorderStyle borderStyle = BorderStyle::None;
        float borderWidth = 0.0f;
        SkColor borderColor = SK_ColorTRANSPARENT;
        sk_sp<SkImage> image;
    };

    struct SvgDomEntry {
        sk_sp<SkSVGDOM> dom;
    };

    enum class ImageState {
        Loading,
        Ready,
        Failed
    };

    struct BitmapImageEntry {
        ImageState state = ImageState::Loading;
        std::shared_ptr<std::vector<unsigned char>> pixels;
        sk_sp<SkImage> image;
        size_t rowBytes = 0;
        size_t byteSize = 0;
        uint64_t lastUsedFrame = 0;
        int width = 0;
        int height = 0;
    };

    struct BitmapImageState {
        std::unordered_map<std::string, BitmapImageEntry> cache;
        std::deque<std::string> queue;
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable cv;
        size_t cacheBytes = 0;
        size_t budgetBytes = 0;
        size_t workerCount = 1;
        uint64_t frame = 0;
        bool dirty = false;
        bool workersStarted = false;
        bool stop = false;
    };

    std::string assetRoot_;
    SkColor clearColor_ = SkColorSetRGB(7, 12, 18);
    size_t bitmapCacheBudgetBytes_ = 0;
    size_t bitmapLoadWorkerCount_ = 1;
    RequestRedrawCallback requestRedraw_;
    sk_sp<SkFontMgr> fontMgr_;
    sk_sp<SkTypeface> regular_;
    sk_sp<SkTypeface> bold_;
    std::unordered_map<std::string, TextEntry> textCache_;
    bool traceRender_ = false;
    double traceBoxMs_ = 0.0;
    double traceProgressMs_ = 0.0;
    double traceImageMs_ = 0.0;
    double traceSvgMs_ = 0.0;
    double traceSelectionMs_ = 0.0;
    double traceTextMs_ = 0.0;
    double traceCaretMs_ = 0.0;
    double traceScrollbarMs_ = 0.0;
    int traceTextCount_ = 0;
    int traceSvgCount_ = 0;
    int traceNodeCount_ = 0;

    sk_sp<SkTypeface> pickTypeface(bool bold);
    SkFont font(float size, bool bold) const;
    SkPaint fill(SkColor color) const;
    SkPaint stroke(SkColor color, float width) const;
    SkPaint backgroundPaint(const Node& node, const Rect& rect) const;
    void drawNode(SkCanvas& canvas, const Document& document, const Node& node);
    void drawBox(SkCanvas& canvas, const Node& node);
    void drawBoxDirect(SkCanvas& canvas, const Node& node, const Rect& rect);
    bool drawCachedBox(SkCanvas& canvas, const Node& node, const Rect& rect, SkColor borderColor);
    void drawProgress(SkCanvas& canvas, const Node& node);
    void drawImage(SkCanvas& canvas, const Document& document, const Node& node);
    void drawInlineSvg(SkCanvas& canvas, const Node& node);
    void drawScrollbars(SkCanvas& canvas, const Node& node);
    void drawSelectableSelection(SkCanvas& canvas, const Node& node);
    void drawSvgMarkup(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor);
    bool drawSvgDom(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor);
    void drawInputSelection(SkCanvas& canvas, const Node& node);
    void drawText(SkCanvas& canvas, const Node& node);
    void drawInputCompositionUnderline(SkCanvas& canvas, const Node& node);
    void drawInputCaret(SkCanvas& canvas, const Node& node);
    void beginBitmapFrame();
    void endBitmapFrame();
    BitmapImageEntry bitmapImageEntry(const std::string& path);
    sk_sp<SkImage> bitmapImageForEntry(const std::string& path, const BitmapImageEntry& entry);
    void enqueueBitmapLoad(const std::shared_ptr<BitmapImageState>& state, const std::string& path) const;
    void ensureBitmapWorkers(const std::shared_ptr<BitmapImageState>& state) const;
    void stopBitmapLoads(const std::shared_ptr<BitmapImageState>& state);
    static void pruneBitmapCacheLocked(BitmapImageState& state);
    static void bitmapWorkerLoop(std::shared_ptr<BitmapImageState> state, RequestRedrawCallback requestRedraw);
    static BitmapImageEntry loadBitmapImage(const std::string& path);
    std::optional<std::string> readSvgAsset(const Document& document, std::string_view src);
    std::string resolveAssetPath(const Document& document, std::string_view src) const;
    static bool isSvgSource(std::string_view src);
    const TextEntry& textEntry(std::string_view value, float size, bool bold);
    float textWidth(std::string_view value, float size, bool bold);
    const std::vector<TextLine>& textLines(const Node& node, const std::string& value);
    std::unordered_map<std::string, SvgDomEntry> svgDomCache_;
    std::unordered_map<std::string, std::string> svgFileCache_;
    std::unordered_map<const Node*, TextLineCacheEntry> textLineCache_;
    std::unordered_map<const Node*, BoxCacheEntry> boxCache_;
    std::shared_ptr<BitmapImageState> bitmapState_;
};

SkColor rgb(unsigned r, unsigned g, unsigned b);
SkColor rgba(unsigned r, unsigned g, unsigned b, unsigned a);
SkColor parseColor(std::string_view value, SkColor fallback);
float clampf(float value, float lo, float hi);
std::string trim(std::string_view value);
std::vector<std::string> splitWhitespace(std::string_view value);
constexpr float kSkuiScrollbarThickness = 6.0f;
constexpr float kSkuiScrollbarInset = 4.0f;
constexpr float kSkuiScrollbarMinThumb = 24.0f;
constexpr float kSkuiScrollbarGutter = kSkuiScrollbarThickness + kSkuiScrollbarInset * 2.0f;
float scrollViewportWidth(const Node& node);
float scrollViewportHeight(const Node& node);
float scrollMaxX(const Node& node);
float scrollMaxY(const Node& node);
bool shouldShowScrollbarX(const Node& node);
bool shouldShowScrollbarY(const Node& node);
Rect scrollContentClipRect(const Node& node);
float stickyVisualOffsetY(const Node& node);
std::filesystem::path pathFromUtf8(std::string_view text);
std::string pathToUtf8(const std::filesystem::path& path);
void parseInlineStyle(std::string_view declarations, Style& style);
void recomputeStyles(Document& document, const RuntimeOptions& options, float viewportWidth = 0.0f, float viewportHeight = 0.0f);
void applyAnimatedStyles(Node& node);

}  // namespace skui
