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
#include <unordered_set>
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
    Grid,
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
    LinearAngle,
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
    Height,
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

struct CubicBezier {
    float x1 = 0.25f;
    float y1 = 0.1f;
    float x2 = 0.25f;
    float y2 = 1.0f;

    [[nodiscard]] bool operator==(const CubicBezier&) const = default;
};

struct EasingFunction {
    Easing keyword = Easing::Ease;
    std::optional<CubicBezier> cubicBezier;

    [[nodiscard]] bool operator==(const EasingFunction&) const = default;
};

enum class BackgroundRepeat {
    Repeat,
    NoRepeat,
    RepeatX,
    RepeatY
};

enum class AnimationTimingKind {
    Easing,
    Steps
};

enum class AnimationStepPosition {
    Start,
    End
};

enum class AnimationDirection {
    Normal,
    Reverse,
    Alternate,
    AlternateReverse
};

enum class AnimationFillMode {
    None,
    Forwards,
    Backwards,
    Both
};

enum class AnimationPlayState {
    Running,
    Paused
};

struct Length {
    float value = 0.0f;
    LengthUnit unit = LengthUnit::Px;
};

struct BackgroundPosition {
    Length x = {0.0f, LengthUnit::Percent};
    Length y = {0.0f, LengthUnit::Percent};
};

struct BackgroundSize {
    Length width = {0.0f, LengthUnit::Auto};
    Length height = {0.0f, LengthUnit::Auto};
};

struct Gradient {
    GradientKind kind = GradientKind::None;
    std::vector<SkColor> colors;
    std::vector<std::optional<Length>> stopPositions;
    float angleDegrees = 180.0f;
    Length centerX = {50.0f, LengthUnit::Percent};
    Length centerY = {50.0f, LengthUnit::Percent};
};

enum class GridTrackKind {
    Fixed,
    Fraction,
    Auto
};

struct GridTrack {
    GridTrackKind kind = GridTrackKind::Auto;
    Length length = {0.0f, LengthUnit::Auto};
    float fraction = 0.0f;
};

enum class GridLineKind {
    Auto,
    Index,
    Span
};

struct GridLine {
    GridLineKind kind = GridLineKind::Auto;
    int value = 0;
};

enum class GridItemAlignment {
    Auto,
    Stretch,
    Start,
    Center,
    End
};

struct Shadow {
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float blurRadius = 0.0f;
    float spreadRadius = 0.0f;
    SkColor color = SK_ColorBLACK;
    bool usesCurrentColor = true;
    bool inset = false;
};

struct BorderSide {
    std::optional<SkColor> color;
    std::optional<float> width;
    std::optional<BorderStyle> style;
};

struct BorderEdges {
    BorderSide left;
    BorderSide top;
    BorderSide right;
    BorderSide bottom;
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

enum class TransformOperationKind {
    Translate,
    Scale,
    Rotate
};

struct TransformOperation {
    TransformOperationKind kind = TransformOperationKind::Translate;
    Length translateX = {0.0f, LengthUnit::Px};
    Length translateY = {0.0f, LengthUnit::Px};
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotateDeg = 0.0f;
};

struct Transform {
    std::vector<TransformOperation> operations;

    [[nodiscard]] bool isIdentity() const {
        for (const TransformOperation& operation : operations) {
            if (operation.kind == TransformOperationKind::Translate &&
                (operation.translateX.value != 0.0f ||
                 operation.translateY.value != 0.0f)) {
                return false;
            }
            if (operation.kind == TransformOperationKind::Scale &&
                (operation.scaleX != 1.0f || operation.scaleY != 1.0f)) {
                return false;
            }
            if (operation.kind == TransformOperationKind::Rotate &&
                operation.rotateDeg != 0.0f) {
                return false;
            }
        }
        return true;
    }
};

struct TransformOrigin {
    Length x = {50.0f, LengthUnit::Percent};
    Length y = {50.0f, LengthUnit::Percent};
};

enum class FilterOperationKind {
    Grayscale,
    Brightness,
    DropShadow
};

struct FilterOperation {
    FilterOperationKind kind = FilterOperationKind::Grayscale;
    float amount = 0.0f;
    Shadow shadow;
};

struct Filter {
    std::vector<FilterOperation> operations;

