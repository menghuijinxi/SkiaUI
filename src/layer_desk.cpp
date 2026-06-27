#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include "d3d_presenter.h"
#include "perf_trace.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkTypeface_win.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int kBaseWidth = 1672;
constexpr int kBaseHeight = 941;
constexpr float kDefaultDpi = 96.0f;
constexpr float kPi = 3.14159265358979323846f;
constexpr COLORREF kWin32Background = RGB(7, 12, 18);

LONG WINAPI writeMiniDump(EXCEPTION_POINTERS* exceptionPointers) {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    wchar_t* lastSlash = std::wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    } else {
        exePath[0] = L'\0';
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t dumpPath[MAX_PATH]{};
    std::swprintf(dumpPath,
                  MAX_PATH,
                  L"%sSkiaLayerDesk_%04u%02u%02u_%02u%02u%02u.dmp",
                  exePath,
                  now.wYear,
                  now.wMonth,
                  now.wDay,
                  now.wHour,
                  now.wMinute,
                  now.wSecond);

    HANDLE file = CreateFileW(dumpPath,
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exceptionPointers;
    info.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(),
                      GetCurrentProcessId(),
                      file,
                      MiniDumpWithDataSegs,
                      exceptionPointers ? &info : nullptr,
                      nullptr,
                      nullptr);
    CloseHandle(file);
    return EXCEPTION_CONTINUE_SEARCH;
}

SkColor rgb(uint8_t r, uint8_t g, uint8_t b) {
    return SkColorSetRGB(r, g, b);
}

SkColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return SkColorSetARGB(a, r, g, b);
}

