#pragma once

#include "skui_runtime.h"

#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkPath.h"
#include "include/core/SkPicture.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "modules/svg/include/SkSVGDOM.h"
#include <yoga/Yoga.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

enum class Position {
    Relative,
    Absolute
};

enum class GradientKind {
    None,
    LinearX,
    LinearY,
    Radial
};

struct Gradient {
    GradientKind kind = GradientKind::None;
    std::vector<SkColor> colors;
    std::vector<float> positions;
};

struct EdgeValues {
    std::optional<float> left;
    std::optional<float> top;
    std::optional<float> right;
    std::optional<float> bottom;
};

struct Style {
    struct Flags {
        bool display = false;
        bool position = false;
        bool flexDirection = false;
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
        bool borderRadius = false;
        bool fontSize = false;
        bool fontBold = false;
        bool icon = false;
        bool iconColor = false;
        bool iconSize = false;
        bool backgroundGradient = false;
    };

    Flags flags;
    Display display = Display::Flex;
    Position position = Position::Relative;
    YGFlexDirection flexDirection = YGFlexDirectionColumn;
    YGAlign alignItems = YGAlignStretch;
    YGJustify justifyContent = YGJustifyFlexStart;
    YGAlign alignSelf = YGAlignAuto;
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    std::optional<float> width;
    std::optional<float> height;
    std::optional<float> minWidth;
    std::optional<float> minHeight;
    std::optional<float> maxWidth;
    std::optional<float> maxHeight;
    EdgeValues margin;
    EdgeValues padding;
    EdgeValues inset;
    SkColor color = SkColorSetARGB(235, 239, 247, 253);
    SkColor backgroundColor = SK_ColorTRANSPARENT;
    SkColor borderColor = SK_ColorTRANSPARENT;
    float borderWidth = 0.0f;
    float borderRadius = 0.0f;
    float fontSize = 16.0f;
    bool fontBold = false;
    std::string icon;
    SkColor iconColor = SkColorSetARGB(220, 216, 226, 238);
    float iconSize = 24.0f;
    Gradient backgroundGradient;
};

struct Node {
    std::string tag;
    std::string id;
    std::vector<std::string> classes;
    std::string text;
    std::string value;
    std::string icon;
    Style style;
    Style inlineStyle;
    Rect layout;
    bool hovered = false;
    bool active = false;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
};

struct StyleRule {
    enum class Kind {
        Tag,
        Class,
        Id
    };

    Kind kind = Kind::Tag;
    std::string selector;
    Style style;
    unsigned order = 0;
};

struct Document {
    std::unique_ptr<Node> root;
    std::vector<StyleRule> rules;
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

private:
    RuntimeOptions options_;
};

class LayoutEngine {
public:
    void layout(Document& document, float width, float height);

private:
    void buildYoga(Node& node, YGNodeRef yogaNode, std::vector<std::unique_ptr<YogaNode>>& owned);
    void readYoga(Node& node, YGNodeRef yogaNode, float offsetX, float offsetY);
};

class SkiaRenderer {
public:
    explicit SkiaRenderer(RuntimeOptions options);
    void draw(Document& document, SkCanvas& canvas, int width, int height, float dpiScale);

private:
    struct TextEntry {
        sk_sp<SkTextBlob> blob;
        float width = 0.0f;
        SkRect bounds = SkRect::MakeEmpty();
    };

    RuntimeOptions options_;
    sk_sp<SkFontMgr> fontMgr_;
    sk_sp<SkTypeface> regular_;
    sk_sp<SkTypeface> bold_;
    std::unordered_map<std::string, TextEntry> textCache_;
    std::unordered_map<std::string, sk_sp<SkSVGDOM>> svgCache_;

    sk_sp<SkTypeface> pickTypeface(bool bold);
    SkFont font(float size, bool bold) const;
    SkPaint fill(SkColor color) const;
    SkPaint stroke(SkColor color, float width) const;
    SkPaint backgroundPaint(const Node& node) const;
    void drawNode(SkCanvas& canvas, const Node& node);
    void drawBox(SkCanvas& canvas, const Node& node);
    void drawText(SkCanvas& canvas, const Node& node);
    void drawIcon(SkCanvas& canvas, const Node& node);
    void drawSvgIcon(SkCanvas& canvas, const std::string& svg, float cx, float cy, float size);
    std::string iconSvg(const Node& node) const;
    const TextEntry& textEntry(std::string_view value, float size, bool bold);
    float textWidth(std::string_view value, float size, bool bold);
};

SkColor rgb(unsigned r, unsigned g, unsigned b);
SkColor rgba(unsigned r, unsigned g, unsigned b, unsigned a);
SkColor parseColor(std::string_view value, SkColor fallback);
float clampf(float value, float lo, float hi);
std::string trim(std::string_view value);
std::vector<std::string> splitWhitespace(std::string_view value);

}  // namespace skui