    [[nodiscard]] bool isIdentity() const {
        for (const FilterOperation& operation : operations) {
            if (operation.kind == FilterOperationKind::Grayscale &&
                operation.amount != 0.0f) {
                return false;
            }
            if (operation.kind == FilterOperationKind::Brightness &&
                operation.amount != 1.0f) {
                return false;
            }
            if (operation.kind == FilterOperationKind::DropShadow &&
                SkColorGetA(operation.shadow.color) > 0) {
                return false;
            }
        }
        return true;
    }
};

struct TransitionDefinition {
    TransitionProperty property = TransitionProperty::All;
    float durationSeconds = 0.0f;
    float delaySeconds = 0.0f;
    EasingFunction easing;
};

struct AnimationTimingFunction {
    AnimationTimingKind kind = AnimationTimingKind::Easing;
    EasingFunction easing;
    int steps = 1;
    AnimationStepPosition stepPosition = AnimationStepPosition::End;
};

struct AnimationDefinition {
    std::string name;
    float durationSeconds = 0.0f;
    float delaySeconds = 0.0f;
    float iterationCount = 1.0f;
    AnimationTimingFunction timing;
    AnimationDirection direction = AnimationDirection::Normal;
    AnimationFillMode fillMode = AnimationFillMode::None;
    AnimationPlayState playState = AnimationPlayState::Running;
};

struct Style {
    struct Flags {
        bool display = false;
        bool visibility = false;
        bool position = false;
        bool zIndex = false;
        bool flexDirection = false;
        bool flexWrap = false;
        bool rowGap = false;
        bool columnGap = false;
        bool alignItems = false;
        bool justifyContent = false;
        bool alignSelf = false;
        bool flexGrow = false;
        bool flexShrink = false;
        bool flexBasis = false;
        bool gridTemplateColumns = false;
        bool gridTemplateRows = false;
        bool gridAutoRows = false;
        bool gridColumnStart = false;
        bool gridColumnEnd = false;
        bool justifyItems = false;
        bool justifySelf = false;
        bool boxSizing = false;
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
        bool backgroundImage = false;
        bool backgroundPosition = false;
        bool backgroundSize = false;
        bool backgroundRepeat = false;
        bool borderTopLeftRadius = false;
        bool borderTopRightRadius = false;
        bool borderBottomRightRadius = false;
        bool borderBottomLeftRadius = false;
        bool fontSize = false;
        bool fontBold = false;
        bool backgroundGradient = false;
        bool maskImage = false;
        bool boxShadow = false;
        bool textShadow = false;
        bool content = false;
        bool opacity = false;
        bool transform = false;
        bool transformOrigin = false;
        bool filter = false;
        bool transition = false;
        bool animation = false;
        bool overflowX = false;
        bool overflowY = false;
        bool scrollbarGutter = false;
        bool pointerEvents = false;
        bool cursor = false;
    };

    Flags flags;
    Display display = Display::Flex;
    bool displayFlex = false;
    Visibility visibility = Visibility::Visible;
    Position position = Position::Relative;
    int zIndex = 0;
    YGFlexDirection flexDirection = YGFlexDirectionColumn;
    YGWrap flexWrap = YGWrapNoWrap;
    std::optional<Length> rowGap;
    std::optional<Length> columnGap;
    YGAlign alignItems = YGAlignStretch;
    YGJustify justifyContent = YGJustifyFlexStart;
    YGAlign alignSelf = YGAlignAuto;
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    std::optional<Length> flexBasis;
    std::vector<GridTrack> gridTemplateColumns;
    std::vector<GridTrack> gridTemplateRows;
    std::optional<GridTrack> gridAutoRows;
    GridLine gridColumnStart;
    GridLine gridColumnEnd;
    GridItemAlignment justifyItems = GridItemAlignment::Stretch;
    GridItemAlignment justifySelf = GridItemAlignment::Auto;
    YGBoxSizing boxSizing = YGBoxSizingContentBox;
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
    std::string backgroundImage;
    BackgroundPosition backgroundPosition;
    BackgroundSize backgroundSize;
    BackgroundRepeat backgroundRepeat = BackgroundRepeat::Repeat;
    BorderEdges borders;
    CornerRadii borderRadius;
    float fontSize = 16.0f;
    bool fontBold = false;
    std::vector<Gradient> backgroundGradients;
    std::optional<Gradient> maskGradient;
    std::vector<Shadow> boxShadows;
    std::vector<Shadow> textShadows;
    std::string content;
    float opacity = 1.0f;
    Transform transform;
    TransformOrigin transformOrigin;
    Filter filter;
    std::vector<TransitionDefinition> transitions;
    std::vector<AnimationDefinition> animations;
    Overflow overflowX = Overflow::Visible;
    Overflow overflowY = Overflow::Visible;
    bool scrollbarGutterStable = false;
    PointerEvents pointerEvents = PointerEvents::Auto;
    Cursor cursor = Cursor::Auto;
};

struct Keyframe {
    float offset = 0.0f;
    Style style;
};

struct KeyframesDefinition {
    std::string name;
    std::vector<Keyframe> frames;
};

struct Node {
    struct InputSnapshot {
        std::string value;
        size_t cursorIndex = 0;
        size_t selectionAnchor = 0;
        size_t selectionStart = 0;
        size_t selectionEnd = 0;
    };