SkColor withAlpha(SkColor color, uint8_t alpha) {
    return SkColorSetARGB(alpha, SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
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

class Renderer {
public:
    Renderer() {
        fontMgr_ = SkFontMgr_New_DirectWrite();
        if (!fontMgr_) {
            fontMgr_ = SkFontMgr_New_GDI();
        }
        regular_ = pickTypeface(false);
        medium_ = pickTypeface(true);
    }

    void draw(SkCanvas& canvas, int width, int height, float dpiScale) {
        const bool traceEnabled = perf::Trace::enabled();
        const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        const float scale = std::max(0.1f, dpiScale);
        const float logicalWidth = static_cast<float>(width) / scale;
        const float logicalHeight = static_cast<float>(height) / scale;

        canvas.clear(rgb(7, 12, 18));
        canvas.save();
        canvas.scale(scale, scale);
        drawApp(canvas, logicalWidth, logicalHeight, traceEnabled);
        canvas.restore();
        if (traceEnabled) {
            perf::Trace::write("skia", "draw_total", width, height, perf::Trace::elapsedMs(traceStart));
        }
    }

private:
    sk_sp<SkFontMgr> fontMgr_;
    sk_sp<SkTypeface> regular_;
    sk_sp<SkTypeface> medium_;
    mutable std::unordered_map<std::string, sk_sp<SkSVGDOM>> svgCache_;
    struct TextEntry {
        sk_sp<SkTextBlob> blob;
        float width = 0.0f;
        SkRect bounds = SkRect::MakeEmpty();
    };
    mutable std::unordered_map<std::string, TextEntry> textCache_;
    mutable sk_sp<SkPicture> sidebarContentPicture_;
    mutable sk_sp<SkPicture> layerPanelPicture_;
    mutable sk_sp<SkPicture> statusContentPicture_;
    mutable sk_sp<SkPicture> compassPicture_;

    sk_sp<SkTypeface> pickTypeface(bool bold) {
        const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
        const std::array<const char*, 5> families = {
            "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", "Arial", nullptr};
        for (const char* family : families) {
            if (!fontMgr_) {
                continue;
            }
            sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(family, style);
            if (typeface) {
                return typeface;
            }
        }
        return nullptr;
    }

    SkFont font(float size, bool bold = false) const {
        SkFont f(bold ? medium_ : regular_, size);
        f.setEdging(SkFont::Edging::kSubpixelAntiAlias);
        f.setHinting(SkFontHinting::kNormal);
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
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setStrokeJoin(SkPaint::kRound_Join);
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
              SkColor color,
              bool isBold = false) const {
        SkPaint p = fill(color);
        const TextEntry& entry = textEntry(value, size, isBold);
        if (entry.blob) {
            c.drawTextBlob(entry.blob, x, y, p);
            return;
        }
        const SkFont f = font(size, isBold);
        c.drawSimpleText(value.data(), value.size(), SkTextEncoding::kUTF8, x, y, f, p);
    }

    float textWidth(std::string_view value, float size, bool isBold = false) const {
        return textEntry(value, size, isBold).width;
    }

    std::string textKey(std::string_view value, float size, bool isBold) const {
        std::string key;
        key.reserve(value.size() + 32);
        key.append(isBold ? "1|" : "0|");
        char sizeBuf[24];
        std::snprintf(sizeBuf, sizeof(sizeBuf), "%.2f|", size);
        key.append(sizeBuf);
        key.append(value.data(), value.size());
        return key;
    }

    const TextEntry& textEntry(std::string_view value, float size, bool isBold) const {
        const std::string key = textKey(value, size, isBold);
        auto it = textCache_.find(key);
        if (it != textCache_.end()) {
            return it->second;
        }

        const SkFont f = font(size, isBold);
        TextEntry entry;
        entry.width = f.measureText(value.data(), value.size(), SkTextEncoding::kUTF8, &entry.bounds);
        entry.blob = SkTextBlob::MakeFromText(value.data(), value.size(), f, SkTextEncoding::kUTF8);
        it = textCache_.emplace(key, std::move(entry)).first;
        return it->second;
    }

    std::string svgHex(SkColor color) const {
        char buf[16];
        std::snprintf(buf,
                      sizeof(buf),
                      "#%02X%02X%02X",
                      SkColorGetR(color),
                      SkColorGetG(color),
                      SkColorGetB(color));
        return buf;
    }

    std::string svgOpacity(SkColor color) const {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<float>(SkColorGetA(color)) / 255.0f);
        return buf;
    }

    std::string svgFloat(float value) const {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }

    std::string svgStrokeAttrs(SkColor color, float width) const {
        return "stroke=\"" + svgHex(color) + "\" stroke-opacity=\"" + svgOpacity(color) +
               "\" stroke-width=\"" + svgFloat(width) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
    }

    std::string svgFillAttrs(SkColor color) const {
        return "fill=\"" + svgHex(color) + "\" fill-opacity=\"" + svgOpacity(color) + "\"";
    }

    std::string svgWrap(std::string_view body, const std::string& attrs) const {
        std::string svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" ";
        svg += attrs;
        svg += ">";
        svg += body;
        svg += "</svg>";
        return svg;
    }

    void drawSvgIcon(SkCanvas& c, const std::string& svg, float cx, float cy, float size) const {
        auto icon = svgCache_.find(svg);
        if (icon == svgCache_.end()) {
            SkMemoryStream stream(svg.data(), svg.size(), false);
            sk_sp<SkSVGDOM> dom = SkSVGDOM::MakeFromStream(stream);
            if (!dom) {
                return;
            }
            dom->setContainerSize(SkSize::Make(24.0f, 24.0f));
            icon = svgCache_.emplace(svg, std::move(dom)).first;
        }
        c.save();
        c.translate(cx - size * 0.5f, cy - size * 0.5f);
        c.scale(size / 24.0f, size / 24.0f);
        icon->second->render(&c);
        c.restore();
    }

    void centeredText(SkCanvas& c,
                      std::string_view value,
                      const Rect& box,
                      float size,
                      SkColor color,
                      bool isBold = false) const {
        SkFont f = font(size, isBold);
        const TextEntry& entry = textEntry(value, size, isBold);
        const SkRect bounds = entry.bounds;
        const float w = entry.width;
        const float x = box.x + (box.w - w) * 0.5f;
        const float y = box.y + box.h * 0.5f - (bounds.fTop + bounds.fBottom) * 0.5f;
        if (entry.blob) {
            c.drawTextBlob(entry.blob, x, y, fill(color));
            return;
        }
        c.drawSimpleText(value.data(), value.size(), SkTextEncoding::kUTF8, x, y, f, fill(color));
    }

    void line(SkCanvas& c, float x1, float y1, float x2, float y2, SkColor color, float width = 1.0f) const {
        c.drawLine(x1, y1, x2, y2, stroke(color, width));
    }

    std::vector<SkColor4f> gradientColors(const SkColor* colors, int count) const {
        std::vector<SkColor4f> stops;
        stops.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            stops.push_back(SkColor4f::FromColor(colors[i]));
        }
        return stops;
    }

    std::vector<float> gradientPositions(const SkScalar* positions, int count) const {
        std::vector<float> stops;
        if (!positions) {
            return stops;
        }
        stops.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            stops.push_back(positions[i]);
        }
        return stops;
    }

    SkGradient makeGradient(SkSpan<const SkColor4f> colors, SkSpan<const float> positions) const {
        return SkGradient(SkGradient::Colors(colors, positions, SkTileMode::kClamp), {});
    }

    SkPaint linearPaint(SkPoint a,
                        SkPoint b,
                        const SkColor* colors,
                        const SkScalar* positions,
                        int count,
                        uint8_t alpha = 255) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        p.setAlphaf(static_cast<float>(alpha) / 255.0f);
        const SkPoint pts[2] = {a, b};
        const std::vector<SkColor4f> gradientColorStops = gradientColors(colors, count);
        const std::vector<float> gradientPositionStops = gradientPositions(positions, count);
        const SkGradient gradient = makeGradient(gradientColorStops, gradientPositionStops);
        p.setShader(SkShaders::LinearGradient(pts, gradient));
        return p;
    }

    SkPaint radialPaint(SkPoint center,
                        float radius,
                        const SkColor* colors,
                        const SkScalar* positions,
                        int count) const {
        SkPaint p;
        p.setAntiAlias(true);
        p.setStyle(SkPaint::kFill_Style);
        const std::vector<SkColor4f> gradientColorStops = gradientColors(colors, count);
        const std::vector<float> gradientPositionStops = gradientPositions(positions, count);
        const SkGradient gradient = makeGradient(gradientColorStops, gradientPositionStops);
        p.setShader(SkShaders::RadialGradient(center, radius, gradient));
        return p;
    }

    template <typename DrawFn>
    sk_sp<SkPicture> recordPicture(const SkRect& bounds, const char* event, DrawFn&& draw) const {
        const bool traceEnabled = perf::Trace::enabled();
        const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        SkPictureRecorder recorder;
        SkCanvas* canvas = recorder.beginRecording(bounds);
        draw(*canvas);
        sk_sp<SkPicture> picture = recorder.finishRecordingAsPicture();
        if (traceEnabled) {
            perf::Trace::write("skia_cache",
                               event,
                               static_cast<int>(bounds.width()),
                               static_cast<int>(bounds.height()),
                               perf::Trace::elapsedMs(traceStart));
        }
        return picture;
    }

    template <typename DrawFn>
    void drawStage(bool traceEnabled, const char* event, int width, int height, DrawFn&& draw) {
        if (!traceEnabled) {
            draw();
            return;
        }
        const auto start = perf::Trace::now();
        draw();
        perf::Trace::write("skia", event, width, height, perf::Trace::elapsedMs(start));
    }

    void drawApp(SkCanvas& c, float width, float height, bool traceEnabled) {
        const int traceWidth = static_cast<int>(width);
        const int traceHeight = static_cast<int>(height);
        drawStage(traceEnabled, "background", traceWidth, traceHeight, [&] { drawBackground(c, width, height); });
        drawStage(traceEnabled, "sidebar", traceWidth, traceHeight, [&] { drawSidebar(c, height); });
        drawStage(traceEnabled, "layer_panel", traceWidth, traceHeight, [&] { drawLayerPanel(c); });
        drawStage(traceEnabled, "compass", traceWidth, traceHeight, [&] { drawCompass(c, width - 94.0f, height - 162.0f); });
        drawStage(traceEnabled, "status_bar", traceWidth, traceHeight, [&] { drawStatusBar(c, width, height); });
        drawStage(traceEnabled, "frame", traceWidth, traceHeight, [&] {
            c.drawRoundRect(SkRect::MakeXYWH(2.0f, 2.0f, width - 4.0f, height - 4.0f),
                            10.0f,
                            10.0f,
                            stroke(rgba(125, 155, 173, 105), 1.0f));
        });
    }

    void drawBackground(SkCanvas& c, float width, float height) {
        const SkColor baseColors[] = {rgb(7, 13, 20), rgb(21, 34, 49), rgb(12, 22, 33)};
        const SkScalar basePos[] = {0.0f, 0.48f, 1.0f};
        c.drawRect(SkRect::MakeWH(width, height),
                   linearPaint(SkPoint::Make(0.0f, 0.0f), SkPoint::Make(width, height), baseColors, basePos, 3));

        const SkColor upperGlow[] = {rgba(83, 119, 151, 120), rgba(26, 55, 83, 70), rgba(8, 15, 22, 0)};
        const SkScalar glowPos[] = {0.0f, 0.38f, 1.0f};
        c.drawRect(SkRect::MakeWH(width, height),
                   radialPaint(SkPoint::Make(575.0f, -130.0f), 820.0f, upperGlow, glowPos, 3));

        const SkColor sideGlow[] = {rgba(48, 84, 115, 70), rgba(16, 31, 47, 20), rgba(4, 9, 14, 0)};
        c.drawRect(SkRect::MakeWH(width, height),
                   radialPaint(SkPoint::Make(width * 0.78f, 190.0f), 980.0f, sideGlow, glowPos, 3));

        const SkColor vignette[] = {rgba(0, 0, 0, 0), rgba(2, 5, 8, 155)};
        const SkScalar vignettePos[] = {0.42f, 1.0f};
        c.drawRect(SkRect::MakeWH(width, height),
                   radialPaint(SkPoint::Make(width * 0.54f, height * 0.44f), width * 0.82f, vignette, vignettePos, 2));
    }

    void drawSidebar(SkCanvas& c, float height) {
        constexpr float sidebarW = 118.0f;
        const Rect sidebar{0.0f, 0.0f, sidebarW, height - 60.0f};
        const SkColor colors[] = {rgba(5, 10, 16, 238), rgba(10, 18, 27, 218)};
        const SkScalar pos[] = {0.0f, 1.0f};
        c.drawRect(sidebar.sk(),
                   linearPaint(SkPoint::Make(0.0f, 0.0f), SkPoint::Make(sidebarW, 0.0f), colors, pos, 2));
        line(c, sidebarW - 0.5f, 0.0f, sidebarW - 0.5f, height - 60.0f, rgba(83, 118, 143, 46), 1.0f);

        if (!sidebarContentPicture_) {
            sidebarContentPicture_ = recordPicture(
                SkRect::MakeWH(sidebarW, 850.0f),
                "sidebar_content_picture",
                [this](SkCanvas& pictureCanvas) { drawSidebarContent(pictureCanvas); });
        }
        c.drawPicture(sidebarContentPicture_.get());
    }

    void drawSidebarContent(SkCanvas& c) {
        constexpr float sidebarW = 118.0f;
        const SkScalar pos[] = {0.0f, 1.0f};
        for (int i = 0; i < 3; ++i) {
            line(c, 47.0f, 35.0f + i * 12.0f, 77.0f, 35.0f + i * 12.0f, rgba(213, 224, 234, 220), 3.2f);
        }

        const Rect active{3.0f, 88.0f, sidebarW - 3.0f, 102.0f};
        const SkColor activeColors[] = {rgba(16, 224, 207, 38), rgba(81, 178, 203, 16)};
        c.drawRect(active.sk(),
                   linearPaint(SkPoint::Make(active.x, 0.0f), SkPoint::Make(active.x + active.w, 0.0f), activeColors, pos, 2));
        c.drawRect(SkRect::MakeXYWH(3.0f, 88.0f, 5.0f, 102.0f), fill(rgb(34, 224, 211)));

        drawNavItem(c, 64.0f, 127.0f, "图层", 0, true);
        drawNavItem(c, 64.0f, 230.0f, "编辑", 1, false);
        drawNavItem(c, 64.0f, 333.0f, "绘制", 2, false);
        drawNavItem(c, 64.0f, 436.0f, "高亮", 3, false);
        drawNavItem(c, 64.0f, 540.0f, "属性", 4, false);
        drawNavItem(c, 64.0f, 645.0f, "变更", 5, false);
        drawNavItem(c, 64.0f, 752.0f, "设置", 6, false);
    }

    void drawNavItem(SkCanvas& c, float cx, float iconY, std::string_view label, int icon, bool active) {
        const SkColor color = active ? rgb(48, 234, 220) : rgba(222, 230, 242, 210);
        switch (icon) {
        case 0:
            drawLayerStack(c, cx, iconY, 44.0f, color, false);
            break;
        case 1:
            drawEditIcon(c, cx, iconY, color);
            break;
        case 2:
            drawPencilIcon(c, cx, iconY, color);
            break;
        case 3:
            drawTargetIcon(c, cx, iconY, color);
            break;
        case 4:
            drawDocIcon(c, cx, iconY, color);
            break;
        case 5:
            drawSwitchIcon(c, cx, iconY, color);
            break;
        default:
            drawGearIcon(c, cx, iconY, color);
            break;
        }
        const float w = textWidth(label, 17.0f, false);
        text(c, label, cx - w * 0.5f, iconY + 47.0f, 17.0f, color, false);
    }

    void drawLayerPanel(SkCanvas& c) {
        if (!layerPanelPicture_) {
            layerPanelPicture_ = recordPicture(
                SkRect::MakeWH(static_cast<float>(kBaseWidth), static_cast<float>(kBaseHeight)),
                "layer_panel_picture",
                [this](SkCanvas& pictureCanvas) { drawLayerPanelContent(pictureCanvas); });
        }
        c.drawPicture(layerPanelPicture_.get());
    }

    void drawLayerPanelContent(SkCanvas& c) {
        const Rect panel{134.0f, 25.0f, 602.0f, 825.0f};
        const SkColor shadowColors[] = {rgba(0, 0, 0, 70), rgba(0, 0, 0, 0)};
        const SkScalar shadowPos[] = {0.0f, 1.0f};
        c.drawRoundRect(SkRect::MakeXYWH(panel.x - 8.0f, panel.y - 8.0f, panel.w + 16.0f, panel.h + 16.0f),
                        14.0f,
                        14.0f,
                        radialPaint(SkPoint::Make(panel.x + panel.w * 0.45f, panel.y + panel.h * 0.5f),
                                    520.0f,
                                    shadowColors,
                                    shadowPos,
                                    2));

        const SkColor panelFill[] = {rgba(23, 39, 55, 214), rgba(11, 22, 32, 228), rgba(20, 34, 48, 205)};
        const SkScalar panelPos[] = {0.0f, 0.58f, 1.0f};
        c.drawRRect(rr(panel, 9.0f),
                    linearPaint(SkPoint::Make(panel.x, panel.y), SkPoint::Make(panel.x + panel.w, panel.y + panel.h), panelFill, panelPos, 3));
        c.drawRRect(rr(panel, 9.0f), stroke(rgba(146, 179, 199, 160), 1.25f));
        c.drawRRect(rr({panel.x + 1.0f, panel.y + 1.0f, panel.w - 2.0f, panel.h - 2.0f}, 8.0f),
                    stroke(rgba(222, 244, 252, 38), 1.0f));

        drawLayerStack(c, 183.0f, 62.0f, 44.0f, rgba(221, 252, 255, 240), false);
        constexpr float titleSize = 27.0f;
        constexpr float subtitleSize = 20.5f;
        const float titleX = 215.0f;
        const float titleBaseline = 72.0f;
        text(c, "图层", titleX, titleBaseline, titleSize, rgba(239, 247, 253, 245), true);
        text(c,
             " / 图层管理",
             titleX + textWidth("图层", titleSize, true) + 7.0f,
             titleBaseline - 1.0f,
             subtitleSize,
             rgba(239, 247, 253, 232),
             false);

        drawActionButton(c, {160.0f, 110.0f, 253.0f, 47.0f}, "导入SHP", true);
        drawActionButton(c, {431.0f, 110.0f, 266.0f, 47.0f}, "新建图层", false);
        drawSearchBox(c, {161.0f, 180.0f, 536.0f, 46.0f});
        drawTable(c);
        drawLayerDetails(c, {149.0f, 566.0f, 568.0f, 262.0f});
    }

    void drawActionButton(SkCanvas& c, const Rect& r, std::string_view label, bool primary) {
        if (primary) {
            const SkColor colors[] = {rgba(18, 190, 178, 130), rgba(8, 95, 102, 150)};
            const SkScalar pos[] = {0.0f, 1.0f};
            c.drawRRect(rr(r, 7.0f),
                        linearPaint(SkPoint::Make(r.x, r.y), SkPoint::Make(r.x + r.w, r.y + r.h), colors, pos, 2));
            c.drawRRect(rr(r, 7.0f), stroke(rgba(38, 237, 218, 175), 1.1f));
        drawUploadIcon(c, r.x + 80.0f, r.y + r.h * 0.5f + 1.0f, rgba(242, 252, 255, 235));
        } else {
            c.drawRRect(rr(r, 7.0f), fill(rgba(19, 32, 45, 104)));
            c.drawRRect(rr(r, 7.0f), stroke(rgba(142, 170, 193, 142), 1.1f));
            drawPlusIcon(c, r.x + 86.0f, r.y + r.h * 0.5f, rgba(241, 247, 254, 230));
        }
        centeredText(c, label, {r.x + 32.0f, r.y, r.w - 32.0f, r.h}, 20.5f, rgba(240, 247, 252, 238), true);
    }

    void drawSearchBox(SkCanvas& c, const Rect& r) {
        c.drawRRect(rr(r, 7.0f), fill(rgba(11, 22, 32, 130)));
        c.drawRRect(rr(r, 7.0f), stroke(rgba(132, 164, 190, 130), 1.15f));
        drawSearchIcon(c, r.x + 26.0f, r.y + r.h * 0.5f, 9.0f, rgba(213, 224, 236, 220), 2.7f);
        text(c, "搜索图层...", r.x + 55.0f, r.y + 29.0f, 17.5f, rgba(180, 195, 207, 132), false);
    }

    void drawTable(SkCanvas& c) {
        text(c, "名称", 224.0f, 264.0f, 16.5f, rgba(229, 239, 247, 230), false);
        text(c, "可见", 356.0f, 264.0f, 16.5f, rgba(229, 239, 247, 230), false);
        text(c, "要素数", 423.0f, 264.0f, 16.5f, rgba(229, 239, 247, 230), false);
        text(c, "类型", 508.0f, 264.0f, 16.5f, rgba(229, 239, 247, 230), false);
        text(c, "坐标系", 596.0f, 264.0f, 16.5f, rgba(229, 239, 247, 230), false);

        drawLayerRow(c, 280.0f, true, 0, "地块边界", "6,523", "Polygon");
        drawLayerRow(c, 334.0f, false, 1, "道路中心线", "2,431", "LineString");
        drawLayerRow(c, 388.0f, false, 2, "控制点", "1,842", "Point");
        drawLayerRow(c, 442.0f, false, 3, "河流", "312", "LineString");
        drawLayerRow(c, 496.0f, false, 4, "建筑物", "164", "Polygon");
    }

    void drawLayerRow(SkCanvas& c,
                      float y,
                      bool selected,
                      int icon,
                      std::string_view name,
                      std::string_view count,
                      std::string_view type) {
        const Rect row{149.0f, y, 570.0f, 54.0f};
        if (selected) {
            const SkColor colors[] = {rgba(0, 168, 150, 150), rgba(20, 173, 166, 105)};
            const SkScalar pos[] = {0.0f, 1.0f};
            c.drawRect(row.sk(),
                       linearPaint(SkPoint::Make(row.x, y), SkPoint::Make(row.x + row.w, y), colors, pos, 2));
        } else {
            c.drawRRect(rr({row.x, row.y, row.w, row.h - 1.0f}, 4.0f), fill(rgba(11, 23, 33, 60)));
            line(c, row.x + 2.0f, y + row.h - 0.5f, row.x + row.w - 2.0f, y + row.h - 0.5f, rgba(120, 149, 169, 32), 1.0f);
        }

        drawDragDots(c, row.x + 19.0f, y + 27.0f, rgba(220, 232, 241, selected ? 215 : 165));
        drawGeometryIcon(c, icon, row.x + 59.0f, y + 27.0f, selected ? rgb(63, 237, 225) : rgba(216, 226, 238, 220));
        text(c, name, row.x + 84.0f, y + 34.0f, 16.5f, rgba(239, 246, 251, 238), false);
        drawEyeIcon(c, row.x + 221.0f, y + 27.0f, rgb(48, 234, 220));
        text(c, count, row.x + 274.0f, y + 34.0f, 15.8f, rgba(235, 244, 249, 232), false);
        text(c, type, row.x + 356.0f, y + 34.0f, 15.0f, rgba(235, 244, 249, 232), false);
        text(c, "EPSG:4326", row.x + 447.0f, y + 34.0f, 15.0f, rgba(235, 244, 249, 232), false);
        drawMoreIcon(c, row.x + 551.0f, y + 27.0f, rgba(232, 241, 248, 215));
    }

    void drawLayerDetails(SkCanvas& c, const Rect& card) {
        c.drawRRect(rr(card, 10.0f), fill(rgba(11, 23, 33, 108)));
        c.drawRRect(rr(card, 10.0f), stroke(rgba(142, 170, 193, 135), 1.1f));

        constexpr float titleSize = 24.5f;
        const float titleX = card.x + 65.0f;
        const float titleBaseline = card.y + 43.0f;
        drawGeometryIcon(c, 0, card.x + 36.0f, card.y + 35.0f, rgb(64, 236, 225), 28.0f);
        text(c, "地块边界", titleX, titleBaseline, titleSize, rgba(239, 248, 252, 242), true);

        const float pillX = std::min(titleX + textWidth("地块边界", titleSize, true) + 16.0f, card.x + card.w - 106.0f);
        const Rect pill{pillX, card.y + 19.0f, 82.0f, 29.0f};
        const SkColor pillColors[] = {rgba(20, 212, 198, 108), rgba(10, 121, 125, 120)};
        const SkScalar pillPos[] = {0.0f, 1.0f};
        c.drawRoundRect(pill.sk(), 15.0f, 15.0f,
                        linearPaint(SkPoint::Make(pill.x, pill.y), SkPoint::Make(pill.x + pill.w, pill.y), pillColors, pillPos, 2));
        centeredText(c, "Polygon", pill, 14.0f, rgb(68, 236, 224), false);

        line(c, card.x + 20.0f, card.y + 64.0f, card.x + card.w - 20.0f, card.y + 64.0f, rgba(140, 170, 191, 104), 1.0f);

        drawInfo(c, card.x + 20.0f, card.y + 101.0f, "图层名称:", "地块边界");
        drawInfo(c, card.x + 20.0f, card.y + 137.0f, "数据源:", "parcels.shp");
        drawInfo(c, card.x + 20.0f, card.y + 173.0f, "坐标系:", "EPSG:4326");
        drawInfo(c, card.x + 20.0f, card.y + 225.0f, "创建时间:", "2024-05-16 14:32:18");

        drawInfo(c, card.x + 302.0f, card.y + 101.0f, "要素数量:", "6,523");
        drawInfo(c, card.x + 302.0f, card.y + 137.0f, "几何类型:", "Polygon");
        drawInfo(c, card.x + 302.0f, card.y + 173.0f, "范围:", "113.2142,22.4987");
        text(c, "-114.9876,23.9145", card.x + 360.0f, card.y + 199.0f, 16.0f, rgba(238, 246, 251, 232), false);
        drawInfo(c, card.x + 302.0f, card.y + 225.0f, "最后编辑:", "2024-05-16 15:47:09");
    }

    void drawInfo(SkCanvas& c, float x, float y, std::string_view label, std::string_view value) {
        text(c, label, x, y, 16.0f, rgba(192, 207, 220, 215), false);
        text(c, value, x + 76.0f, y, 16.0f, rgba(238, 246, 251, 232), false);
    }

    void drawStatusBar(SkCanvas& c, float width, float height) {
        const Rect bar{0.0f, height - 60.0f, width, 60.0f};
        const SkColor colors[] = {rgba(8, 16, 24, 214), rgba(4, 10, 16, 236)};
        const SkScalar pos[] = {0.0f, 1.0f};
        c.drawRect(bar.sk(),
                   linearPaint(SkPoint::Make(0.0f, bar.y), SkPoint::Make(0.0f, height), colors, pos, 2));
        line(c, 0.0f, bar.y + 0.5f, width, bar.y + 0.5f, rgba(108, 136, 158, 66), 1.0f);

        if (!statusContentPicture_) {
            statusContentPicture_ = recordPicture(
                SkRect::MakeWH(760.0f, 60.0f),
                "status_content_picture",
                [this](SkCanvas& pictureCanvas) { drawStatusBarContent(pictureCanvas); });
        }
        c.save();
        c.translate(0.0f, height - 60.0f);
        c.drawPicture(statusContentPicture_.get());
        c.restore();
    }

    void drawStatusBarContent(SkCanvas& c) {
        drawLayerStack(c, 63.0f, 30.0f, 31.0f, rgba(216, 226, 238, 210), false);
        text(c, "图层:5", 89.0f, 38.0f, 20.0f, rgba(222, 232, 241, 235), false);
        drawEyeIcon(c, 293.0f, 30.0f, rgba(216, 226, 238, 210), 29.0f);
        text(c, "可见:5", 323.0f, 38.0f, 20.0f, rgba(222, 232, 241, 235), false);
        drawChartIcon(c, 524.0f, 30.0f, rgba(216, 226, 238, 210));
        text(c, "总要素:11,272", 549.0f, 38.0f, 20.0f, rgba(222, 232, 241, 235), false);
    }

    void drawCompass(SkCanvas& c, float cx, float cy) {
        if (!compassPicture_) {
            compassPicture_ = recordPicture(
                SkRect::MakeXYWH(-80.0f, -90.0f, 160.0f, 180.0f),
                "compass_picture",
                [this](SkCanvas& pictureCanvas) { drawCompassContent(pictureCanvas); });
        }
        c.save();
        c.translate(cx, cy);
        c.drawPicture(compassPicture_.get());
        c.restore();
    }

    void drawCompassContent(SkCanvas& c) {
        constexpr float cx = 0.0f;
        constexpr float cy = 0.0f;
        const float r = 51.0f;
        c.drawCircle(cx, cy, r, stroke(rgba(197, 213, 224, 145), 1.1f));
        c.drawCircle(cx, cy, r * 0.74f, stroke(rgba(197, 213, 224, 45), 1.0f));

        for (int i = 0; i < 24; ++i) {
            const float angle = -kPi * 0.5f + i * (2.0f * kPi / 24.0f);
            const float len = (i % 6 == 0) ? 11.0f : 6.0f;
            const float x1 = cx + std::cos(angle) * (r - len);
            const float y1 = cy + std::sin(angle) * (r - len);
            const float x2 = cx + std::cos(angle) * r;
            const float y2 = cy + std::sin(angle) * r;
            line(c, x1, y1, x2, y2, rgba(207, 220, 230, i % 6 == 0 ? 150 : 95), 1.0f);
        }

        centeredText(c, "N", {cx - 10.0f, cy - r - 29.0f, 20.0f, 20.0f}, 17.0f, rgba(237, 245, 251, 235), true);
        centeredText(c, "E", {cx + r + 7.0f, cy - 10.0f, 20.0f, 20.0f}, 16.0f, rgba(237, 245, 251, 220), false);
        centeredText(c, "S", {cx - 10.0f, cy + r + 10.0f, 20.0f, 20.0f}, 16.0f, rgba(237, 245, 251, 220), false);
        centeredText(c, "W", {cx - r - 28.0f, cy - 10.0f, 20.0f, 20.0f}, 16.0f, rgba(237, 245, 251, 220), false);

        SkPathBuilder north;
        north.moveTo(cx, cy - r + 12.0f);
        north.lineTo(cx + 9.0f, cy - 1.0f);
        north.lineTo(cx, cy - 8.0f);
        north.lineTo(cx - 9.0f, cy - 1.0f);
        north.close();
        c.drawPath(north.detach(), fill(rgb(227, 74, 82)));

        SkPathBuilder south;
        south.moveTo(cx, cy + r - 13.0f);
        south.lineTo(cx + 9.0f, cy + 1.0f);
        south.lineTo(cx, cy + 8.0f);
        south.lineTo(cx - 9.0f, cy + 1.0f);
        south.close();
        c.drawPath(south.detach(), fill(rgba(224, 232, 239, 230)));
        c.drawCircle(cx, cy, 6.5f, fill(rgba(11, 20, 30, 240)));
        c.drawCircle(cx, cy, 3.0f, fill(rgba(190, 210, 222, 215)));
    }

    void drawLayerStack(SkCanvas& c, float cx, float cy, float size, SkColor color, bool glow) {
        if (glow) {
            c.drawCircle(cx, cy + 3.0f, size * 0.78f, fill(rgba(41, 233, 220, 42)));
        }
        std::string body;
        body += "<path d=\"M12 3.2 4.2 7.2 12 11.2l7.8-4-7.8-4Z\" ";
        body += svgFillAttrs(withAlpha(color, glow ? 210 : 235));
        body += "/>";
        body +=
            "<path d=\"M4.2 12 12 16l7.8-4\"/>"
            "<path d=\"M4.2 16.6 12 20.6l7.8-4\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 2.05f)), cx, cy, size);
    }

    void drawSearchIcon(SkCanvas& c, float cx, float cy, float r, SkColor color, float strokeWidth) {
        const std::string body = "<circle cx=\"10.5\" cy=\"10.5\" r=\"6.3\"/><path d=\"M15.4 15.4 21 21\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, strokeWidth)), cx, cy, r * 2.8f);
    }

    void drawUploadIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body =
            "<path d=\"M12 16.5V5.4\"/>"
            "<path d=\"M7.1 10.3 12 5.4l4.9 4.9\"/>"
            "<path d=\"M5.2 15.6v3.2h13.6v-3.2\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 2.15f)), cx, cy, 31.0f);
    }

    void drawPlusIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body = "<path d=\"M12 5v14\"/><path d=\"M5 12h14\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 1.9f)), cx, cy, 29.0f);
    }

    void drawDragDots(SkCanvas& c, float cx, float cy, SkColor color) {
        for (int row = -1; row <= 1; ++row) {
            c.drawCircle(cx - 4.0f, cy + row * 6.0f, 1.9f, fill(color));
            c.drawCircle(cx + 4.0f, cy + row * 6.0f, 1.9f, fill(color));
        }
    }

    void drawGeometryIcon(SkCanvas& c, int icon, float cx, float cy, SkColor color, float size = 22.0f) {
        std::string body;
        switch (icon) {
        case 0:
            body = "<path d=\"M12.2 2.9 20.1 8.8 17.2 21 5.7 20.2 3.7 9.1 12.2 2.9Z\"/>";
            break;
        case 1:
            body = "<path d=\"M3.8 5.4 9.1 9.6 13.6 13.2 20.2 18.3\"/><circle cx=\"3.8\" cy=\"5.4\" r=\"1.35\"/><circle cx=\"20.2\" cy=\"18.3\" r=\"1.35\"/>";
            break;
        case 2:
            body = "<circle cx=\"12\" cy=\"12\" r=\"6.2\"/>";
            break;
        case 3:
            body = "<path d=\"M3.8 9.4c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/><path d=\"M3.8 14.8c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/>";
            break;
        default:
            body = "<path d=\"M5.1 5.1h13.8v13.8H5.1Z\"/>";
            break;
        }
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 2.1f)), cx, cy, size);
    }

    void drawEyeIcon(SkCanvas& c, float cx, float cy, SkColor color, float size = 27.0f) {
        std::string body = "<path d=\"M3.2 12s3.3-5.2 8.8-5.2 8.8 5.2 8.8 5.2-3.3 5.2-8.8 5.2S3.2 12 3.2 12Z\"/>";
        body += "<circle cx=\"12\" cy=\"12\" r=\"2.25\" ";
        body += svgFillAttrs(color);
        body += " stroke=\"none\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 1.9f)), cx, cy, size);
    }

    void drawMoreIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        c.drawCircle(cx, cy - 9.0f, 2.0f, fill(color));
        c.drawCircle(cx, cy, 2.0f, fill(color));
        c.drawCircle(cx, cy + 9.0f, 2.0f, fill(color));
    }

    void drawChartIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        std::string body = "<path d=\"M4.4 4.4h15.2v15.2H4.4Z\"/>";
        body += "<path d=\"M8 15.7v-4.2M12 15.7V8.2M16 15.7v-6\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 1.8f)), cx, cy, 29.0f);
    }

    void drawEditIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body =
            "<path d=\"M12 3.6H5.7c-1.2 0-2.1.9-2.1 2.1v12.6c0 1.2.9 2.1 2.1 2.1h12.6c1.2 0 2.1-.9 2.1-2.1V12\"/>"
            "<path d=\"M16.8 3.9c.8-.8 2.1-.8 2.9 0s.8 2.1 0 2.9l-8.9 8.9-4 1 1-4 9-8.8Z\"/>"
            "<path d=\"M15.5 5.2l3.3 3.3\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 2.05f)), cx, cy, 42.0f);
    }

    void drawPencilIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body =
            "<path d=\"M4.2 19.8 5 16.2 16.4 4.8l2.8 2.8L7.8 19l-3.6.8Z\"/>"
            "<path d=\"M14.8 6.4 17.6 9.2\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 2.0f)), cx, cy, 41.0f);
    }

    void drawTargetIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        std::string body =
            "<circle cx=\"12\" cy=\"12\" r=\"8.1\"/>"
            "<circle cx=\"12\" cy=\"12\" r=\"3.8\"/>";
        body += "<circle cx=\"12\" cy=\"12\" r=\"1.25\" ";
        body += svgFillAttrs(color);
        body += " stroke=\"none\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 1.85f)), cx, cy, 44.0f);
    }

    void drawDocIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body =
            "<path d=\"M6.1 3.5h11.8v17H6.1Z\"/>"
            "<path d=\"M9 7.2h6\"/>"
            "<path d=\"M9 11.5h6\"/>"
            "<path d=\"M9 15.8h6\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 1.95f)), cx, cy, 42.0f);
    }

    void drawSwitchIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body =
            "<path d=\"M4 7.2h15\"/>"
            "<path d=\"M15.1 3.9 19 7.2l-3.9 3.3\"/>"
            "<path d=\"M20 16.8H5\"/>"
            "<path d=\"M8.9 13.5 5 16.8l3.9 3.3\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 1.95f)), cx, cy, 42.0f);
    }

    void drawGearIcon(SkCanvas& c, float cx, float cy, SkColor color) {
        const std::string body =
            "<path d=\"M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.09a2 2 0 0 1 1 1.74v.5a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.38a2 2 0 0 0-.73-2.73l-.15-.09a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2Z\"/>"
            "<circle cx=\"12\" cy=\"12\" r=\"3\"/>";
        drawSvgIcon(c, svgWrap(body, svgStrokeAttrs(color, 2.0f)), cx, cy, 43.0f);
    }
};

