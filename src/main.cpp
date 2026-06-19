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

    void draw(SkCanvas& canvas, int width, int height) {
        canvas.clear(rgb(248, 250, 252));
        const float sx = static_cast<float>(width) / kBaseWidth;
        const float sy = static_cast<float>(height) / kContentHeight;
        const float s = std::max(0.1f, std::min(sx, sy));
        const float drawWidth = kBaseWidth * s;
        const float drawHeight = kContentHeight * s;
        const float originX = (static_cast<float>(width) - drawWidth) * 0.5f;
        const float originY = (static_cast<float>(height) - drawHeight) * 0.5f;

        canvas.save();
        canvas.translate(originX, originY);
        canvas.scale(s, s);
        canvas.translate(0, -kChromeHeight);
        drawApp(canvas);
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

    void drawApp(SkCanvas& c) {
        drawSidebar(c);
        drawConversation(c);
        drawInspector(c);
    }

    void drawSidebar(SkCanvas& c) {
        const float x = 0;
        const float y = 46;
        const float w = 386;
        c.drawRect(SkRect::MakeXYWH(x, y, w, 914), fill(rgb(252, 254, 255)));
        line(c, 385.5f, 46, 385.5f, 960, rgb(216, 222, 229));

        c.drawRRect(rr({22, 65, 43, 43}, 12), fill(rgb(0, 157, 153)));
        centeredText(c, "林", {22, 65, 43, 43}, 23, SK_ColorWHITE, true);
        text(c, "林一凡", 80, 82, 18, rgb(13, 17, 23), true);
        text(c, "本机 · 192.168.1.8", 80, 103, 14, rgb(64, 74, 88));
        drawGear(c, 344, 87, 12, rgb(90, 100, 115));

        Rect search{23, 125, 340, 46};
        c.drawRRect(rr(search, 7), fill(SK_ColorWHITE));
        c.drawRRect(rr(search, 7), stroke(rgb(205, 214, 224)));
        drawSearch(c, 50, 148, 10, rgb(24, 29, 38), 2.0f);
        text(c, "搜索设备", 78, 154, 14, rgb(136, 148, 162));

        text(c, "在线设备 (5)", 39, 209, 15, rgb(18, 23, 31), true);

        std::vector<Device> online = {
            {"Alex-PC", "192.168.1.24", true},
            {"DESKTOP-J8K2TQ", "192.168.1.31", true},
            {"LAPTOP-9F3V2M", "192.168.1.42", true},
            {"DEV-SERVER", "192.168.1.10", true},
            {"MARK-PC", "192.168.1.77", true},
        };

        float itemY = 230;
        for (size_t i = 0; i < online.size(); ++i) {
            drawDeviceRow(c, online[i], itemY, i == 0);
            itemY += 81;
        }

        text(c, "离线设备 (3)", 39, 669, 15, rgb(18, 23, 31), true);
        std::vector<Device> offline = {
            {"FINANCE-PC", "192.168.1.15", false},
            {"HR-LAPTOP", "192.168.1.28", false},
            {"OLD-PC", "192.168.1.55", false},
        };
        itemY = 697;
        for (const auto& d : offline) {
            drawDeviceRow(c, d, itemY, false);
            itemY += 78;
        }
    }

    void drawDeviceRow(SkCanvas& c, const Device& d, float y, bool selected) {
        if (selected) {
            Rect r{20, y, 343, 81};
            c.drawRRect(rr(r, 7), fill(rgb(235, 254, 254)));
            c.drawRRect(rr(r, 7), stroke(rgb(118, 214, 213), 1.0f));
        }
        c.drawCircle(40, y + 38, 5.3f, fill(d.online ? rgb(45, 181, 36) : rgb(157, 168, 181)));
        drawMonitor(c, 82, y + 38, 18, rgb(13, 17, 23));
        text(c, d.name, 117, y + 33, 15.5f, rgb(19, 25, 34), true);
        text(c, d.ip, 117, y + 56, 13.2f, rgb(53, 63, 78));
    }

    void drawConversation(SkCanvas& c) {
        const float left = 386;
        const float right = 1151;
        c.drawRect(SkRect::MakeXYWH(left, 46, right - left, 914), fill(SK_ColorWHITE));
        line(c, 386, 194.5f, 1151, 194.5f, rgb(217, 223, 230));
        line(c, 386, 848.5f, 1151, 848.5f, rgb(217, 223, 230));
        line(c, 1151.5f, 46, 1151.5f, 960, rgb(216, 222, 229));

        drawChatHeader(c);
        drawTabs(c);
        drawMessages(c);
        drawComposer(c);
    }

    void drawChatHeader(SkCanvas& c) {
        drawMonitor(c, 437, 94, 26, rgb(13, 17, 23));
        text(c, "Alex-PC", 481, 89, 21, rgb(18, 23, 31), true);
        c.drawCircle(470, 108, 5.8f, fill(rgb(45, 181, 36)));
        text(c, "192.168.1.24", 486, 113, 13.5f, rgb(53, 63, 78));

        drawPhone(c, 968, 96, 16, rgb(8, 12, 20));
        drawSearch(c, 1044, 96, 14, rgb(8, 12, 20), 3.4f);
        drawMore(c, 1113, 95, rgb(8, 12, 20));
    }

    void drawTabs(SkCanvas& c) {
        Rect outer{417, 140, 542, 39};
        c.drawRRect(rr(outer, 6), fill(SK_ColorWHITE));
        c.drawRRect(rr(outer, 6), stroke(rgb(202, 211, 222)));
        Rect active{417, 140, 177, 39};
        c.drawRRect(rr(active, 6), fill(rgb(0, 157, 153)));
        c.drawRect(SkRect::MakeXYWH(585, 140, 10, 39), fill(rgb(0, 157, 153)));
        line(c, 594, 140, 594, 179, rgb(202, 211, 222));
        line(c, 781, 140, 781, 179, rgb(202, 211, 222));
        centeredText(c, "聊天", {417, 140, 177, 39}, 14, SK_ColorWHITE, true);
        centeredText(c, "历史", {594, 140, 187, 39}, 14, rgb(70, 79, 94), true);
        centeredText(c, "传输", {781, 140, 178, 39}, 14, rgb(70, 79, 94), true);
    }

    void drawMessages(SkCanvas& c) {
        Rect today{718, 210, 62, 32};
        c.drawRRect(rr(today, 7), fill(SK_ColorWHITE));
        c.drawRRect(rr(today, 7), stroke(rgb(205, 214, 224)));
        centeredText(c, "今天", today, 12.8f, rgb(90, 100, 115), true);

        drawBubble(c, {416, 253, 264, 51}, "能把最新的 Q2 报告发我吗?", false);
        text(c, "09:21", 692, 282, 12, rgb(86, 98, 115));

        drawBubble(c, {868, 316, 171, 51}, "可以，正在上传。", true);
        text(c, "09:21", 1050, 346, 12, rgb(86, 98, 115));
        drawCheck(c, 1091, 343, rgb(0, 157, 153));

        drawTransferCard(c, {483, 385, 600, 141}, true);

        drawBubble(c, {416, 545, 241, 51}, "谢谢! 设计说明也在吗?", false);
        text(c, "09:23", 667, 575, 12, rgb(86, 98, 115));

        drawTransferCard(c, {416, 616, 668, 137}, false);

        drawBubble(c, {795, 775, 222, 51}, "是的，这是最新版本。", true);
        text(c, "09:25", 1030, 805, 12, rgb(86, 98, 115));
        drawCheck(c, 1074, 802, rgb(0, 157, 153));

        c.drawRoundRect(SkRect::MakeXYWH(1137, 208, 6, 342), 3, 3, fill(rgb(180, 190, 202)));
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

        drawFileIcon(c, r.x + 25, r.y + 28, accent);

        const std::string name = teal ? "Q2_Report_2024.pdf" : "Design_Specs_v2.zip";
        const std::string size = teal ? "24.8 MB" : "112.6 MB";
        const std::string percent = teal ? "78%" : "45%";
        const std::string detail = teal ? "19.4 MB / 24.8 MB  -  5.2 MB/s  -  剩余 00:00:01"
                                        : "50.7 MB / 112.6 MB  -  4.1 MB/s  -  剩余 00:00:15";
        const std::string time = teal ? "09:22" : "09:24";
        const float progress = teal ? 0.78f : 0.45f;

        text(c, name, r.x + 77, r.y + 38, 14.2f, rgb(18, 23, 31), true);
        text(c, size, r.x + 77, r.y + 70, 12.8f, rgb(65, 76, 92), true);
        text(c, percent, r.x + r.w - 64, r.y + 70, 12.8f, rgb(18, 23, 31), true);
        drawProgress(c, r.x + 77, r.y + 85, r.w - 122, progress, accent, 6);
        text(c, detail, r.x + 77, r.y + 119, 12.3f, rgb(80, 92, 108), true);
        text(c, time, r.x + r.w - 82, r.y + 119, 12.0f, rgb(80, 92, 108));
        drawCheck(c, r.x + r.w - 38, r.y + 116, rgb(0, 157, 153));
    }

    void drawComposer(SkCanvas& c) {
        Rect input{413, 870, 712, 78};
        c.drawRRect(rr(input, 8), fill(SK_ColorWHITE));
        c.drawRRect(rr(input, 8), stroke(rgb(214, 222, 231)));
        text(c, "给 Alex-PC 发送消息", 429, 914, 14, rgb(139, 150, 165));
        drawSmile(c, 854, 909, 14, rgb(8, 12, 20));
        drawPaperclip(c, 918, 909, rgb(8, 12, 20));
        drawFolder(c, 980, 910, rgb(8, 12, 20));
        Rect send{1030, 886, 84, 46};
        c.drawRRect(rr(send, 7), fill(rgb(0, 157, 153)));
        centeredText(c, "发送", send, 15, SK_ColorWHITE, true);
    }

    void drawInspector(SkCanvas& c) {
        c.drawRect(SkRect::MakeXYWH(1152, 46, 384, 914), fill(SK_ColorWHITE));
        text(c, "Alex-PC", 1178, 91, 18, rgb(18, 23, 31), true);
        drawClose(c, 1495, 86, 15, rgb(8, 12, 20));

        drawMonitor(c, 1256, 181, 39, rgb(13, 17, 23));
        c.drawCircle(1328, 176, 7, fill(rgb(45, 181, 36)));
        text(c, "在线", 1347, 182, 15, rgb(45, 181, 36), true);
        text(c, "192.168.1.24", 1347, 216, 14, rgb(18, 23, 31), true);
        line(c, 1152, 267.5f, 1536, 267.5f, rgb(217, 223, 230));

        text(c, "设备信息", 1176, 306, 15, rgb(18, 23, 31), true);
        drawInfoRow(c, 1176, 350, "电脑名", "Alex-PC");
        drawInfoRow(c, 1176, 385, "用户名", "Alex");
        drawInfoRow(c, 1176, 420, "系统", "Windows 11 Pro 23H2");
        drawInfoRow(c, 1176, 455, "IP 地址", "192.168.1.24");
        drawInfoRow(c, 1176, 490, "MAC 地址", "00-15-5D-8E-2A-7C");
        drawInfoRow(c, 1176, 525, "在线时长", "2天 4时 18分");

        line(c, 1152, 558.5f, 1536, 558.5f, rgb(217, 223, 230));
        text(c, "活跃传输 (2)", 1176, 604, 15, rgb(18, 23, 31), true);
        drawMiniTransfer(c, 1179, 644, true);
        line(c, 1176, 734, 1484, 734, rgb(224, 230, 237));
        drawMiniTransfer(c, 1179, 763, false);
        text(c, "查看全部传输", 1176, 921, 14, rgb(0, 157, 153), true);
    }

    void drawInfoRow(SkCanvas& c, float x, float y, std::string_view label, std::string_view value) {
        text(c, label, x, y, 13.2f, rgb(83, 94, 111), true);
        const float w = textWidth(value, 13.2f, true);
        text(c, value, 1500 - w, y, 13.2f, rgb(18, 23, 31), true);
    }

    void drawMiniTransfer(SkCanvas& c, float x, float y, bool teal) {
        const SkColor accent = teal ? rgb(0, 157, 153) : rgb(230, 143, 0);
        drawFileIcon(c, x, y, accent, 30);
        const std::string name = teal ? "Q2_Report_2024.pdf" : "Design_Specs_v2.zip";
        const std::string direction = teal ? "发给 Alex-PC" : "来自 Alex-PC";
        const std::string pct = teal ? "78%" : "45%";
        const std::string detail = teal ? "19.4 MB / 24.8 MB  -  5.2 MB/s" : "50.7 MB / 112.6 MB  -  4.1 MB/s";
        text(c, name, x + 46, y + 10, 13.2f, rgb(18, 23, 31), true);
        text(c, direction, x + 46, y + 40, 12.2f, rgb(80, 92, 108), true);
        text(c, pct, 1456, y + 40, 12.2f, accent, true);
        drawProgress(c, x, y + 57, 303, teal ? 0.78f : 0.45f, accent, 5);
        text(c, detail, x, y + 83, 12.0f, rgb(80, 92, 108), true);
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
        RECT desired{0, 0, kBaseWidth, kContentHeight};
        AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
        const int frameWidth = desired.right - desired.left;
        const int frameHeight = desired.bottom - desired.top;
        const float scale = std::min(1.0f, std::min((workWidth - 80.0f) / frameWidth, (workHeight - 80.0f) / frameHeight));
        const int width = static_cast<int>(kBaseWidth * std::max(0.72f, scale));
        const int height = static_cast<int>(kContentHeight * std::max(0.72f, scale));
        RECT initialRect{0, 0, width, height};
        AdjustWindowRectEx(&initialRect, WS_OVERLAPPEDWINDOW, FALSE, 0);
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

        const SkImageInfo info = SkImageInfo::MakeN32Premul(surfaceWidth_, surfaceHeight_);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels_.data(), static_cast<size_t>(surfaceWidth_) * sizeof(uint32_t));
        if (surface) {
            renderer_.draw(*surface->getCanvas(), surfaceWidth_, surfaceHeight_);
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