    struct ResolvedEdges {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
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
    Style beforeStyle;
    Style afterStyle;
    Style presentationStyle;
    Style inlineStyle;
    Style animatedStyle;
    Style::Flags animatedStyleFlags;
    bool hasAnimatedStyle = false;
    bool hasBeforeStyle = false;
    bool hasAfterStyle = false;
    sk_sp<SkImage> videoFrame;
    int videoFrameWidth = 0;
    int videoFrameHeight = 0;
    Rect layout;
    ResolvedEdges resolvedPadding;
    float scrollX = 0.0f;
    float scrollY = 0.0f;
    float scrollContentWidth = 0.0f;
    float scrollContentHeight = 0.0f;
    bool hovered = false;
    bool active = false;
    bool focused = false;
    bool editingFocused = false;
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

enum class PseudoElement {
    None,
    Before,
    After
};

struct SelectorPart {
    SelectorCombinator combinator = SelectorCombinator::None;
    CompoundSelector selector;
};

struct StyleRule {
    std::vector<SelectorPart> selector;
    Style style;
    PseudoElement pseudoElement = PseudoElement::None;
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
    std::unordered_map<std::string, KeyframesDefinition> keyframes;
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
    void buildYoga(Node& node, YGNodeRef yogaNode, bool isRoot);
    void readYoga(Node& node, YGNodeRef yogaNode, float offsetX, float offsetY);
};

class SkiaRenderer {
public:
    struct TextHitResult {
        size_t index = 0;
        bool insideText = false;
    };

    explicit SkiaRenderer(RuntimeOptions options);
    ~SkiaRenderer();
    void draw(Document& document,
              SkCanvas& canvas,
              int width,
              int height,
              float dpiScale,
              bool clearCanvas = true);
    void clearCaches();
    void clearNodeCaches();
    void shutdownCaches();
    [[nodiscard]] MemoryStats memoryStats() const;
    size_t textIndexAtOffset(std::string_view value, float size, bool bold, float offset);
    TextHitResult textHitAtPoint(const Node& node,
                                 const std::string& value,
                                 float x,
                                 float y);
    float textStartX(const Node& node, std::string_view value);
    [[nodiscard]] bool consumeImageDirty();
    void requestBitmapImages(const Document& document);

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

    struct DisplayedBitmapImage {
        std::string path;
        sk_sp<SkImage> image;
        uint64_t lastUsedFrame = 0;
        int width = 0;
        int height = 0;
    };

    struct ResidentBitmapImage {
        sk_sp<SkImage> rasterImage;
        sk_sp<SkImage> displayImage;
        size_t rowBytes = 0;
        size_t byteSize = 0;
        uint64_t lastUsedFrame = 0;
        int width = 0;
        int height = 0;
    };

    struct BitmapImageState {
        std::unordered_map<std::string, BitmapImageEntry> cache;
        std::unordered_set<std::string> activePaths;
        std::deque<std::string> queue;
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable cv;
        size_t cacheBytes = 0;
        size_t budgetBytes = 0;
        size_t workerCount = 1;
        uint64_t frame = 0;
        size_t peakCacheBytes = 0;
        uint64_t cacheHitCount = 0;
        uint64_t cacheMissCount = 0;
        uint64_t decodeCount = 0;
        uint64_t evictionCount = 0;
        bool dirty = false;
        bool workersStarted = false;
        bool stop = false;
    };

    std::string assetRoot_;
    SkColor clearColor_ = SkColorSetRGB(7, 12, 18);
    size_t bitmapCacheBudgetBytes_ = 0;
    size_t bitmapLoadWorkerCount_ = 1;
    float lazyImagePreloadMarginViewports_ = 0.0f;
    float lazyImagePreloadMarginX_ = 0.0f;
    float lazyImagePreloadMarginY_ = 0.0f;
    RequestRedrawCallback requestRedraw_;
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