class AppWindow {
public:
    ~AppWindow() {
        if (backgroundBrush_) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
    }

    int run(HINSTANCE instance, int showCmd) {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
        dpi_ = getSystemDpi();
        const float dpiScale = dpiScaleForDpi(dpi_);
        backgroundBrush_ = CreateSolidBrush(kWin32Background);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance = instance;
        wc.lpfnWndProc = &AppWindow::WndProc;
        wc.lpszClassName = L"SkiaLayerDeskWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = backgroundBrush_;
        RegisterClassExW(&wc);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        const int desiredClientWidth = static_cast<int>(std::round(kBaseWidth * dpiScale));
        const int desiredClientHeight = static_cast<int>(std::round(kBaseHeight * dpiScale));
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
                                    L"SkiaLayerDesk",
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

        BOOL dark = TRUE;
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
    D3DPresenter d3d_{kWin32Background};
    std::vector<uint32_t> pixels_;
    int cpuSurfaceWidth_ = 0;
    int cpuSurfaceHeight_ = 0;
    UINT dpi_ = static_cast<UINT>(kDefaultDpi);
    HBRUSH backgroundBrush_ = nullptr;
    bool paintActive_ = false;
    bool frameDirty_ = true;
    bool hasPresentedFrame_ = false;
    int presentedWidth_ = 0;
    int presentedHeight_ = 0;

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

    void requestRepaint(HWND hwnd, bool immediate) {
        const UINT flags = RDW_INVALIDATE | RDW_NOCHILDREN |
                           (immediate ? (RDW_ERASE | RDW_UPDATENOW) : 0);
        RedrawWindow(hwnd, nullptr, nullptr, flags);
    }

    void markFrameDirty() {
        frameDirty_ = true;
    }

    void eraseBackground(HWND hwnd, HDC hdc) {
        RECT client{};
        GetClientRect(hwnd, &client);
        if (backgroundBrush_) {
            FillRect(hdc, &client, backgroundBrush_);
            return;
        }

        SetDCBrushColor(hdc, kWin32Background);
        FillRect(hdc, &client, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
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
        case WM_WINDOWPOSCHANGING: {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            if (pos && !(pos->flags & SWP_NOSIZE)) {
                pos->flags |= SWP_NOCOPYBITS;
            }
            break;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                return 0;
            }
            app->markFrameDirty();
            app->requestRepaint(hwnd, true);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app->frameDirty_) {
                app->requestRepaint(hwnd, true);
            }
            return 0;
        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wParam);
            app->markFrameDirty();
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app->requestRepaint(hwnd, true);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_PAINT:
            app->paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            app->eraseBackground(hwnd, reinterpret_cast<HDC>(wParam));
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool renderD3D(HWND hwnd, int width, int height) {
        const auto traceStart = perf::Trace::now();
        const bool ok = d3d_.render(
            hwnd,
            width,
            height,
            [this](SkCanvas& canvas, int drawWidth, int drawHeight) {
                renderer_.draw(canvas, drawWidth, drawHeight, dpiScaleForDpi(dpi_));
            },
            [this](uint32_t* pixels, int drawWidth, int drawHeight, size_t rowBytes) {
                return renderCpuSurface(pixels, drawWidth, drawHeight, rowBytes);
            });
        perf::Trace::write("skia_app", "render_d3d", width, height, perf::Trace::elapsedMs(traceStart));
        return ok;
    }

    bool renderCpuSurface(uint32_t* pixels, int width, int height, size_t rowBytes) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!pixels || rowBytes < static_cast<size_t>(width) * sizeof(uint32_t)) {
            return false;
        }

        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
        if (!surface) {
            return false;
        }

