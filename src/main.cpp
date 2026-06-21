#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_win.h"
#include <yoga/Yoga.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kBaseWidth = 1536;
constexpr int kBaseHeight = 960;
constexpr int kChromeHeight = 46;
constexpr int kContentHeight = kBaseHeight - kChromeHeight;
constexpr float kHeaderHeight = 148.0f;
constexpr float kDefaultDpi = 96.0f;

SkColor rgb(uint8_t r, uint8_t g, uint8_t b) {
    return SkColorSetRGB(r, g, b);
}

SkColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return SkColorSetARGB(a, r, g, b);
}

float clampf(float value, float lo, float hi) {
    return std::max(lo, std::min(value, hi));
}

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    SkRect sk() const {
        return SkRect::MakeXYWH(x, y, w, h);
    }
};

struct Device {
    std::string name;
    std::string ip;
    bool online = true;
};

struct Layout {
    float width = 0.0f;
    float height = 0.0f;
    Rect sidebar;
    Rect chat;
    Rect inspector;
    Rect chatHeader;
    Rect messages;
    Rect composer;
    Rect composerInput;
    Rect composerSend;
    Rect msgToday;
    Rect msgAskBubble;
    Rect msgAskTime;
    Rect msgUploadBubble;
    Rect msgUploadTime;
    Rect msgUploadCheck;
    Rect msgReportCard;
    Rect msgThanksBubble;
    Rect msgThanksTime;
    Rect msgDesignCard;
    Rect msgFinalBubble;
    Rect msgFinalTime;
    Rect msgFinalCheck;
    Rect msgScrollbar;
    float composerSmileX = 0.0f;
    float composerPaperclipX = 0.0f;
    float composerFolderX = 0.0f;
    float sidebarW = 0.0f;
    float chatLeft = 0.0f;
    float chatRight = 0.0f;
    float chatW = 0.0f;
    float inspectorLeft = 0.0f;
    float inspectorW = 0.0f;
    float composerTop = 0.0f;
    bool showInspector = true;
};

class YogaNode {
public:
    YogaNode() : node_(YGNodeNew()) {}
    ~YogaNode() {
        if (node_) {
            YGNodeFree(node_);
        }
    }

    YogaNode(const YogaNode&) = delete;
    YogaNode& operator=(const YogaNode&) = delete;

    YGNodeRef get() const {
        return node_;
    }

private:
    YGNodeRef node_ = nullptr;
};

class Renderer {
public:
    Renderer() {
        fontMgr_ = SkFontMgr_New_DirectWrite();
        if (!fontMgr_) {
            fontMgr_ = SkFontMgr_New_GDI();
        }
        regular_ = pickTypeface(false);
        bold_ = pickTypeface(true);
    }

    void draw(SkCanvas& canvas, int width, int height, float dpiScale) {
        canvas.clear(rgb(248, 250, 252));
        const float scale = std::max(0.1f, dpiScale);
        const float logicalWidth = static_cast<float>(width) / scale;
        const float logicalHeight = static_cast<float>(height) / scale;

        canvas.save();
        canvas.scale(scale, scale);
        drawApp(canvas, makeLayout(logicalWidth, logicalHeight));
        canvas.restore();
    }

private:
    sk_sp<SkFontMgr> fontMgr_;
    sk_sp<SkTypeface> regular_;
    sk_sp<SkTypeface> bold_;

    sk_sp<SkTypeface> pickTypeface(bool bold) {
        const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
        const std::array<const char*, 4> families = {"Microsoft YaHei UI", "Segoe UI", "Microsoft YaHei", nullptr};
        for (const char* family : families) {
            if (fontMgr_) {
                sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(family, style);
                if (typeface) {
                    return typeface;
                }
            }
        }
        return nullptr;
    }

    SkFont font(float size, bool bold = false) const {
        SkFont f(bold ? bold_ : regular_, size);
        f.setEdging(SkFont::Edging::kAntiAlias);
        f.setSubpixel(true);
        return f;
    }