    SkFont font(float size, bool bold) const;
    SkPaint fill(SkColor color) const;
    SkPaint stroke(SkColor color, float width) const;
    SkPaint gradientPaint(const Gradient& gradient,
                          const Style& style,
                          const Rect& rect) const;
    void drawNode(SkCanvas& canvas, const Document& document, const Node& node);
    void drawBox(SkCanvas& canvas, const Document& document, const Node& node);
    void drawBoxDirect(SkCanvas& canvas,
                       const Document& document,
                       const Node& node,
                       const Rect& rect);
    void drawBoxShadows(SkCanvas& canvas,
                        const Node& node,
                        const Rect& rect,
                        bool inset);
    void drawPseudoElement(SkCanvas& canvas,
                           const Document& document,
                           const Node& node,
                           const Style& style);
    void drawBackgroundImage(SkCanvas& canvas,
                             const Document& document,
                             const Node& node,
                             const Rect& rect);
    void drawProgress(SkCanvas& canvas, const Node& node);
    void drawImage(SkCanvas& canvas, const Document& document, const Node& node);
    void drawInlineSvg(SkCanvas& canvas, const Node& node);
    void drawScrollbars(SkCanvas& canvas, const Node& node);
    void drawSelectableSelection(SkCanvas& canvas, const Node& node);
    void drawSvgMarkup(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor);
    bool drawSvgDom(SkCanvas& canvas, const std::string& svg, const Rect& rect, SkColor currentColor);
    void drawInputSelection(SkCanvas& canvas, const Node& node);
    void drawText(SkCanvas& canvas, const Node& node);
    void drawStyledTextBlob(SkCanvas& canvas,
                            const Node& node,
                            const sk_sp<SkTextBlob>& blob,
                            float x,
                            float y,
                            SkColor color);
    void drawInputCompositionUnderline(SkCanvas& canvas, const Node& node);
    void drawInputCaret(SkCanvas& canvas, const Node& node);
    void beginBitmapFrame();
    void endBitmapFrame();
    void requestLazyBitmapImageIfNeeded(SkCanvas& canvas,
                                        const Document& document,
                                        const Node& node);
    void requestBitmapImagesForNode(const Document& document, const Node& node);
    void requestBitmapImage(const std::string& path);
    BitmapImageEntry bitmapImageEntry(const std::string& path);
    sk_sp<SkImage> bitmapImageForEntry(SkCanvas& canvas,
                                       const std::string& path,
                                       const BitmapImageEntry& entry);
    sk_sp<SkImage> displayBitmapImageForNode(SkCanvas& canvas,
                                             const Node& node,
                                             const std::string& path,
                                             const BitmapImageEntry& entry,
                                             int& imageWidth,
                                             int& imageHeight);
    void drawBitmapImage(SkCanvas& canvas,
                         const Node& node,
                         SkImage& image,
                         int imageWidth,
                         int imageHeight);
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
    std::unordered_map<const Node*, DisplayedBitmapImage> displayedBitmapImages_;
    std::unordered_map<std::string, ResidentBitmapImage> residentBitmapImages_;
    std::unordered_set<std::string> lazyPreloadPathsPreviousFrame_;
    std::unordered_set<std::string> lazyPreloadPathsCurrentFrame_;
    std::shared_ptr<BitmapImageState> bitmapState_;
};

SkColor rgb(unsigned r, unsigned g, unsigned b);
SkColor rgba(unsigned r, unsigned g, unsigned b, unsigned a);
SkColor parseColor(std::string_view value, SkColor fallback);
sk_sp<SkFontMgr> uiFontManager();
SkFont makeUiFont(float size, bool bold);
float measureUiTextWidth(std::string_view value, float size, bool bold);
float clampf(float value, float lo, float hi);
std::string trim(std::string_view value);
std::vector<std::string> splitWhitespace(std::string_view value);
enum class ContentEditableState {
    Inherit,
    True,
    False,
    PlaintextOnly
};
ContentEditableState contentEditableState(const Node& node);
bool isContentEditable(const Node& node);
bool isContentEditableEditingHost(const Node& node);
bool isContentEditableTextNode(const Node& node);
bool isTextEditingNode(const Node& node);
Node* contentEditableEditingHost(Node* node);
const Node* contentEditableEditingHost(const Node* node);
void prepareContentEditableTree(Node& node);
void syncContentEditablePlaceholder(Node& node);
std::string textContent(const Node& node);
std::string editableTextContent(const Node& node);
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
bool requiresZIndexOrdering(const Node& node);
std::vector<const Node*> childrenInPaintOrder(const Node& node);
std::vector<Node*> childrenInPaintOrder(Node& node);
std::filesystem::path pathFromUtf8(std::string_view text);
std::string pathToUtf8(const std::filesystem::path& path);
void parseInlineStyle(std::string_view declarations, Style& style);
void recomputeStyles(Document& document, const RuntimeOptions& options, float viewportWidth = 0.0f, float viewportHeight = 0.0f);
void applyAnimatedStyles(Node& node);

}  // namespace skui