        renderer_.draw(*surface->getCanvas(), width, height, dpiScaleForDpi(dpi_));
        return true;
    }

    bool renderCpuSurface(int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (width != cpuSurfaceWidth_ || height != cpuSurfaceHeight_ || pixels_.empty()) {
            cpuSurfaceWidth_ = width;
            cpuSurfaceHeight_ = height;
            pixels_.resize(static_cast<size_t>(cpuSurfaceWidth_) * static_cast<size_t>(cpuSurfaceHeight_));
        }

        return renderCpuSurface(pixels_.data(),
                                cpuSurfaceWidth_,
                                cpuSurfaceHeight_,
                                static_cast<size_t>(cpuSurfaceWidth_) * sizeof(uint32_t));
    }

    void renderCpuFallback(HDC hdc, const PAINTSTRUCT& ps, int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!renderCpuSurface(width, height)) {
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = cpuSurfaceWidth_;
        bmi.bmiHeader.biHeight = -cpuSurfaceHeight_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        const int paintX = std::max<LONG>(0, ps.rcPaint.left);
        const int paintY = std::max<LONG>(0, ps.rcPaint.top);
        const int paintRight = std::min<LONG>(cpuSurfaceWidth_, ps.rcPaint.right);
        const int paintBottom = std::min<LONG>(cpuSurfaceHeight_, ps.rcPaint.bottom);
        const int paintWidth = paintRight - paintX;
        const int paintHeight = paintBottom - paintY;
        if (paintWidth > 0 && paintHeight > 0) {
            StretchDIBits(hdc,
                          paintX,
                          paintY,
                          paintWidth,
                          paintHeight,
                          paintX,
                          paintY,
                          paintWidth,
                          paintHeight,
                          pixels_.data(),
                          &bmi,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        }
    }

    void paint(HWND hwnd) {
        if (paintActive_) {
            ValidateRect(hwnd, nullptr);
            return;
        }
        const auto traceStart = perf::Trace::now();
        paintActive_ = true;

        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        dpi_ = getWindowDpi(hwnd);

        if (!frameDirty_ && hasPresentedFrame_ && width == presentedWidth_ && height == presentedHeight_) {
            EndPaint(hwnd, &ps);
            paintActive_ = false;
            perf::Trace::write("skia_app", "paint_reuse_presented_frame", width, height, perf::Trace::elapsedMs(traceStart));
            return;
        }

        const auto renderStart = perf::Trace::now();
        const bool renderedD3D = renderD3D(hwnd, width, height);
        perf::Trace::write("skia_app", renderedD3D ? "render_d3d_ok" : "render_d3d_fail", width, height, perf::Trace::elapsedMs(renderStart));
        if (renderedD3D) {
            hasPresentedFrame_ = true;
            presentedWidth_ = width;
            presentedHeight_ = height;
            frameDirty_ = false;
        }
        if (!renderedD3D) {
            const auto fallbackStart = perf::Trace::now();
            renderCpuFallback(hdc, ps, width, height);
            perf::Trace::write("skia_app", "render_gdi_fallback", width, height, perf::Trace::elapsedMs(fallbackStart));
        }

        EndPaint(hwnd, &ps);
        paintActive_ = false;
        perf::Trace::write("skia_app", "paint_total", width, height, perf::Trace::elapsedMs(traceStart));
    }
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    SetUnhandledExceptionFilter(writeMiniDump);
    AppWindow app;
    return app.run(instance, showCmd);
}