    SkPaint fill(SkColor color) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        p.setColor(color);
        return p;
    }

    SkPaint stroke(SkColor color, float width = 1.0f) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(width);
        p.setColor(color);
        return p;
    }

    SkRRect rr(const Rect& r, float radius) const {
        return SkRRect::MakeRectXY(r.sk(), radius, radius);
    }

    void text(SkCanvas& c,
              std::string_view value,
              float x,
              float y,
              float size,
              SkColor color = rgb(18, 23, 31),
              bool isBold = false) const {
        SkPaint p = fill(color);
        SkFont f = font(size, isBold);
        c.drawSimpleText(value.data(), value.size(), SkTextEncoding::kUTF8, x, y, f, p);
    }

    float textWidth(std::string_view value, float size, bool isBold = false) const {
        SkFont f = font(size, isBold);
        return f.measureText(value.data(), value.size(), SkTextEncoding::kUTF8);
    }

    void centeredText(SkCanvas& c,
                      std::string_view value,
                      const Rect& box,
                      float size,
                      SkColor color,
                      bool isBold = false) const {
        SkFont f = font(size, isBold);
        SkRect bounds;
        const float w = f.measureText(value.data(), value.size(), SkTextEncoding::kUTF8, &bounds);
        SkPaint p = fill(color);
        const float x = box.x + (box.w - w) * 0.5f;
        const float y = box.y + box.h * 0.5f - (bounds.fTop + bounds.fBottom) * 0.5f;
        c.drawSimpleText(value.data(), value.size(), SkTextEncoding::kUTF8, x, y, f, p);
    }

    void line(SkCanvas& c, float x1, float y1, float x2, float y2, SkColor color, float width = 1.0f) const {
        c.drawLine(x1, y1, x2, y2, stroke(color, width));
    }

    Rect layoutRect(YGNodeConstRef node, float offsetX = 0.0f, float offsetY = 0.0f) const {
        return {offsetX + YGNodeLayoutGetLeft(node),
                offsetY + YGNodeLayoutGetTop(node),
                YGNodeLayoutGetWidth(node),
                YGNodeLayoutGetHeight(node)};
    }

    Rect childLayoutRect(YGNodeConstRef parent, YGNodeConstRef child, const Rect& offset) const {
        return layoutRect(child, offset.x + YGNodeLayoutGetLeft(parent), offset.y + YGNodeLayoutGetTop(parent));
    }

    void setNodeSize(YGNodeRef node, float width, float height) const {
        YGNodeStyleSetWidth(node, width);
        YGNodeStyleSetHeight(node, height);
        YGNodeStyleSetFlexShrink(node, 0.0f);
    }

    void setMessageNode(YGNodeRef node, float width, float height, bool outgoing, float bottomGap) const {
        setNodeSize(node, width, height);
        YGNodeStyleSetAlignSelf(node, outgoing ? YGAlignFlexEnd : YGAlignFlexStart);
        YGNodeStyleSetMargin(node, YGEdgeBottom, bottomGap);
    }

    Layout makeLayout(float width, float height) const {
        Layout l;
        l.width = width;
        l.height = height;

        YogaNode root;
        YogaNode sidebar;
        YogaNode chat;
        YogaNode inspector;
        YGNodeStyleSetWidth(root.get(), width);
        YGNodeStyleSetHeight(root.get(), height);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(root.get(), YGAlignStretch);

        YGNodeStyleSetWidthPercent(sidebar.get(), 30.0f);
        YGNodeStyleSetMinWidth(sidebar.get(), 230.0f);
        YGNodeStyleSetMaxWidth(sidebar.get(), 386.0f);
        YGNodeStyleSetFlexShrink(sidebar.get(), 0.0f);

        l.showInspector = width >= 1120.0f;
        if (l.showInspector) {
            YGNodeStyleSetWidthPercent(inspector.get(), 25.0f);
            YGNodeStyleSetMinWidth(inspector.get(), 300.0f);
            YGNodeStyleSetMaxWidth(inspector.get(), 384.0f);
            YGNodeStyleSetFlexShrink(inspector.get(), 1.0f);
        }

        YGNodeStyleSetFlexGrow(chat.get(), 1.0f);
        YGNodeStyleSetFlexShrink(chat.get(), 1.0f);
        YGNodeStyleSetMinWidth(chat.get(), 440.0f);

        YGNodeInsertChild(root.get(), sidebar.get(), 0);
        YGNodeInsertChild(root.get(), chat.get(), 1);
        if (l.showInspector) {
            YGNodeInsertChild(root.get(), inspector.get(), 2);
            YGNodeCalculateLayout(root.get(), width, height, YGDirectionLTR);
            if (YGNodeLayoutGetWidth(inspector.get()) < 280.0f || YGNodeLayoutGetWidth(chat.get()) < 440.0f) {
                YGNodeRemoveChild(root.get(), inspector.get());
                l.showInspector = false;
            }
        }

        YGNodeCalculateLayout(root.get(), width, height, YGDirectionLTR);

        l.sidebar = layoutRect(sidebar.get());
        l.chat = layoutRect(chat.get());
        l.inspector = l.showInspector ? layoutRect(inspector.get()) : Rect{};

        YogaNode chatRoot;
        YogaNode header;
        YogaNode messages;
        YogaNode composer;
        YGNodeStyleSetWidth(chatRoot.get(), l.chat.w);
        YGNodeStyleSetHeight(chatRoot.get(), l.chat.h);
        YGNodeStyleSetFlexDirection(chatRoot.get(), YGFlexDirectionColumn);

        YGNodeStyleSetHeight(header.get(), kHeaderHeight);
        YGNodeStyleSetFlexShrink(header.get(), 0.0f);

        YGNodeStyleSetFlexGrow(messages.get(), 1.0f);
        YGNodeStyleSetFlexShrink(messages.get(), 1.0f);

        YGNodeStyleSetHeight(composer.get(), 112.0f);
        YGNodeStyleSetFlexShrink(composer.get(), 0.0f);

        YGNodeInsertChild(chatRoot.get(), header.get(), 0);
        YGNodeInsertChild(chatRoot.get(), messages.get(), 1);
        YGNodeInsertChild(chatRoot.get(), composer.get(), 2);
        YGNodeCalculateLayout(chatRoot.get(), l.chat.w, l.chat.h, YGDirectionLTR);

        l.chatHeader = layoutRect(header.get(), l.chat.x, l.chat.y);
        l.messages = layoutRect(messages.get(), l.chat.x, l.chat.y);
        l.composer = layoutRect(composer.get(), l.chat.x, l.chat.y);

        YogaNode messageRoot;
        YogaNode today;
        YogaNode askRow;
        YogaNode askBubble;
        YogaNode askTime;
        YogaNode uploadRow;
        YogaNode uploadBubble;
        YogaNode uploadTime;
        YogaNode uploadCheck;
        YogaNode reportCard;
        YogaNode thanksRow;
        YogaNode thanksBubble;
        YogaNode thanksTime;
        YogaNode designCard;
        YogaNode finalRow;
        YogaNode finalBubble;
        YogaNode finalTime;
        YogaNode finalCheck;
        YGNodeStyleSetWidth(messageRoot.get(), l.messages.w);
        YGNodeStyleSetHeight(messageRoot.get(), 700.0f);
        YGNodeStyleSetFlexDirection(messageRoot.get(), YGFlexDirectionColumn);
        YGNodeStyleSetPadding(messageRoot.get(), YGEdgeLeft, 30.0f);
        YGNodeStyleSetPadding(messageRoot.get(), YGEdgeRight, 30.0f);
        YGNodeStyleSetPadding(messageRoot.get(), YGEdgeTop, 16.0f);

        setMessageNode(today.get(), 62.0f, 32.0f, false, 10.0f);
        YGNodeStyleSetAlignSelf(today.get(), YGAlignCenter);

        setNodeSize(askRow.get(), l.messages.w - 60.0f, 51.0f);
        YGNodeStyleSetFlexDirection(askRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(askRow.get(), YGAlignCenter);
        YGNodeStyleSetMargin(askRow.get(), YGEdgeBottom, 12.0f);
        setNodeSize(askBubble.get(), 264.0f, 51.0f);
        setNodeSize(askTime.get(), 48.0f, 24.0f);
        YGNodeStyleSetMargin(askTime.get(), YGEdgeLeft, 12.0f);

        setNodeSize(uploadRow.get(), l.messages.w - 60.0f, 51.0f);
        YGNodeStyleSetFlexDirection(uploadRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetJustifyContent(uploadRow.get(), YGJustifyFlexEnd);
        YGNodeStyleSetAlignItems(uploadRow.get(), YGAlignCenter);
        YGNodeStyleSetMargin(uploadRow.get(), YGEdgeBottom, 18.0f);
        setNodeSize(uploadBubble.get(), 171.0f, 51.0f);
        setNodeSize(uploadTime.get(), 41.0f, 24.0f);
        setNodeSize(uploadCheck.get(), 16.0f, 16.0f);
        YGNodeStyleSetMargin(uploadTime.get(), YGEdgeLeft, 12.0f);
        YGNodeStyleSetMargin(uploadCheck.get(), YGEdgeLeft, 8.0f);

        setMessageNode(
            reportCard.get(),
            std::min(clampf(l.messages.w - 160.0f, 360.0f, 600.0f), std::max(240.0f, l.messages.w - 60.0f)),
            141.0f,
            true,
            18.0f);

        setNodeSize(thanksRow.get(), l.messages.w - 60.0f, 51.0f);
        YGNodeStyleSetFlexDirection(thanksRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(thanksRow.get(), YGAlignCenter);
        YGNodeStyleSetMargin(thanksRow.get(), YGEdgeBottom, 20.0f);
        setNodeSize(thanksBubble.get(), 241.0f, 51.0f);
        setNodeSize(thanksTime.get(), 48.0f, 24.0f);
        YGNodeStyleSetMargin(thanksTime.get(), YGEdgeLeft, 12.0f);

        setMessageNode(
            designCard.get(),
            std::min(clampf(l.messages.w - 90.0f, 380.0f, 668.0f), std::max(260.0f, l.messages.w - 60.0f)),
            137.0f,
            false,
            20.0f);

        setNodeSize(finalRow.get(), l.messages.w - 60.0f, 51.0f);
        YGNodeStyleSetFlexDirection(finalRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetJustifyContent(finalRow.get(), YGJustifyFlexEnd);
        YGNodeStyleSetAlignItems(finalRow.get(), YGAlignCenter);
        setNodeSize(finalBubble.get(), 222.0f, 51.0f);
        setNodeSize(finalTime.get(), 41.0f, 24.0f);
        setNodeSize(finalCheck.get(), 16.0f, 16.0f);
        YGNodeStyleSetMargin(finalTime.get(), YGEdgeLeft, 12.0f);
        YGNodeStyleSetMargin(finalCheck.get(), YGEdgeLeft, 8.0f);

        YGNodeInsertChild(askRow.get(), askBubble.get(), 0);
        YGNodeInsertChild(askRow.get(), askTime.get(), 1);
        YGNodeInsertChild(uploadRow.get(), uploadBubble.get(), 0);
        YGNodeInsertChild(uploadRow.get(), uploadTime.get(), 1);
        YGNodeInsertChild(uploadRow.get(), uploadCheck.get(), 2);
        YGNodeInsertChild(thanksRow.get(), thanksBubble.get(), 0);
        YGNodeInsertChild(thanksRow.get(), thanksTime.get(), 1);
        YGNodeInsertChild(finalRow.get(), finalBubble.get(), 0);
        YGNodeInsertChild(finalRow.get(), finalTime.get(), 1);
        YGNodeInsertChild(finalRow.get(), finalCheck.get(), 2);

        YGNodeInsertChild(messageRoot.get(), today.get(), 0);
        YGNodeInsertChild(messageRoot.get(), askRow.get(), 1);
        YGNodeInsertChild(messageRoot.get(), uploadRow.get(), 2);
        YGNodeInsertChild(messageRoot.get(), reportCard.get(), 3);
        YGNodeInsertChild(messageRoot.get(), thanksRow.get(), 4);
        YGNodeInsertChild(messageRoot.get(), designCard.get(), 5);
        YGNodeInsertChild(messageRoot.get(), finalRow.get(), 6);
        YGNodeCalculateLayout(messageRoot.get(), l.messages.w, 700.0f, YGDirectionLTR);

        l.msgToday = layoutRect(today.get(), l.messages.x, l.messages.y);
        l.msgAskBubble = childLayoutRect(askRow.get(), askBubble.get(), l.messages);
        l.msgAskTime = childLayoutRect(askRow.get(), askTime.get(), l.messages);
        l.msgUploadBubble = childLayoutRect(uploadRow.get(), uploadBubble.get(), l.messages);
        l.msgUploadTime = childLayoutRect(uploadRow.get(), uploadTime.get(), l.messages);
        l.msgUploadCheck = childLayoutRect(uploadRow.get(), uploadCheck.get(), l.messages);
        l.msgReportCard = layoutRect(reportCard.get(), l.messages.x, l.messages.y);
        l.msgThanksBubble = childLayoutRect(thanksRow.get(), thanksBubble.get(), l.messages);
        l.msgThanksTime = childLayoutRect(thanksRow.get(), thanksTime.get(), l.messages);
        l.msgDesignCard = layoutRect(designCard.get(), l.messages.x, l.messages.y);
        l.msgFinalBubble = childLayoutRect(finalRow.get(), finalBubble.get(), l.messages);
        l.msgFinalTime = childLayoutRect(finalRow.get(), finalTime.get(), l.messages);
        l.msgFinalCheck = childLayoutRect(finalRow.get(), finalCheck.get(), l.messages);
        l.msgScrollbar = {l.messages.x + l.messages.w - 14.0f, l.messages.y + 14.0f, 6.0f, 342.0f};

        YogaNode composerRoot;
        YogaNode input;
        YogaNode textSlot;
        YogaNode smile;
        YogaNode paperclip;
        YogaNode folder;
        YogaNode send;
        YGNodeStyleSetWidth(composerRoot.get(), l.composer.w);
        YGNodeStyleSetHeight(composerRoot.get(), l.composer.h);
        YGNodeStyleSetFlexDirection(composerRoot.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(composerRoot.get(), YGAlignCenter);
        YGNodeStyleSetPadding(composerRoot.get(), YGEdgeLeft, 26.0f);
        YGNodeStyleSetPadding(composerRoot.get(), YGEdgeRight, 26.0f);

        YGNodeStyleSetHeight(input.get(), 78.0f);
        YGNodeStyleSetFlexGrow(input.get(), 1.0f);
        YGNodeStyleSetFlexShrink(input.get(), 1.0f);
        YGNodeStyleSetMinWidth(input.get(), 280.0f);
        YGNodeStyleSetFlexDirection(input.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(input.get(), YGAlignCenter);
        YGNodeStyleSetPadding(input.get(), YGEdgeLeft, 16.0f);
        YGNodeStyleSetPadding(input.get(), YGEdgeRight, 10.0f);

        YGNodeStyleSetFlexGrow(textSlot.get(), 1.0f);
        YGNodeStyleSetFlexShrink(textSlot.get(), 1.0f);
        YGNodeStyleSetMinWidth(textSlot.get(), 120.0f);

        for (YGNodeRef icon : {smile.get(), paperclip.get(), folder.get()}) {
            YGNodeStyleSetWidth(icon, 38.0f);
            YGNodeStyleSetHeight(icon, 46.0f);
            YGNodeStyleSetMargin(icon, YGEdgeLeft, 22.0f);
            YGNodeStyleSetFlexShrink(icon, 0.0f);
        }

        YGNodeStyleSetWidth(send.get(), 84.0f);
        YGNodeStyleSetHeight(send.get(), 46.0f);
        YGNodeStyleSetMargin(send.get(), YGEdgeLeft, 12.0f);
        YGNodeStyleSetFlexShrink(send.get(), 0.0f);

        YGNodeInsertChild(input.get(), textSlot.get(), 0);
        YGNodeInsertChild(input.get(), smile.get(), 1);
        YGNodeInsertChild(input.get(), paperclip.get(), 2);
        YGNodeInsertChild(input.get(), folder.get(), 3);
        YGNodeInsertChild(input.get(), send.get(), 4);
        YGNodeInsertChild(composerRoot.get(), input.get(), 0);
        YGNodeCalculateLayout(composerRoot.get(), l.composer.w, l.composer.h, YGDirectionLTR);

        l.composerInput = layoutRect(input.get(), l.composer.x, l.composer.y);
        l.composerSend = layoutRect(send.get(), l.composerInput.x, l.composerInput.y);
        l.composerSmileX =
            l.composerInput.x + YGNodeLayoutGetLeft(smile.get()) + YGNodeLayoutGetWidth(smile.get()) * 0.5f;
        l.composerPaperclipX =
            l.composerInput.x + YGNodeLayoutGetLeft(paperclip.get()) + YGNodeLayoutGetWidth(paperclip.get()) * 0.5f;
        l.composerFolderX =
            l.composerInput.x + YGNodeLayoutGetLeft(folder.get()) + YGNodeLayoutGetWidth(folder.get()) * 0.5f;

        l.sidebarW = l.sidebar.w;
        l.chatLeft = l.chat.x;
        l.chatRight = l.chat.x + l.chat.w;
        l.chatW = l.chat.w;
        l.inspectorLeft = l.inspector.x;
        l.inspectorW = l.inspector.w;
        l.composerTop = l.composer.y;

        return l;
    }

    void drawApp(SkCanvas& c, const Layout& l) {
        drawSidebar(c, l);
        drawConversation(c, l);
        drawInspector(c, l);
    }

    void drawSidebar(SkCanvas& c, const Layout& l) {
        c.drawRect(l.sidebar.sk(), fill(rgb(252, 254, 255)));
        line(c,
             l.sidebar.x + l.sidebar.w - 0.5f,
             l.sidebar.y,
             l.sidebar.x + l.sidebar.w - 0.5f,
             l.sidebar.y + l.sidebar.h,
             rgb(216, 222, 229));

        YogaNode root;
        YogaNode profile;
        YogaNode avatar;
        YogaNode identity;
        YogaNode gear;
        YogaNode searchNode;
        YogaNode onlineLabel;
        std::array<YogaNode, 5> onlineRows;
        YogaNode offlineLabel;
        std::array<YogaNode, 3> offlineRows;

        YGNodeStyleSetWidth(root.get(), l.sidebar.w);
        YGNodeStyleSetHeight(root.get(), l.sidebar.h);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionColumn);

        setNodeSize(profile.get(), l.sidebar.w - 44.0f, 43.0f);
        YGNodeStyleSetFlexDirection(profile.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(profile.get(), YGAlignCenter);
        YGNodeStyleSetMargin(profile.get(), YGEdgeLeft, 22.0f);
        YGNodeStyleSetMargin(profile.get(), YGEdgeTop, 19.0f);
        setNodeSize(avatar.get(), 43.0f, 43.0f);
        YGNodeStyleSetWidth(identity.get(), 160.0f);
        YGNodeStyleSetFlexGrow(identity.get(), 1.0f);
        YGNodeStyleSetFlexShrink(identity.get(), 1.0f);
        YGNodeStyleSetMargin(identity.get(), YGEdgeLeft, 15.0f);
        setNodeSize(gear.get(), 28.0f, 28.0f);

        setNodeSize(searchNode.get(), l.sidebar.w - 46.0f, 46.0f);
        YGNodeStyleSetMargin(searchNode.get(), YGEdgeLeft, 23.0f);
        YGNodeStyleSetMargin(searchNode.get(), YGEdgeTop, 17.0f);

        setNodeSize(onlineLabel.get(), l.sidebar.w - 78.0f, 24.0f);
        YGNodeStyleSetMargin(onlineLabel.get(), YGEdgeLeft, 39.0f);
        YGNodeStyleSetMargin(onlineLabel.get(), YGEdgeTop, 36.0f);
        YGNodeStyleSetMargin(onlineLabel.get(), YGEdgeBottom, 12.0f);

        for (auto& row : onlineRows) {
            setNodeSize(row.get(), l.sidebar.w - 40.0f, 81.0f);
            YGNodeStyleSetMargin(row.get(), YGEdgeLeft, 20.0f);
        }

        setNodeSize(offlineLabel.get(), l.sidebar.w - 78.0f, 24.0f);
        YGNodeStyleSetMargin(offlineLabel.get(), YGEdgeLeft, 39.0f);
        YGNodeStyleSetMargin(offlineLabel.get(), YGEdgeTop, 34.0f);
        YGNodeStyleSetMargin(offlineLabel.get(), YGEdgeBottom, 12.0f);

        for (auto& row : offlineRows) {
            setNodeSize(row.get(), l.sidebar.w - 40.0f, 78.0f);
            YGNodeStyleSetMargin(row.get(), YGEdgeLeft, 20.0f);
        }

        YGNodeInsertChild(profile.get(), avatar.get(), 0);
        YGNodeInsertChild(profile.get(), identity.get(), 1);
        YGNodeInsertChild(profile.get(), gear.get(), 2);
        YGNodeInsertChild(root.get(), profile.get(), 0);
        YGNodeInsertChild(root.get(), searchNode.get(), 1);
        YGNodeInsertChild(root.get(), onlineLabel.get(), 2);
        for (size_t i = 0; i < onlineRows.size(); ++i) {
            YGNodeInsertChild(root.get(), onlineRows[i].get(), 3 + i);
        }
        YGNodeInsertChild(root.get(), offlineLabel.get(), 8);
        for (size_t i = 0; i < offlineRows.size(); ++i) {
            YGNodeInsertChild(root.get(), offlineRows[i].get(), 9 + i);
        }
        YGNodeCalculateLayout(root.get(), l.sidebar.w, l.sidebar.h, YGDirectionLTR);

        const Rect profileRect = layoutRect(profile.get(), l.sidebar.x, l.sidebar.y);
        const Rect avatarRect = childLayoutRect(profile.get(), avatar.get(), l.sidebar);
        const Rect identityRect = childLayoutRect(profile.get(), identity.get(), l.sidebar);
        const Rect gearRect = childLayoutRect(profile.get(), gear.get(), l.sidebar);
        const Rect search = layoutRect(searchNode.get(), l.sidebar.x, l.sidebar.y);
        const Rect onlineLabelRect = layoutRect(onlineLabel.get(), l.sidebar.x, l.sidebar.y);
        const Rect offlineLabelRect = layoutRect(offlineLabel.get(), l.sidebar.x, l.sidebar.y);

        c.drawRRect(rr(avatarRect, 12), fill(rgb(0, 157, 153)));
        centeredText(c, "林", avatarRect, 23, SK_ColorWHITE, true);
        text(c, "林一凡", identityRect.x, profileRect.y + 17.0f, 18, rgb(13, 17, 23), true);
        text(c, "本机 · 192.168.1.8", identityRect.x, profileRect.y + 38.0f, 14, rgb(64, 74, 88));
        drawGear(c, gearRect.x + gearRect.w * 0.5f, gearRect.y + gearRect.h * 0.5f, 12, rgb(90, 100, 115));

        c.drawRRect(rr(search, 7), fill(SK_ColorWHITE));
        c.drawRRect(rr(search, 7), stroke(rgb(205, 214, 224)));
        drawSearch(c, search.x + 27.0f, search.y + 23.0f, 10, rgb(24, 29, 38), 2.0f);
        text(c, "搜索设备", search.x + 55.0f, search.y + 29.0f, 14, rgb(136, 148, 162));

        text(c, "在线设备 (5)", onlineLabelRect.x, onlineLabelRect.y + 15.0f, 15, rgb(18, 23, 31), true);

        std::vector<Device> online = {
            {"Alex-PC", "192.168.1.24", true},
            {"DESKTOP-J8K2TQ", "192.168.1.31", true},
            {"LAPTOP-9F3V2M", "192.168.1.42", true},
            {"DEV-SERVER", "192.168.1.10", true},
            {"MARK-PC", "192.168.1.77", true},
        };

        for (size_t i = 0; i < online.size(); ++i) {
            drawDeviceRow(c, online[i], layoutRect(onlineRows[i].get(), l.sidebar.x, l.sidebar.y), i == 0);
        }

        text(c, "离线设备 (3)", offlineLabelRect.x, offlineLabelRect.y + 15.0f, 15, rgb(18, 23, 31), true);
        std::vector<Device> offline = {
            {"FINANCE-PC", "192.168.1.15", false},
            {"HR-LAPTOP", "192.168.1.28", false},
            {"OLD-PC", "192.168.1.55", false},
        };
        for (size_t i = 0; i < offline.size(); ++i) {
            drawDeviceRow(c, offline[i], layoutRect(offlineRows[i].get(), l.sidebar.x, l.sidebar.y), false);
        }
    }

    void drawDeviceRow(SkCanvas& c, const Device& d, const Rect& row, bool selected) {
        if (selected) {
            c.drawRRect(rr(row, 7), fill(rgb(235, 254, 254)));
            c.drawRRect(rr(row, 7), stroke(rgb(118, 214, 213), 1.0f));
        }

        YogaNode root;
        YogaNode dot;
        YogaNode monitor;
        YogaNode label;
        YGNodeStyleSetWidth(root.get(), row.w);
        YGNodeStyleSetHeight(root.get(), row.h);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(root.get(), YGAlignCenter);
        YGNodeStyleSetPadding(root.get(), YGEdgeLeft, 15.0f);
        setNodeSize(dot.get(), 11.0f, 11.0f);
        YGNodeStyleSetMargin(dot.get(), YGEdgeRight, 17.0f);
        setNodeSize(monitor.get(), 36.0f, 36.0f);
        YGNodeStyleSetMargin(monitor.get(), YGEdgeRight, 17.0f);
        YGNodeStyleSetFlexGrow(label.get(), 1.0f);
        YGNodeStyleSetFlexShrink(label.get(), 1.0f);
        YGNodeStyleSetHeight(label.get(), 44.0f);

        YGNodeInsertChild(root.get(), dot.get(), 0);
        YGNodeInsertChild(root.get(), monitor.get(), 1);
        YGNodeInsertChild(root.get(), label.get(), 2);
        YGNodeCalculateLayout(root.get(), row.w, row.h, YGDirectionLTR);

        const Rect dotRect = layoutRect(dot.get(), row.x, row.y);
        const Rect monitorRect = layoutRect(monitor.get(), row.x, row.y);
        const Rect labelRect = layoutRect(label.get(), row.x, row.y);
        c.drawCircle(dotRect.x + dotRect.w * 0.5f, dotRect.y + dotRect.h * 0.5f, 5.3f, fill(d.online ? rgb(45, 181, 36) : rgb(157, 168, 181)));
        drawMonitor(c, monitorRect.x + monitorRect.w * 0.5f, monitorRect.y + monitorRect.h * 0.5f, 18, rgb(13, 17, 23));
        text(c, d.name, labelRect.x, labelRect.y + 18.0f, 15.5f, rgb(19, 25, 34), true);
        text(c, d.ip, labelRect.x, labelRect.y + 41.0f, 13.2f, rgb(53, 63, 78));
    }

    void drawConversation(SkCanvas& c, const Layout& l) {
        c.drawRect(l.chat.sk(), fill(SK_ColorWHITE));
        line(c,
             l.chat.x,
             l.chatHeader.y + l.chatHeader.h - 0.5f,
             l.chat.x + l.chat.w,
             l.chatHeader.y + l.chatHeader.h - 0.5f,
             rgb(217, 223, 230));
        line(c, l.chat.x, l.composer.y - 0.5f, l.chat.x + l.chat.w, l.composer.y - 0.5f, rgb(217, 223, 230));
        if (l.showInspector) {
            line(c,
                 l.chat.x + l.chat.w + 0.5f,
                 l.chat.y,
                 l.chat.x + l.chat.w + 0.5f,
                 l.chat.y + l.chat.h,
                 rgb(216, 222, 229));
        }

        drawChatHeader(c, l);
        drawTabs(c, l);

        c.save();
        c.clipRect(l.messages.sk());
        drawMessages(c, l);
        c.restore();

        drawComposer(c, l);
    }

    void drawChatHeader(SkCanvas& c, const Layout& l) {
        YogaNode root;
        YogaNode monitor;
        YogaNode info;
        YogaNode spacer;
        YogaNode phone;
        YogaNode search;
        YogaNode more;
        YGNodeStyleSetWidth(root.get(), l.chatHeader.w);
        YGNodeStyleSetHeight(root.get(), 86.0f);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(root.get(), YGAlignCenter);
        YGNodeStyleSetPadding(root.get(), YGEdgeLeft, 31.0f);
        YGNodeStyleSetPadding(root.get(), YGEdgeRight, 24.0f);

        setNodeSize(monitor.get(), 52.0f, 52.0f);
        YGNodeStyleSetMargin(monitor.get(), YGEdgeRight, 18.0f);
        setNodeSize(info.get(), 180.0f, 48.0f);
        YGNodeStyleSetFlexGrow(spacer.get(), 1.0f);
        YGNodeStyleSetFlexShrink(spacer.get(), 1.0f);
        for (YGNodeRef action : {phone.get(), search.get(), more.get()}) {
            setNodeSize(action, 50.0f, 52.0f);
            YGNodeStyleSetMargin(action, YGEdgeLeft, 26.0f);
        }

        YGNodeInsertChild(root.get(), monitor.get(), 0);
        YGNodeInsertChild(root.get(), info.get(), 1);
        YGNodeInsertChild(root.get(), spacer.get(), 2);
        YGNodeInsertChild(root.get(), phone.get(), 3);
        YGNodeInsertChild(root.get(), search.get(), 4);
        YGNodeInsertChild(root.get(), more.get(), 5);
        YGNodeCalculateLayout(root.get(), l.chatHeader.w, 86.0f, YGDirectionLTR);

        const Rect monitorRect = layoutRect(monitor.get(), l.chatHeader.x, l.chatHeader.y);
        const Rect infoRect = layoutRect(info.get(), l.chatHeader.x, l.chatHeader.y);
        const Rect phoneRect = layoutRect(phone.get(), l.chatHeader.x, l.chatHeader.y);
        const Rect searchRect = layoutRect(search.get(), l.chatHeader.x, l.chatHeader.y);
        const Rect moreRect = layoutRect(more.get(), l.chatHeader.x, l.chatHeader.y);

        drawMonitor(c, monitorRect.x + monitorRect.w * 0.5f, monitorRect.y + monitorRect.h * 0.5f, 26, rgb(13, 17, 23));
        text(c, "Alex-PC", infoRect.x, infoRect.y + 19.0f, 21, rgb(18, 23, 31), true);
        c.drawCircle(infoRect.x - 11.0f, infoRect.y + 38.0f, 5.8f, fill(rgb(45, 181, 36)));
        text(c, "192.168.1.24", infoRect.x + 5.0f, infoRect.y + 43.0f, 13.5f, rgb(53, 63, 78));

        drawPhone(c, phoneRect.x + phoneRect.w * 0.5f, phoneRect.y + phoneRect.h * 0.5f, 16, rgb(8, 12, 20));
        drawSearch(c, searchRect.x + searchRect.w * 0.5f, searchRect.y + searchRect.h * 0.5f, 14, rgb(8, 12, 20), 3.4f);
        drawMore(c, moreRect.x + moreRect.w * 0.5f, moreRect.y + moreRect.h * 0.5f - 1.0f, rgb(8, 12, 20));
    }

    void drawTabs(SkCanvas& c, const Layout& l) {
        YogaNode root;
        YogaNode outerNode;
        std::array<YogaNode, 3> segments;
        const float tabW = std::min(clampf(l.chatW - 190.0f, 300.0f, 542.0f), std::max(180.0f, l.chatW - 62.0f));
        YGNodeStyleSetWidth(root.get(), l.chatHeader.w);
        YGNodeStyleSetHeight(root.get(), 62.0f);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionColumn);
        YGNodeStyleSetPadding(root.get(), YGEdgeLeft, 31.0f);
        setNodeSize(outerNode.get(), tabW, 39.0f);
        YGNodeStyleSetFlexDirection(outerNode.get(), YGFlexDirectionRow);
        YGNodeStyleSetMargin(outerNode.get(), YGEdgeTop, 8.0f);
        for (auto& segment : segments) {
            YGNodeStyleSetFlexGrow(segment.get(), 1.0f);
            YGNodeStyleSetFlexBasis(segment.get(), 0.0f);
            YGNodeStyleSetHeight(segment.get(), 39.0f);
        }
        for (size_t i = 0; i < segments.size(); ++i) {
            YGNodeInsertChild(outerNode.get(), segments[i].get(), i);
        }
        YGNodeInsertChild(root.get(), outerNode.get(), 0);
        YGNodeCalculateLayout(root.get(), l.chatHeader.w, 62.0f, YGDirectionLTR);

        const Rect outer = layoutRect(outerNode.get(), l.chatHeader.x, l.chatHeader.y + 86.0f);
        const Rect first = childLayoutRect(outerNode.get(), segments[0].get(), {l.chatHeader.x, l.chatHeader.y + 86.0f, 0.0f, 0.0f});
        const Rect second = childLayoutRect(outerNode.get(), segments[1].get(), {l.chatHeader.x, l.chatHeader.y + 86.0f, 0.0f, 0.0f});
        const Rect third = childLayoutRect(outerNode.get(), segments[2].get(), {l.chatHeader.x, l.chatHeader.y + 86.0f, 0.0f, 0.0f});

        c.drawRRect(rr(outer, 6), fill(SK_ColorWHITE));
        c.drawRRect(rr(outer, 6), stroke(rgb(202, 211, 222)));
        c.drawRRect(rr(first, 6), fill(rgb(0, 157, 153)));
        c.drawRect(SkRect::MakeXYWH(first.x + first.w - 8, first.y, 8, first.h), fill(rgb(0, 157, 153)));
        line(c, second.x, outer.y, second.x, outer.y + outer.h, rgb(202, 211, 222));
        line(c, third.x, outer.y, third.x, outer.y + outer.h, rgb(202, 211, 222));
        centeredText(c, "聊天", first, 14, SK_ColorWHITE, true);
        centeredText(c, "历史", second, 14, rgb(70, 79, 94), true);
        centeredText(c, "传输", third, 14, rgb(70, 79, 94), true);
    }

    void drawMessages(SkCanvas& c, const Layout& l) {
        c.drawRRect(rr(l.msgToday, 7), fill(SK_ColorWHITE));
        c.drawRRect(rr(l.msgToday, 7), stroke(rgb(205, 214, 224)));
        centeredText(c, "今天", l.msgToday, 12.8f, rgb(90, 100, 115), true);

        drawBubble(c, l.msgAskBubble, "能把最新的 Q2 报告发我吗?", false);
        text(c, "09:21", l.msgAskTime.x, l.msgAskTime.y + 16, 12, rgb(86, 98, 115));

        drawBubble(c, l.msgUploadBubble, "可以，正在上传。", true);
        text(c, "09:21", l.msgUploadTime.x, l.msgUploadTime.y + 16, 12, rgb(86, 98, 115));
        drawCheck(c, l.msgUploadCheck.x, l.msgUploadCheck.y + 10, rgb(0, 157, 153));

        drawTransferCard(c, l.msgReportCard, true);

        drawBubble(c, l.msgThanksBubble, "谢谢! 设计说明也在吗?", false);
        text(c, "09:23", l.msgThanksTime.x, l.msgThanksTime.y + 16, 12, rgb(86, 98, 115));

        drawTransferCard(c, l.msgDesignCard, false);

        drawBubble(c, l.msgFinalBubble, "是的，这是最新版本。", true);
        text(c, "09:25", l.msgFinalTime.x, l.msgFinalTime.y + 16, 12, rgb(86, 98, 115));
        drawCheck(c, l.msgFinalCheck.x, l.msgFinalCheck.y + 10, rgb(0, 157, 153));

        c.drawRoundRect(l.msgScrollbar.sk(), 3, 3, fill(rgb(180, 190, 202)));
    }

    void drawBubble(SkCanvas& c, const Rect& r, std::string_view value, bool outgoing) {
        const SkColor bg = outgoing ? rgb(229, 251, 251) : rgb(255, 255, 255);
        const SkColor border = outgoing ? rgb(153, 220, 220) : rgb(203, 212, 223);
        c.drawRRect(rr(r, 7), fill(bg));
        c.drawRRect(rr(r, 7), stroke(border));
        text(c, value, r.x + 16, r.y + 31, 14.2f, rgb(18, 23, 31), true);
    }

    void drawTransferCard(SkCanvas& c, const Rect& r, bool teal) {
        const SkColor accent = teal ? rgb(0, 157, 153) : rgb(230, 143, 0);
        const SkColor bg = teal ? rgb(235, 254, 254) : rgb(255, 248, 231);
        const SkColor border = teal ? rgb(123, 212, 211) : rgb(255, 190, 93);
        c.drawRRect(rr(r, 7), fill(bg));
        c.drawRRect(rr(r, 7), stroke(border));

        const std::string name = teal ? "Q2_Report_2024.pdf" : "Design_Specs_v2.zip";
        const std::string size = teal ? "24.8 MB" : "112.6 MB";
        const std::string percent = teal ? "78%" : "45%";
        const std::string detail = teal ? "19.4 MB / 24.8 MB  -  5.2 MB/s  -  剩余 00:00:01"
                                        : "50.7 MB / 112.6 MB  -  4.1 MB/s  -  剩余 00:00:15";
        const std::string time = teal ? "09:22" : "09:24";
        const float progress = teal ? 0.78f : 0.45f;

        YogaNode root;
        YogaNode topRow;
        YogaNode icon;
        YogaNode content;
        YogaNode percentNode;
        YogaNode progressNode;
        YogaNode bottomRow;
        YogaNode detailNode;
        YogaNode timeNode;
        YogaNode checkNode;
        YGNodeStyleSetWidth(root.get(), r.w);
        YGNodeStyleSetHeight(root.get(), r.h);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionColumn);
        YGNodeStyleSetPadding(root.get(), YGEdgeLeft, 25.0f);
        YGNodeStyleSetPadding(root.get(), YGEdgeRight, 38.0f);
        YGNodeStyleSetPadding(root.get(), YGEdgeTop, 28.0f);

        setNodeSize(topRow.get(), r.w - 63.0f, 47.0f);
        YGNodeStyleSetFlexDirection(topRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(topRow.get(), YGAlignFlexStart);
        setNodeSize(icon.get(), 35.0f, 35.0f);
        YGNodeStyleSetMargin(icon.get(), YGEdgeRight, 42.0f);
        YGNodeStyleSetFlexGrow(content.get(), 1.0f);
        YGNodeStyleSetFlexShrink(content.get(), 1.0f);
        YGNodeStyleSetHeight(content.get(), 47.0f);
        setNodeSize(percentNode.get(), 48.0f, 20.0f);
        YGNodeStyleSetMargin(percentNode.get(), YGEdgeTop, 30.0f);

        setNodeSize(progressNode.get(), std::max(80.0f, r.w - 122.0f), 6.0f);
        YGNodeStyleSetMargin(progressNode.get(), YGEdgeLeft, 77.0f);
        YGNodeStyleSetMargin(progressNode.get(), YGEdgeTop, 10.0f);

        setNodeSize(bottomRow.get(), std::max(80.0f, r.w - 122.0f), 24.0f);
        YGNodeStyleSetFlexDirection(bottomRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(bottomRow.get(), YGAlignCenter);
        YGNodeStyleSetMargin(bottomRow.get(), YGEdgeLeft, 77.0f);
        YGNodeStyleSetMargin(bottomRow.get(), YGEdgeTop, 16.0f);
        YGNodeStyleSetFlexGrow(detailNode.get(), 1.0f);
        YGNodeStyleSetFlexShrink(detailNode.get(), 1.0f);
        YGNodeStyleSetHeight(detailNode.get(), 24.0f);
        setNodeSize(timeNode.get(), 44.0f, 24.0f);
        YGNodeStyleSetMargin(timeNode.get(), YGEdgeLeft, 8.0f);
        setNodeSize(checkNode.get(), 16.0f, 16.0f);
        YGNodeStyleSetMargin(checkNode.get(), YGEdgeLeft, 8.0f);

        YGNodeInsertChild(topRow.get(), icon.get(), 0);
        YGNodeInsertChild(topRow.get(), content.get(), 1);
        YGNodeInsertChild(topRow.get(), percentNode.get(), 2);
        YGNodeInsertChild(bottomRow.get(), detailNode.get(), 0);
        YGNodeInsertChild(bottomRow.get(), timeNode.get(), 1);
        YGNodeInsertChild(bottomRow.get(), checkNode.get(), 2);
        YGNodeInsertChild(root.get(), topRow.get(), 0);
        YGNodeInsertChild(root.get(), progressNode.get(), 1);
        YGNodeInsertChild(root.get(), bottomRow.get(), 2);
        YGNodeCalculateLayout(root.get(), r.w, r.h, YGDirectionLTR);

        const Rect iconRect = childLayoutRect(topRow.get(), icon.get(), r);
        const Rect contentRect = childLayoutRect(topRow.get(), content.get(), r);
        const Rect percentRect = childLayoutRect(topRow.get(), percentNode.get(), r);
        const Rect progressRect = layoutRect(progressNode.get(), r.x, r.y);
        const Rect detailRect = childLayoutRect(bottomRow.get(), detailNode.get(), r);
        const Rect timeRect = childLayoutRect(bottomRow.get(), timeNode.get(), r);
        const Rect checkRect = childLayoutRect(bottomRow.get(), checkNode.get(), r);

        drawFileIcon(c, iconRect.x, iconRect.y, accent);
        text(c, name, contentRect.x, contentRect.y + 10.0f, 14.2f, rgb(18, 23, 31), true);
        text(c, size, contentRect.x, contentRect.y + 42.0f, 12.8f, rgb(65, 76, 92), true);
        text(c, percent, percentRect.x, percentRect.y + 12.0f, 12.8f, rgb(18, 23, 31), true);
        drawProgress(c, progressRect.x, progressRect.y, progressRect.w, progress, accent, 6);
        text(c, detail, detailRect.x, detailRect.y + 16.0f, 12.3f, rgb(80, 92, 108), true);
        text(c, time, timeRect.x, timeRect.y + 16.0f, 12.0f, rgb(80, 92, 108));
        drawCheck(c, checkRect.x, checkRect.y + 10.0f, rgb(0, 157, 153));
    }

    void drawComposer(SkCanvas& c, const Layout& l) {
        const Rect input = l.composerInput;
        c.drawRRect(rr(input, 8), fill(SK_ColorWHITE));
        c.drawRRect(rr(input, 8), stroke(rgb(214, 222, 231)));
        text(c, "给 Alex-PC 发送消息", input.x + 16, input.y + 36, 14, rgb(139, 150, 165));

        drawSmile(c, l.composerSmileX, l.composerInput.y + 39, 14, rgb(8, 12, 20));
        drawPaperclip(c, l.composerPaperclipX, l.composerInput.y + 39, rgb(8, 12, 20));
        drawFolder(c, l.composerFolderX, l.composerInput.y + 40, rgb(8, 12, 20));
        c.drawRRect(rr(l.composerSend, 7), fill(rgb(0, 157, 153)));
        centeredText(c, "发送", l.composerSend, 15, SK_ColorWHITE, true);
    }

    void drawInspector(SkCanvas& c, const Layout& l) {
        if (!l.showInspector) {
            return;
        }

        const float x = l.inspector.x;
        const float right = l.inspector.x + l.inspector.w;
        c.drawRect(l.inspector.sk(), fill(SK_ColorWHITE));

        YogaNode root;
        YogaNode titleBar;
        YogaNode title;
        YogaNode close;
        YogaNode statusRow;
        YogaNode monitor;
        YogaNode statusText;
        YogaNode infoSection;
        YogaNode infoTitle;
        std::array<YogaNode, 6> infoRows;
        YogaNode transfersSection;
        YogaNode transferTitle;
        YogaNode transferA;
        YogaNode divider;
        YogaNode transferB;
        YogaNode allTransfers;

        YGNodeStyleSetWidth(root.get(), l.inspector.w);
        YGNodeStyleSetHeight(root.get(), l.inspector.h);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionColumn);

        setNodeSize(titleBar.get(), l.inspector.w, 74.0f);
        YGNodeStyleSetFlexDirection(titleBar.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(titleBar.get(), YGAlignCenter);
        YGNodeStyleSetPadding(titleBar.get(), YGEdgeLeft, 26.0f);
        YGNodeStyleSetPadding(titleBar.get(), YGEdgeRight, 26.0f);
        YGNodeStyleSetFlexGrow(title.get(), 1.0f);
        YGNodeStyleSetFlexShrink(title.get(), 1.0f);
        YGNodeStyleSetHeight(title.get(), 28.0f);
        setNodeSize(close.get(), 30.0f, 30.0f);

        setNodeSize(statusRow.get(), l.inspector.w, 147.0f);
        YGNodeStyleSetFlexDirection(statusRow.get(), YGFlexDirectionRow);
        YGNodeStyleSetAlignItems(statusRow.get(), YGAlignCenter);
        YGNodeStyleSetPadding(statusRow.get(), YGEdgeLeft, 66.0f);
        YGNodeStyleSetPadding(statusRow.get(), YGEdgeRight, 36.0f);
        setNodeSize(monitor.get(), 78.0f, 78.0f);
        YGNodeStyleSetMargin(monitor.get(), YGEdgeRight, 34.0f);
        YGNodeStyleSetFlexGrow(statusText.get(), 1.0f);
        YGNodeStyleSetFlexShrink(statusText.get(), 1.0f);
        YGNodeStyleSetHeight(statusText.get(), 58.0f);

        setNodeSize(infoSection.get(), l.inspector.w, 291.0f);
        YGNodeStyleSetFlexDirection(infoSection.get(), YGFlexDirectionColumn);
        YGNodeStyleSetPadding(infoSection.get(), YGEdgeLeft, 24.0f);
        YGNodeStyleSetPadding(infoSection.get(), YGEdgeRight, 36.0f);
        YGNodeStyleSetPadding(infoSection.get(), YGEdgeTop, 31.0f);
        setNodeSize(infoTitle.get(), l.inspector.w - 60.0f, 20.0f);
        YGNodeStyleSetMargin(infoTitle.get(), YGEdgeBottom, 24.0f);
        for (auto& row : infoRows) {
            setNodeSize(row.get(), l.inspector.w - 60.0f, 35.0f);
        }

        setNodeSize(transfersSection.get(), l.inspector.w, 430.0f);
        YGNodeStyleSetFlexDirection(transfersSection.get(), YGFlexDirectionColumn);
        YGNodeStyleSetPadding(transfersSection.get(), YGEdgeLeft, 24.0f);
        YGNodeStyleSetPadding(transfersSection.get(), YGEdgeRight, 36.0f);
        YGNodeStyleSetPadding(transfersSection.get(), YGEdgeTop, 34.0f);
        setNodeSize(transferTitle.get(), l.inspector.w - 60.0f, 20.0f);
        YGNodeStyleSetMargin(transferTitle.get(), YGEdgeBottom, 20.0f);
        setNodeSize(transferA.get(), l.inspector.w - 54.0f, 90.0f);
        setNodeSize(divider.get(), l.inspector.w - 76.0f, 1.0f);
        YGNodeStyleSetMargin(divider.get(), YGEdgeTop, 0.0f);
        YGNodeStyleSetMargin(divider.get(), YGEdgeBottom, 29.0f);
        setNodeSize(transferB.get(), l.inspector.w - 54.0f, 90.0f);
        YGNodeStyleSetFlexGrow(allTransfers.get(), 1.0f);
        YGNodeStyleSetFlexShrink(allTransfers.get(), 1.0f);
        YGNodeStyleSetMinHeight(allTransfers.get(), 36.0f);

        YGNodeInsertChild(titleBar.get(), title.get(), 0);
        YGNodeInsertChild(titleBar.get(), close.get(), 1);
        YGNodeInsertChild(statusRow.get(), monitor.get(), 0);
        YGNodeInsertChild(statusRow.get(), statusText.get(), 1);
        YGNodeInsertChild(infoSection.get(), infoTitle.get(), 0);
        for (size_t i = 0; i < infoRows.size(); ++i) {
            YGNodeInsertChild(infoSection.get(), infoRows[i].get(), i + 1);
        }
        YGNodeInsertChild(transfersSection.get(), transferTitle.get(), 0);
        YGNodeInsertChild(transfersSection.get(), transferA.get(), 1);
        YGNodeInsertChild(transfersSection.get(), divider.get(), 2);
        YGNodeInsertChild(transfersSection.get(), transferB.get(), 3);
        YGNodeInsertChild(transfersSection.get(), allTransfers.get(), 4);
        YGNodeInsertChild(root.get(), titleBar.get(), 0);
        YGNodeInsertChild(root.get(), statusRow.get(), 1);
        YGNodeInsertChild(root.get(), infoSection.get(), 2);
        YGNodeInsertChild(root.get(), transfersSection.get(), 3);
        YGNodeCalculateLayout(root.get(), l.inspector.w, l.inspector.h, YGDirectionLTR);

        const Rect titleRect = childLayoutRect(titleBar.get(), title.get(), l.inspector);
        const Rect closeRect = childLayoutRect(titleBar.get(), close.get(), l.inspector);
        const Rect monitorRect = childLayoutRect(statusRow.get(), monitor.get(), l.inspector);
        const Rect statusTextRect = childLayoutRect(statusRow.get(), statusText.get(), l.inspector);
        const Rect infoSectionRect = layoutRect(infoSection.get(), l.inspector.x, l.inspector.y);
        const Rect infoTitleRect = childLayoutRect(infoSection.get(), infoTitle.get(), l.inspector);
        const Rect transferSectionRect = layoutRect(transfersSection.get(), l.inspector.x, l.inspector.y);
        const Rect transferTitleRect = childLayoutRect(transfersSection.get(), transferTitle.get(), l.inspector);
        const Rect transferARect = childLayoutRect(transfersSection.get(), transferA.get(), l.inspector);
        const Rect dividerRect = childLayoutRect(transfersSection.get(), divider.get(), l.inspector);
        const Rect transferBRect = childLayoutRect(transfersSection.get(), transferB.get(), l.inspector);
        const Rect allTransfersRect = childLayoutRect(transfersSection.get(), allTransfers.get(), l.inspector);

        text(c, "Alex-PC", titleRect.x, titleRect.y + 18.0f, 18, rgb(18, 23, 31), true);
        drawClose(c, closeRect.x + closeRect.w * 0.5f, closeRect.y + closeRect.h * 0.5f, 15, rgb(8, 12, 20));

        drawMonitor(c, monitorRect.x + monitorRect.w * 0.5f, monitorRect.y + monitorRect.h * 0.5f, 39, rgb(13, 17, 23));
        c.drawCircle(statusTextRect.x - 19.0f, statusTextRect.y + 18.0f, 7, fill(rgb(45, 181, 36)));
        text(c, "在线", statusTextRect.x, statusTextRect.y + 24.0f, 15, rgb(45, 181, 36), true);
        text(c, "192.168.1.24", statusTextRect.x, statusTextRect.y + 58.0f, 14, rgb(18, 23, 31), true);
        line(c, x, infoSectionRect.y - 0.5f, right, infoSectionRect.y - 0.5f, rgb(217, 223, 230));

        text(c, "设备信息", infoTitleRect.x, infoTitleRect.y + 15.0f, 15, rgb(18, 23, 31), true);
        const std::array<std::pair<std::string_view, std::string_view>, 6> rows = {{
            {"电脑名", "Alex-PC"},
            {"用户名", "Alex"},
            {"系统", "Windows 11 Pro 23H2"},
            {"IP 地址", "192.168.1.24"},
            {"MAC 地址", "00-15-5D-8E-2A-7C"},
            {"在线时长", "2天 4时 18分"},
        }};
        for (size_t i = 0; i < rows.size(); ++i) {
            const Rect row = childLayoutRect(infoSection.get(), infoRows[i].get(), l.inspector);
            drawInfoRow(c, row, rows[i].first, rows[i].second);
        }

        line(c, x, transferSectionRect.y - 0.5f, right, transferSectionRect.y - 0.5f, rgb(217, 223, 230));
        text(c, "活跃传输 (2)", transferTitleRect.x, transferTitleRect.y + 15.0f, 15, rgb(18, 23, 31), true);
        drawMiniTransfer(c, transferARect, true);
        line(c, dividerRect.x, dividerRect.y, dividerRect.x + dividerRect.w, dividerRect.y, rgb(224, 230, 237));
        drawMiniTransfer(c, transferBRect, false);
        text(c,
             "查看全部传输",
             allTransfersRect.x,
             std::min(l.inspector.y + l.inspector.h - 36.0f, allTransfersRect.y + allTransfersRect.h - 10.0f),
             14,
             rgb(0, 157, 153),
             true);
    }

    void drawInfoRow(SkCanvas& c, const Rect& row, std::string_view label, std::string_view value) {
        text(c, label, row.x, row.y + 13.0f, 13.2f, rgb(83, 94, 111), true);
        const float w = textWidth(value, 13.2f, true);
        text(c, value, row.x + row.w - w, row.y + 13.0f, 13.2f, rgb(18, 23, 31), true);
    }

    void drawMiniTransfer(SkCanvas& c, const Rect& r, bool teal) {
        const SkColor accent = teal ? rgb(0, 157, 153) : rgb(230, 143, 0);
        const std::string name = teal ? "Q2_Report_2024.pdf" : "Design_Specs_v2.zip";
        const std::string direction = teal ? "发给 Alex-PC" : "来自 Alex-PC";
        const std::string pct = teal ? "78%" : "45%";
        const std::string detail = teal ? "19.4 MB / 24.8 MB  -  5.2 MB/s" : "50.7 MB / 112.6 MB  -  4.1 MB/s";

        YogaNode root;
        YogaNode topRow;
        YogaNode icon;
        YogaNode textBlock;
        YogaNode pctNode;
        YogaNode progressNode;
        YogaNode detailNode;
        YGNodeStyleSetWidth(root.get(), r.w);
        YGNodeStyleSetHeight(root.get(), r.h);
        YGNodeStyleSetFlexDirection(root.get(), YGFlexDirectionColumn);

        setNodeSize(topRow.get(), r.w, 50.0f);
        YGNodeStyleSetFlexDirection(topRow.get(), YGFlexDirectionRow);
        setNodeSize(icon.get(), 30.0f, 30.0f);
        YGNodeStyleSetMargin(icon.get(), YGEdgeRight, 16.0f);
        YGNodeStyleSetFlexGrow(textBlock.get(), 1.0f);
        YGNodeStyleSetFlexShrink(textBlock.get(), 1.0f);
        YGNodeStyleSetHeight(textBlock.get(), 50.0f);
        setNodeSize(pctNode.get(), 38.0f, 20.0f);
        YGNodeStyleSetMargin(pctNode.get(), YGEdgeTop, 30.0f);

        setNodeSize(progressNode.get(), r.w - 2.0f, 5.0f);
        YGNodeStyleSetMargin(progressNode.get(), YGEdgeTop, 7.0f);
        setNodeSize(detailNode.get(), r.w, 24.0f);
        YGNodeStyleSetMargin(detailNode.get(), YGEdgeTop, 14.0f);

        YGNodeInsertChild(topRow.get(), icon.get(), 0);
        YGNodeInsertChild(topRow.get(), textBlock.get(), 1);
        YGNodeInsertChild(topRow.get(), pctNode.get(), 2);
        YGNodeInsertChild(root.get(), topRow.get(), 0);
        YGNodeInsertChild(root.get(), progressNode.get(), 1);
        YGNodeInsertChild(root.get(), detailNode.get(), 2);
        YGNodeCalculateLayout(root.get(), r.w, r.h, YGDirectionLTR);

        const Rect iconRect = childLayoutRect(topRow.get(), icon.get(), r);
        const Rect textRect = childLayoutRect(topRow.get(), textBlock.get(), r);
        const Rect pctRect = childLayoutRect(topRow.get(), pctNode.get(), r);
        const Rect progressRect = layoutRect(progressNode.get(), r.x, r.y);
        const Rect detailRect = layoutRect(detailNode.get(), r.x, r.y);

        drawFileIcon(c, iconRect.x, iconRect.y, accent, 30);
        text(c, name, textRect.x, textRect.y + 10.0f, 13.2f, rgb(18, 23, 31), true);
        text(c, direction, textRect.x, textRect.y + 40.0f, 12.2f, rgb(80, 92, 108), true);
        text(c, pct, pctRect.x + pctRect.w - textWidth(pct, 12.2f, true), pctRect.y + 10.0f, 12.2f, accent, true);
        drawProgress(c, progressRect.x, progressRect.y, progressRect.w, teal ? 0.78f : 0.45f, accent, 5);
        text(c, detail, detailRect.x, detailRect.y + 12.0f, 12.0f, rgb(80, 92, 108), true);
    }

    void drawProgress(SkCanvas& c, float x, float y, float w, float progress, SkColor accent, float h) {
        c.drawRoundRect(SkRect::MakeXYWH(x, y, w, h), h * 0.5f, h * 0.5f, fill(rgb(204, 211, 218)));
        c.drawRoundRect(SkRect::MakeXYWH(x, y, w * clampf(progress, 0, 1), h), h * 0.5f, h * 0.5f, fill(accent));
    }

    void drawMonitor(SkCanvas& c, float cx, float cy, float size, SkColor color) {
        const float w = size * 1.5f;
        const float h = size * 0.98f;
        SkPaint p = fill(color);
        c.drawRoundRect(SkRect::MakeXYWH(cx - w * 0.5f, cy - h * 0.62f, w, h), size * 0.13f, size * 0.13f, p);
        c.drawRect(SkRect::MakeXYWH(cx - w * 0.34f, cy - h * 0.42f, w * 0.68f, h * 0.55f), fill(SK_ColorWHITE));
        c.drawRect(SkRect::MakeXYWH(cx - size * 0.12f, cy + h * 0.34f, size * 0.24f, size * 0.32f), p);
        c.drawRoundRect(SkRect::MakeXYWH(cx - size * 0.55f, cy + h * 0.63f, size * 1.1f, size * 0.16f),
                        size * 0.08f,
                        size * 0.08f,
                        p);
    }

    void drawFileIcon(SkCanvas& c, float x, float y, SkColor color, float size = 35) {
        SkPathBuilder path;
        path.moveTo(x, y);
        path.lineTo(x + size * 0.63f, y);
        path.lineTo(x + size, y + size * 0.34f);
        path.lineTo(x + size, y + size);
        path.lineTo(x, y + size);
        path.close();
        c.drawRoundRect(SkRect::MakeXYWH(x, y, size, size), size * 0.12f, size * 0.12f, fill(color));
        SkPathBuilder fold;
        fold.moveTo(x + size * 0.63f, y);
        fold.lineTo(x + size, y + size * 0.34f);
        fold.lineTo(x + size * 0.63f, y + size * 0.34f);
        fold.close();
        c.drawPath(fold.detach(), fill(rgba(255, 255, 255, 90)));
    }

    void drawSearch(SkCanvas& c, float cx, float cy, float r, SkColor color, float strokeWidth) {
        c.drawCircle(cx, cy, r, stroke(color, strokeWidth));
        line(c, cx + r * 0.68f, cy + r * 0.68f, cx + r * 1.34f, cy + r * 1.34f, color, strokeWidth);
    }

    void drawGear(SkCanvas& c, float cx, float cy, float r, SkColor color) {
        SkPathBuilder gear(SkPathFillType::kEvenOdd);
        constexpr float pi = 3.14159265358979323846f;
        constexpr int toothCount = 8;
        constexpr int pointsPerTooth = 4;
        const float innerRoot = r * 0.66f;
        const float outerTip = r * 0.98f;
        const float step = 2.0f * pi / static_cast<float>(toothCount * pointsPerTooth);

        for (int i = 0; i < toothCount * pointsPerTooth; ++i) {
            const int phase = i % pointsPerTooth;
            const float radius = (phase == 1 || phase == 2) ? outerTip : innerRoot;
            const float angle = -pi * 0.5f + step * static_cast<float>(i);
            const float x = cx + std::cos(angle) * radius;
            const float y = cy + std::sin(angle) * radius;
            if (i == 0) {
                gear.moveTo(x, y);
            } else {
                gear.lineTo(x, y);
            }
        }
        gear.close();
        gear.addOval(SkRect::MakeXYWH(cx - r * 0.32f, cy - r * 0.32f, r * 0.64f, r * 0.64f));

        SkPaint body = fill(color);
        c.drawPath(gear.detach(), body);
        c.drawCircle(cx, cy, r * 0.31f, stroke(rgb(252, 254, 255), 1.5f));
    }

    void drawPhone(SkCanvas& c, float cx, float cy, float size, SkColor color) {
        SkPathBuilder p;
        p.moveTo(cx - size * 0.74f, cy - size * 0.54f);
        p.cubicTo(cx - size * 0.50f, cy + size * 0.42f,
                  cx + size * 0.14f, cy + size * 0.96f,
                  cx + size * 0.76f, cy + size * 0.72f);
        p.cubicTo(cx + size * 0.88f, cy + size * 0.67f,
                  cx + size * 0.92f, cy + size * 0.50f,
                  cx + size * 0.82f, cy + size * 0.41f);
        p.lineTo(cx + size * 0.45f, cy + size * 0.07f);
        p.cubicTo(cx + size * 0.36f, cy - size * 0.01f,
                  cx + size * 0.22f, cy + size * 0.02f,
                  cx + size * 0.15f, cy + size * 0.12f);
        p.cubicTo(cx + size * 0.07f, cy + size * 0.24f,
                  cx - size * 0.16f, cy + size * 0.04f,
                  cx - size * 0.26f, cy - size * 0.08f);
        p.cubicTo(cx - size * 0.37f, cy - size * 0.21f,
                  cx - size * 0.43f, cy - size * 0.43f,
                  cx - size * 0.30f, cy - size * 0.51f);
        p.lineTo(cx - size * 0.49f, cy - size * 0.86f);
        p.cubicTo(cx - size * 0.55f, cy - size * 0.98f,
                  cx - size * 0.72f, cy - size * 0.94f,
                  cx - size * 0.77f, cy - size * 0.81f);
        p.cubicTo(cx - size * 0.82f, cy - size * 0.70f,
                  cx - size * 0.81f, cy - size * 0.61f,
                  cx - size * 0.74f, cy - size * 0.54f);
        p.close();
        c.drawPath(p.detach(), fill(color));
    }

    void drawMore(SkCanvas& c, float cx, float cy, SkColor color) {
        c.drawCircle(cx, cy - 10, 2.5f, fill(color));
        c.drawCircle(cx, cy, 2.5f, fill(color));
        c.drawCircle(cx, cy + 10, 2.5f, fill(color));
    }

    void drawCheck(SkCanvas& c, float x, float y, SkColor color) {
        line(c, x, y, x + 4, y + 4, color, 1.6f);
        line(c, x + 4, y + 4, x + 10, y - 4, color, 1.6f);
        line(c, x + 7, y + 4, x + 13, y - 4, color, 1.6f);
    }

    void drawSmile(SkCanvas& c, float cx, float cy, float r, SkColor color) {
        c.drawCircle(cx, cy, r, stroke(color, 2.3f));
        c.drawCircle(cx - 5, cy - 4, 1.8f, fill(color));
        c.drawCircle(cx + 5, cy - 4, 1.8f, fill(color));
        SkPathBuilder smile;
        smile.moveTo(cx - 6, cy + 4);
        smile.quadTo(cx, cy + 9, cx + 6, cy + 4);
        c.drawPath(smile.detach(), stroke(color, 2.0f));
    }

    void drawPaperclip(SkCanvas& c, float cx, float cy, SkColor color) {
        SkPathBuilder path;
        path.moveTo(cx - 7, cy + 6);
        path.lineTo(cx + 4, cy - 6);
        path.cubicTo(cx + 10, cy - 12, cx + 19, cy - 4, cx + 12, cy + 4);
        path.lineTo(cx, cy + 17);
        path.cubicTo(cx - 8, cy + 25, cx - 20, cy + 13, cx - 11, cy + 4);
        path.lineTo(cx + 2, cy - 9);
        c.drawPath(path.detach(), stroke(color, 2.4f));
    }

    void drawFolder(SkCanvas& c, float cx, float cy, SkColor color) {
        SkPathBuilder p;
        p.moveTo(cx - 14, cy - 8);
        p.lineTo(cx - 3, cy - 8);
        p.lineTo(cx + 1, cy - 3);
        p.lineTo(cx + 15, cy - 3);
        p.lineTo(cx + 15, cy + 13);
        p.lineTo(cx - 14, cy + 13);
        p.close();
        c.drawPath(p.detach(), stroke(color, 2.5f));
    }

    void drawClose(SkCanvas& c, float cx, float cy, float r, SkColor color) {
        line(c, cx - r * 0.45f, cy - r * 0.45f, cx + r * 0.45f, cy + r * 0.45f, color, 2.4f);
        line(c, cx + r * 0.45f, cy - r * 0.45f, cx - r * 0.45f, cy + r * 0.45f, color, 2.4f);
    }
};

class AppWindow {
public:
    int run(HINSTANCE instance, int showCmd) {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
        dpi_ = getSystemDpi();
        const float dpiScale = dpiScaleForDpi(dpi_);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = &AppWindow::WndProc;
        wc.lpszClassName = L"SkiaRelayDeskWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        RegisterClassExW(&wc);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        const int desiredClientWidth = static_cast<int>(std::round(kBaseWidth * dpiScale));
        const int desiredClientHeight = static_cast<int>(std::round(kContentHeight * dpiScale));
        RECT desired{0, 0, desiredClientWidth, desiredClientHeight};
        adjustWindowRectForDpi(desired, dpi_);
        const int frameWidth = desired.right - desired.left;
        const int frameHeight = desired.bottom - desired.top;
        const float scale = std::min(1.0f, std::min((workWidth - 80.0f) / frameWidth, (workHeight - 80.0f) / frameHeight));
        const int width = static_cast<int>(desiredClientWidth * std::max(0.72f, scale));
        const int height = static_cast<int>(desiredClientHeight * std::max(0.72f, scale));
        RECT initialRect{0, 0, width, height};
        adjustWindowRectForDpi(initialRect, dpi_);
        const int windowWidth = initialRect.right - initialRect.left;
        const int windowHeight = initialRect.bottom - initialRect.top;
        const int x = workArea.left + (workWidth - windowWidth) / 2;
        const int y = workArea.top + (workHeight - windowHeight) / 2;

        HWND hwnd = CreateWindowExW(0,
                                    wc.lpszClassName,
                                    L"RelayDesk",
                                    WS_OVERLAPPEDWINDOW,
                                    x,
                                    y,
                                    windowWidth,
                                    windowHeight,
                                    nullptr,
                                    nullptr,
                                    instance,
                                    this);
        if (!hwnd) {
            return 1;
        }

        BOOL dark = FALSE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        ShowWindow(hwnd, showCmd);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    Renderer renderer_;
    std::vector<uint32_t> pixels_;
    int surfaceWidth_ = 0;
    int surfaceHeight_ = 0;
    UINT dpi_ = static_cast<UINT>(kDefaultDpi);

    static float dpiScaleForDpi(UINT dpi) {
        return static_cast<float>(std::max<UINT>(dpi, 1)) / kDefaultDpi;
    }

    static UINT getSystemDpi() {
        const UINT dpi = GetDpiForSystem();
        return dpi != 0 ? dpi : static_cast<UINT>(kDefaultDpi);
    }

    static UINT getWindowDpi(HWND hwnd) {
        const UINT dpi = GetDpiForWindow(hwnd);
        return dpi != 0 ? dpi : getSystemDpi();
    }

    static void adjustWindowRectForDpi(RECT& rect, UINT dpi) {
        if (!AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
            AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
        }
    }

    static AppWindow* get(HWND hwnd) {
        return reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        AppWindow* app = get(hwnd);
        if (!app) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        switch (message) {
        case WM_SIZE:
            app->resize(LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT:
            app->paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    void resize(int width, int height) {
        surfaceWidth_ = std::max(1, width);
        surfaceHeight_ = std::max(1, height);
        pixels_.assign(static_cast<size_t>(surfaceWidth_) * static_cast<size_t>(surfaceHeight_), 0xFFFFFFFFu);
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        if (width != surfaceWidth_ || height != surfaceHeight_ || pixels_.empty()) {
            resize(width, height);
        }
        dpi_ = getWindowDpi(hwnd);

        const SkImageInfo info = SkImageInfo::MakeN32Premul(surfaceWidth_, surfaceHeight_);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels_.data(), static_cast<size_t>(surfaceWidth_) * sizeof(uint32_t));
        if (surface) {
            renderer_.draw(*surface->getCanvas(), surfaceWidth_, surfaceHeight_, dpiScaleForDpi(dpi_));
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = surfaceWidth_;
        bmi.bmiHeader.biHeight = -surfaceHeight_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(hdc,
                      0,
                      0,
                      surfaceWidth_,
                      surfaceHeight_,
                      0,
                      0,
                      surfaceWidth_,
                      surfaceHeight_,
                      pixels_.data(),
                      &bmi,
                      DIB_RGB_COLORS,
                      SRCCOPY);
        EndPaint(hwnd, &ps);
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    AppWindow app;
    return app.run(instance, showCmd);
}
