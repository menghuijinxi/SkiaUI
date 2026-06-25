#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkVertices.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkTypeface_win.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kBaseWidth = 1672;
constexpr int kBaseHeight = 941;
constexpr int kRmlSupersample = 2;
constexpr float kDefaultDpi = 96.0f;
constexpr float kPi = 3.14159265358979323846f;
float gRmlTextureScale = 1.0f;

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    SkRect sk() const {
        return SkRect::MakeXYWH(x, y, w, h);
    }
};

SkColor rgb(uint8_t r, uint8_t g, uint8_t b) {
    return SkColorSetRGB(r, g, b);
}

SkColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return SkColorSetARGB(a, r, g, b);
}

Gdiplus::Color argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return Gdiplus::Color(a, r, g, b);
}

uint8_t clampByte(float value) {
    return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, value)));
}

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(0, count)), L'\0');
    if (count > 0) {
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), count);
    }
    return wide;
}

bool getEncoderClsid(const WCHAR* mimeType, CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (count == 0 || bytes == 0) {
        return false;
    }

    std::vector<uint8_t> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
        return false;
    }

    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mimeType) == 0) {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

class RmlSystem final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_).count();
    }

    bool LogMessage(Rml::Log::Type, const Rml::String& message) override {
        std::wstring wide = utf8ToWide(message);
        wide += L"\n";
        OutputDebugStringW(wide.c_str());
        return true;
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

class LayerIconElement final : public Rml::Element {
public:
    explicit LayerIconElement(const Rml::String& tag) : Rml::Element(tag) {}

    ~LayerIconElement() {
        texture_.Release();
    }

protected:
    void OnRender() override {
        Rml::RenderManager* renderManager = GetRenderManager();
        if (!renderManager) {
            return;
        }

        Rml::Vector2f size = GetBox().GetSize();
        if (size.x <= 0.0f || size.y <= 0.0f) {
            return;
        }

        const Rml::String name = GetAttribute<Rml::String>("name", Rml::String());
        const SkColor color = toSkColor(GetComputedValues().color(), GetComputedValues().opacity());
        const float textureScale = std::max(1.0f, gRmlTextureScale);
        const int textureWidth = std::max(1, static_cast<int>(std::ceil(size.x * textureScale)));
        const int textureHeight = std::max(1, static_cast<int>(std::ceil(size.y * textureScale)));

        if (!ensureTexture(*renderManager, name, color, textureWidth, textureHeight)) {
            return;
        }

        Rml::Mesh mesh;
        Rml::MeshUtilities::GenerateQuad(mesh,
                                         GetAbsoluteOffset(Rml::BoxArea::Content),
                                         size,
                                         Rml::ColourbPremultiplied(255, 255, 255, 255));
        Rml::Geometry geometry = renderManager->MakeGeometry(std::move(mesh));
        geometry.Render(Rml::Vector2f(0.0f, 0.0f), static_cast<Rml::Texture>(texture_));
    }

private:
    Rml::CallbackTexture texture_;
    std::string textureKey_;

    bool ensureTexture(Rml::RenderManager& renderManager,
                       const Rml::String& name,
                       SkColor color,
                       int textureWidth,
                       int textureHeight) {
        std::string svg = svgForIcon(name, color);
        if (svg.empty()) {
            return false;
        }

        std::string key = name;
        key += ':';
        key += std::to_string(textureWidth);
        key += 'x';
        key += std::to_string(textureHeight);
        key += ':';
        key += std::to_string(color);
        if (key == textureKey_) {
            return true;
        }

        texture_.Release();
        textureKey_ = key;
        texture_ = renderManager.MakeCallbackTexture(
            [svg = std::move(svg), textureWidth, textureHeight](const Rml::CallbackTextureInterface& textureInterface) {
                std::vector<uint8_t> rgba(static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 4u, 0u);
                const SkImageInfo info = SkImageInfo::Make(textureWidth,
                                                           textureHeight,
                                                           kRGBA_8888_SkColorType,
                                                           kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, rgba.data(), static_cast<size_t>(textureWidth) * 4u);
                if (!surface) {
                    return false;
                }

                SkMemoryStream stream(svg.data(), svg.size(), false);
                sk_sp<SkSVGDOM> dom = SkSVGDOM::MakeFromStream(stream);
                if (!dom) {
                    return false;
                }

                SkCanvas* canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);
                dom->setContainerSize(SkSize::Make(24.0f, 24.0f));
                const float iconSize = static_cast<float>(std::min(textureWidth, textureHeight));
                canvas->translate((static_cast<float>(textureWidth) - iconSize) * 0.5f,
                                  (static_cast<float>(textureHeight) - iconSize) * 0.5f);
                canvas->scale(iconSize / 24.0f, iconSize / 24.0f);
                dom->render(canvas);
                return textureInterface.GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()),
                                                        {textureWidth, textureHeight});
            });
        return true;
    }

    static SkColor toSkColor(Rml::Colourb color, float opacity) {
        const float alpha = static_cast<float>(color.alpha) * std::clamp(opacity, 0.0f, 1.0f);
        return SkColorSetARGB(clampByte(alpha),
                              color.red,
                              color.green,
                              color.blue);
    }

    static SkColor withAlpha(SkColor color, uint8_t alpha) {
        const int scaledAlpha = static_cast<int>(SkColorGetA(color)) * static_cast<int>(alpha) / 255;
        return SkColorSetARGB(static_cast<U8CPU>(scaledAlpha),
                              SkColorGetR(color),
                              SkColorGetG(color),
                              SkColorGetB(color));
    }

    static std::string svgHex(SkColor color) {
        char buf[16];
        std::snprintf(buf,
                      sizeof(buf),
                      "#%02X%02X%02X",
                      SkColorGetR(color),
                      SkColorGetG(color),
                      SkColorGetB(color));
        return buf;
    }

    static std::string svgOpacity(SkColor color) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<float>(SkColorGetA(color)) / 255.0f);
        return buf;
    }

    static std::string svgFloat(float value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }

    static std::string svgStrokeAttrs(SkColor color, float width) {
        return "stroke=\"" + svgHex(color) + "\" stroke-opacity=\"" + svgOpacity(color) +
               "\" stroke-width=\"" + svgFloat(width) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
    }

    static std::string svgFillAttrs(SkColor color) {
        return "fill=\"" + svgHex(color) + "\" fill-opacity=\"" + svgOpacity(color) + "\"";
    }

    static std::string svgWrap(std::string_view body, const std::string& attrs) {
        std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" ";
        svg += attrs;
        svg += ">";
        svg += body;
        svg += "</svg>";
        return svg;
    }

    static std::string svgForIcon(const Rml::String& name, SkColor color) {
        if (name == "menu") {
            return svgWrap("<path d=\"M4 6h16\"/><path d=\"M4 12h16\"/><path d=\"M4 18h16\"/>",
                           svgStrokeAttrs(color, 2.4f));
        }

        if (name == "layer" || name == "layer-active") {
            std::string body;
            body += "<path d=\"M12 3.2 4.2 7.2 12 11.2l7.8-4-7.8-4Z\" ";
            body += svgFillAttrs(withAlpha(color, 235));
            body += "/>";
            body += "<path d=\"M4.2 12 12 16l7.8-4\"/>"
                    "<path d=\"M4.2 16.6 12 20.6l7.8-4\"/>";
            return svgWrap(body, svgStrokeAttrs(color, 2.05f));
        }

        if (name == "search") {
            return svgWrap("<circle cx=\"10.5\" cy=\"10.5\" r=\"6.3\"/><path d=\"M15.4 15.4 21 21\"/>",
                           svgStrokeAttrs(color, 2.7f));
        }

        if (name == "upload") {
            return svgWrap("<path d=\"M12 16.5V5.4\"/>"
                           "<path d=\"M7.1 10.3 12 5.4l4.9 4.9\"/>"
                           "<path d=\"M5.2 15.6v3.2h13.6v-3.2\"/>",
                           svgStrokeAttrs(color, 2.15f));
        }

        if (name == "plus") {
            return svgWrap("<path d=\"M12 5v14\"/><path d=\"M5 12h14\"/>", svgStrokeAttrs(color, 1.9f));
        }

        if (name == "edit") {
            return svgWrap("<path d=\"M12 3.6H5.7c-1.2 0-2.1.9-2.1 2.1v12.6c0 1.2.9 2.1 2.1 2.1h12.6c1.2 0 2.1-.9 2.1-2.1V12\"/>"
                           "<path d=\"M16.8 3.9c.8-.8 2.1-.8 2.9 0s.8 2.1 0 2.9l-8.9 8.9-4 1 1-4 9-8.8Z\"/>"
                           "<path d=\"M15.5 5.2l3.3 3.3\"/>",
                           svgStrokeAttrs(color, 2.05f));
        }

        if (name == "pencil") {
            return svgWrap("<path d=\"M4.2 19.8 5 16.2 16.4 4.8l2.8 2.8L7.8 19l-3.6.8Z\"/>"
                           "<path d=\"M14.8 6.4 17.6 9.2\"/>",
                           svgStrokeAttrs(color, 2.0f));
        }

        if (name == "target") {
            std::string body = "<circle cx=\"12\" cy=\"12\" r=\"8.1\"/>"
                               "<circle cx=\"12\" cy=\"12\" r=\"3.8\"/>";
            body += "<circle cx=\"12\" cy=\"12\" r=\"1.25\" ";
            body += svgFillAttrs(color);
            body += " stroke=\"none\"/>";
            return svgWrap(body, svgStrokeAttrs(color, 1.85f));
        }

        if (name == "doc") {
            return svgWrap("<path d=\"M6.1 3.5h11.8v17H6.1Z\"/>"
                           "<path d=\"M9 7.2h6\"/>"
                           "<path d=\"M9 11.5h6\"/>"
                           "<path d=\"M9 15.8h6\"/>",
                           svgStrokeAttrs(color, 1.95f));
        }

        if (name == "switch") {
            return svgWrap("<path d=\"M4 7.2h15\"/>"
                           "<path d=\"M15.1 3.9 19 7.2l-3.9 3.3\"/>"
                           "<path d=\"M20 16.8H5\"/>"
                           "<path d=\"M8.9 13.5 5 16.8l3.9 3.3\"/>",
                           svgStrokeAttrs(color, 1.95f));
        }

        if (name == "gear") {
            return svgWrap("<path d=\"M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.09a2 2 0 0 1 1 1.74v.5a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.38a2 2 0 0 0-.73-2.73l-.15-.09a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2Z\"/>"
                           "<circle cx=\"12\" cy=\"12\" r=\"3\"/>",
                           svgStrokeAttrs(color, 2.0f));
        }

        if (name == "geom-poly") {
            return svgWrap("<path d=\"M12.2 2.9 20.1 8.8 17.2 21 5.7 20.2 3.7 9.1 12.2 2.9Z\"/>",
                           svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-line") {
            return svgWrap("<path d=\"M3.8 5.4 9.1 9.6 13.6 13.2 20.2 18.3\"/>"
                           "<circle cx=\"3.8\" cy=\"5.4\" r=\"1.35\"/>"
                           "<circle cx=\"20.2\" cy=\"18.3\" r=\"1.35\"/>",
                           svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-point") {
            return svgWrap("<circle cx=\"12\" cy=\"12\" r=\"6.2\"/>", svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-river") {
            return svgWrap("<path d=\"M3.8 9.4c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/>"
                           "<path d=\"M3.8 14.8c3.3 2.4 6.1 2.4 9.3 0 2.7-2 5-2.1 7.1-.3\"/>",
                           svgStrokeAttrs(color, 2.1f));
        }

        if (name == "geom-box") {
            return svgWrap("<path d=\"M5.1 5.1h13.8v13.8H5.1Z\"/>", svgStrokeAttrs(color, 2.1f));
        }

        if (name == "eye") {
            std::string body = "<path d=\"M3.2 12s3.3-5.2 8.8-5.2 8.8 5.2 8.8 5.2-3.3 5.2-8.8 5.2S3.2 12 3.2 12Z\"/>";
            body += "<circle cx=\"12\" cy=\"12\" r=\"2.25\" ";
            body += svgFillAttrs(color);
            body += " stroke=\"none\"/>";
            return svgWrap(body, svgStrokeAttrs(color, 1.9f));
        }

        if (name == "chart") {
            return svgWrap("<path d=\"M4.4 4.4h15.2v15.2H4.4Z\"/>"
                           "<path d=\"M8 15.7v-4.2M12 15.7V8.2M16 15.7v-6\"/>",
                           svgStrokeAttrs(color, 1.8f));
        }

        if (name == "drag") {
            std::string body;
            for (float y : {6.0f, 12.0f, 18.0f}) {
                body += "<circle cx=\"8\" cy=\"" + svgFloat(y) + "\" r=\"1.9\" ";
                body += svgFillAttrs(color);
                body += " stroke=\"none\"/>";
                body += "<circle cx=\"16\" cy=\"" + svgFloat(y) + "\" r=\"1.9\" ";
                body += svgFillAttrs(color);
                body += " stroke=\"none\"/>";
            }
            return svgWrap(body, "stroke=\"none\"");
        }

        if (name == "more") {
            std::string body;
            for (float y : {5.0f, 12.0f, 19.0f}) {
                body += "<circle cx=\"12\" cy=\"" + svgFloat(y) + "\" r=\"2\" ";
                body += svgFillAttrs(color);
                body += " stroke=\"none\"/>";
            }
            return svgWrap(body, "stroke=\"none\"");
        }

        if (name == "compass") {
            std::string body = "<circle cx=\"12\" cy=\"12\" r=\"10.8\"/>";
            const SkColor tick = withAlpha(color, 180);
            for (int i = 0; i < 24; ++i) {
                const float angle = -kPi * 0.5f + static_cast<float>(i) * (2.0f * kPi / 24.0f);
                const float outer = 22.0f;
                const float inner = outer - ((i % 6 == 0) ? 4.6f : 2.6f);
                const float x1 = 12.0f + std::cos(angle) * inner * 0.48f;
                const float y1 = 12.0f + std::sin(angle) * inner * 0.48f;
                const float x2 = 12.0f + std::cos(angle) * outer * 0.48f;
                const float y2 = 12.0f + std::sin(angle) * outer * 0.48f;
                body += "<path d=\"M" + svgFloat(x1) + " " + svgFloat(y1) + " " + svgFloat(x2) + " " + svgFloat(y2) +
                        "\" " + svgStrokeAttrs(tick, 0.35f) + "/>";
            }
            body += "<path d=\"M12 3 14.2 11.5 12 10 9.8 11.5Z\" ";
            body += svgFillAttrs(SkColorSetARGB(SkColorGetA(color), 227, 74, 82));
            body += " stroke=\"none\"/>";
            body += "<path d=\"M12 21 14.2 12.5 12 14 9.8 12.5Z\" ";
            body += svgFillAttrs(SkColorSetARGB(static_cast<U8CPU>(std::min(230, static_cast<int>(SkColorGetA(color)))), 224, 232, 239));
            body += " stroke=\"none\"/>";
            return svgWrap(body, svgStrokeAttrs(withAlpha(color, 145), 0.35f));
        }

        return {};
    }

};

class SkiaFontEngine final : public Rml::FontEngineInterface {
public:
    struct Face {
        int size = 16;
        bool bold = false;
        sk_sp<SkTypeface> typeface;
        Rml::FontMetrics metrics{};
    };

    SkiaFontEngine() {
        fontMgr_ = SkFontMgr_New_DirectWrite();
        if (!fontMgr_) {
            fontMgr_ = SkFontMgr_New_GDI();
        }
    }

    Rml::FontFaceHandle GetFontFaceHandle(const Rml::String& family,
                                          Rml::Style::FontStyle,
                                          Rml::Style::FontWeight weight,
                                          int size) override {
        const int clampedSize = std::max(1, size);
        const bool bold = static_cast<int>(weight) >= static_cast<int>(Rml::Style::FontWeight::Bold);
        const uint64_t key = (static_cast<uint64_t>(clampedSize) << 1) | (bold ? 1u : 0u);
        auto [it, inserted] = faces_.try_emplace(key);
        if (inserted) {
            Face& face = it->second;
            face.size = clampedSize;
            face.bold = bold;
            face.typeface = pickTypeface(family, bold);
            face.metrics = measureMetrics(face);
        }
        return reinterpret_cast<Rml::FontFaceHandle>(&it->second);
    }

    const Rml::FontMetrics& GetFontMetrics(Rml::FontFaceHandle handle) override {
        return face(handle).metrics;
    }

    int GetStringWidth(Rml::FontFaceHandle handle,
                       Rml::StringView string,
                       const Rml::TextShapingContext& textShaping,
                       Rml::Character = Rml::Character::Null) override {
        return measureString(face(handle), string, textShaping.letter_spacing);
    }

    int GenerateString(Rml::RenderManager& renderManager,
                       Rml::FontFaceHandle handle,
                       Rml::FontEffectsHandle,
                       Rml::StringView string,
                       Rml::Vector2f position,
                       Rml::ColourbPremultiplied color,
                       float,
                       const Rml::TextShapingContext& textShaping,
                       Rml::TexturedMeshList& meshList) override {
        const Face& fontFace = face(handle);
        const Rml::String text(string.begin(), string.end());
        const int width = measureString(fontFace, string, textShaping.letter_spacing);
        if (text.empty() || width <= 0) {
            return std::max(0, width);
        }

        const float textureScale = textureScale_;
        const float texturePadding = 4.0f;
        const float logicalTextureWidth = static_cast<float>(width) + texturePadding * 2.0f;
        const float logicalTextureHeight = std::ceil(fontFace.metrics.line_spacing) + texturePadding * 2.0f;
        const int textureWidth = std::max(1, static_cast<int>(std::ceil(logicalTextureWidth * textureScale)));
        const int textureHeight = std::max(1, static_cast<int>(std::ceil(logicalTextureHeight * textureScale)));
        const float baselineY = texturePadding + fontFace.metrics.ascent;

        Rml::CallbackTexture callback = renderManager.MakeCallbackTexture(
            [text, fontFace, color, textureScale, textureWidth, textureHeight, texturePadding, baselineY](const Rml::CallbackTextureInterface& textureInterface) {
                std::vector<uint8_t> rgba(static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 4u, 0u);

                const SkImageInfo info = SkImageInfo::Make(textureWidth,
                                                           textureHeight,
                                                           kRGBA_8888_SkColorType,
                                                           kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, rgba.data(), static_cast<size_t>(textureWidth) * 4u);
                if (surface) {
                    SkCanvas* canvas = surface->getCanvas();
                    canvas->clear(SK_ColorTRANSPARENT);
                    SkPaint paint;
                    paint.setAntiAlias(true);
                    paint.setStyle(SkPaint::kFill_Style);
                    paint.setColor(toSkColor(color));

                    SkFont font(fontFace.typeface, static_cast<float>(fontFace.size) * textureScale);
                    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
                    font.setHinting(SkFontHinting::kNormal);
                    font.setSubpixel(true);
                    canvas->drawSimpleText(text.data(),
                                           text.size(),
                                           SkTextEncoding::kUTF8,
                                           texturePadding * textureScale,
                                           baselineY * textureScale,
                                           font,
                                           paint);
                }

                return textureInterface.GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), {textureWidth, textureHeight});
            });

        Rml::TexturedMesh texturedMesh;
        texturedMesh.texture = static_cast<Rml::Texture>(callback);
        const Rml::Vector2f topLeft = (position + Rml::Vector2f(0.0f, -fontFace.metrics.ascent) -
                                      Rml::Vector2f(texturePadding, texturePadding));
        const Rml::Vector2f dimensions(static_cast<float>(textureWidth) / textureScale,
                                       static_cast<float>(textureHeight) / textureScale);
        Rml::MeshUtilities::GenerateQuad(texturedMesh.mesh, topLeft, dimensions, Rml::ColourbPremultiplied(255, 255, 255, 255));
        meshList.push_back(std::move(texturedMesh));
        liveTextures_.push_back(std::move(callback));
        return width;
    }

    int GetVersion(Rml::FontFaceHandle) override {
        return version_;
    }

    void ReleaseFontResources() override {
        releaseTextTextures();
    }

    void beginFrame(float textureScale) {
        textureScale_ = std::max(1.0f, textureScale);
        ++version_;
        releaseTextTextures();
    }

private:
    sk_sp<SkFontMgr> fontMgr_;
    float textureScale_ = 1.0f;
    int version_ = 1;

    static Face& face(Rml::FontFaceHandle handle) {
        return *reinterpret_cast<Face*>(handle);
    }

    sk_sp<SkTypeface> pickTypeface(const Rml::String& requestedFamily, bool bold) const {
        const SkFontStyle style = bold ? SkFontStyle::Bold() : SkFontStyle::Normal();
        std::array<const char*, 6> families = {
            requestedFamily.empty() ? nullptr : requestedFamily.c_str(),
            "Microsoft YaHei UI",
            "Microsoft YaHei",
            "Segoe UI",
            "Arial",
            nullptr};

        for (const char* family : families) {
            if (!fontMgr_ || !family) {
                continue;
            }
            sk_sp<SkTypeface> typeface = fontMgr_->matchFamilyStyle(family, style);
            if (typeface) {
                return typeface;
            }
        }
        return nullptr;
    }

    static SkFont font(const Face& fontFace) {
        SkFont skFont(fontFace.typeface, static_cast<float>(fontFace.size));
        skFont.setEdging(SkFont::Edging::kSubpixelAntiAlias);
        skFont.setHinting(SkFontHinting::kNormal);
        skFont.setSubpixel(true);
        return skFont;
    }

    static SkColor toSkColor(Rml::ColourbPremultiplied color) {
        if (color.alpha == 0) {
            return SK_ColorTRANSPARENT;
        }
        const auto unpremultiply = [alpha = color.alpha](uint8_t value) {
            return static_cast<uint8_t>(std::min(255, static_cast<int>(value) * 255 / static_cast<int>(alpha)));
        };
        return SkColorSetARGB(color.alpha,
                              unpremultiply(color.red),
                              unpremultiply(color.green),
                              unpremultiply(color.blue));
    }

    static Rml::FontMetrics measureMetrics(const Face& fontFace) {
        Rml::FontMetrics metrics{};
        metrics.size = fontFace.size;

        SkFontMetrics skMetrics{};
        font(fontFace).getMetrics(&skMetrics);
        metrics.ascent = std::max(1.0f, -skMetrics.fAscent);
        metrics.descent = std::max(1.0f, skMetrics.fDescent);
        metrics.line_spacing = std::max(static_cast<float>(fontFace.size),
                                        metrics.ascent + metrics.descent + std::max(0.0f, skMetrics.fLeading));
        metrics.x_height = skMetrics.fXHeight < 0.0f ? -skMetrics.fXHeight : static_cast<float>(fontFace.size) * 0.54f;

        SkScalar underlinePosition = 0.0f;
        SkScalar underlineThickness = 0.0f;
        metrics.underline_position = skMetrics.hasUnderlinePosition(&underlinePosition)
                                         ? underlinePosition
                                         : static_cast<float>(fontFace.size) * 0.12f;
        metrics.underline_thickness = skMetrics.hasUnderlineThickness(&underlineThickness)
                                          ? std::max(1.0f, underlineThickness)
                                          : std::max(1.0f, static_cast<float>(fontFace.size) * (fontFace.bold ? 0.08f : 0.06f));
        metrics.has_ellipsis = true;
        return metrics;
    }

    static int measureString(const Face& fontFace, Rml::StringView string, float letterSpacing) {
        if (string.empty()) {
            return 0;
        }
        const Rml::String text(string.begin(), string.end());
        SkRect bounds;
        const float width = font(fontFace).measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &bounds);
        const float spacing = std::max(0, static_cast<int>(Rml::StringUtilities::LengthUTF8(string)) - 1) * letterSpacing;
        return std::max(1, static_cast<int>(std::ceil(width + spacing)));
    }

    void releaseTextTextures() {
        for (Rml::CallbackTexture& texture : liveTextures_) {
            texture.Release();
        }
        liveTextures_.clear();
    }

    std::map<uint64_t, Face> faces_;
    std::vector<Rml::CallbackTexture> liveTextures_;
};

class SkiaRenderInterface final : public Rml::RenderInterface {
public:
    struct Geometry {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };

    struct Texture {
        int width = 0;
        int height = 0;
        sk_sp<SkImage> image;
    };

    void begin(uint32_t* pixels, int width, int height, float scale) {
        width_ = width;
        height_ = height;
        scale_ = std::max(0.1f, scale);
        scissorEnabled_ = false;
        scissor_ = Rml::Rectanglei::FromPositionSize({0, 0}, {width, height});

        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        surface_ = SkSurfaces::WrapPixels(info, pixels, static_cast<size_t>(width) * sizeof(uint32_t));
        canvas_ = surface_ ? surface_->getCanvas() : nullptr;
        if (canvas_) {
            canvas_->clear(SK_ColorTRANSPARENT);
            canvas_->scale(scale_, scale_);
        }
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        auto* geometry = new Geometry;
        geometry->vertices.assign(vertices.begin(), vertices.end());
        geometry->indices.assign(indices.begin(), indices.end());
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle) override {
        const auto* geometry = reinterpret_cast<const Geometry*>(handle);
        const auto* texture = reinterpret_cast<const Texture*>(textureHandle);
        if (!geometry || !canvas_ || geometry->vertices.empty() || geometry->indices.empty()) {
            return;
        }

        const bool hasTexture = texture && texture->image;
        if (!hasTexture) {
            return;
        }

        if (geometry->vertices.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max()) ||
            geometry->indices.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return;
        }

        std::vector<SkPoint> positions;
        std::vector<SkColor> colors;
        std::vector<SkPoint> texCoords;
        std::vector<uint16_t> indices;
        positions.reserve(geometry->vertices.size());
        colors.reserve(geometry->vertices.size());
        texCoords.reserve(geometry->vertices.size());

        for (const Rml::Vertex& vertex : geometry->vertices) {
            positions.push_back(SkPoint::Make(vertex.position.x + translation.x, vertex.position.y + translation.y));
            colors.push_back(toSkColor(vertex.colour));
            texCoords.push_back(SkPoint::Make(vertex.tex_coord.x * static_cast<float>(texture->width),
                                              vertex.tex_coord.y * static_cast<float>(texture->height)));
        }

        indices.reserve(geometry->indices.size());
        for (int index : geometry->indices) {
            if (index < 0 || index > std::numeric_limits<uint16_t>::max()) {
                return;
            }
            indices.push_back(static_cast<uint16_t>(index));
        }

        sk_sp<SkVertices> vertices = SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode,
                                                          static_cast<int>(positions.size()),
                                                          positions.data(),
                                                          texCoords.empty() ? nullptr : texCoords.data(),
                                                          colors.data(),
                                                          static_cast<int>(indices.size()),
                                                          indices.data());
        if (!vertices) {
            return;
        }

        SkPaint paint;
        paint.setAntiAlias(false);
        paint.setColor(SK_ColorWHITE);
        paint.setBlendMode(SkBlendMode::kSrcOver);
        paint.setShader(texture->image->makeShader(SkTileMode::kClamp,
                                                   SkTileMode::kClamp,
                                                   SkSamplingOptions(SkFilterMode::kLinear)));
        canvas_->drawVertices(vertices, SkBlendMode::kModulate, paint);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        delete reinterpret_cast<Geometry*>(handle);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& textureDimensions, const Rml::String&) override {
        textureDimensions = {0, 0};
        return 0;
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i sourceDimensions) override {
        if (sourceDimensions.x <= 0 || sourceDimensions.y <= 0 || source.empty()) {
            return 0;
        }
        auto* texture = new Texture;
        texture->width = sourceDimensions.x;
        texture->height = sourceDimensions.y;
        sk_sp<SkData> data = SkData::MakeWithCopy(source.data(), source.size());
        const SkImageInfo info = SkImageInfo::Make(sourceDimensions.x,
                                                   sourceDimensions.y,
                                                   kRGBA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        texture->image = SkImages::RasterFromData(info, std::move(data), static_cast<size_t>(sourceDimensions.x) * 4u);
        if (!texture->image) {
            delete texture;
            return 0;
        }
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        delete reinterpret_cast<Texture*>(texture);
    }

    void EnableScissorRegion(bool enable) override {
        if (scissorEnabled_ == enable) {
            return;
        }
        scissorEnabled_ = enable;
        updateScissorClip();
    }

    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_ = region;
        if (scissorEnabled_) {
            updateScissorClip();
        }
    }

private:
    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0f;
    bool scissorEnabled_ = false;
    int scissorSaveCount_ = 0;
    Rml::Rectanglei scissor_ = Rml::Rectanglei::FromPositionSize({0, 0}, {0, 0});
    static SkColor toSkColor(Rml::ColourbPremultiplied color) {
        if (color.alpha == 0) {
            return SK_ColorTRANSPARENT;
        }
        const auto unpremultiply = [alpha = color.alpha](uint8_t value) {
            return static_cast<uint8_t>(std::min(255, static_cast<int>(value) * 255 / static_cast<int>(alpha)));
        };
        return SkColorSetARGB(color.alpha,
                              unpremultiply(color.red),
                              unpremultiply(color.green),
                              unpremultiply(color.blue));
    }

    void updateScissorClip() {
        if (!canvas_) {
            return;
        }
        if (scissorSaveCount_ != 0) {
            canvas_->restoreToCount(scissorSaveCount_);
            scissorSaveCount_ = 0;
        }
        if (!scissorEnabled_) {
            return;
        }
        scissorSaveCount_ = canvas_->save();
        canvas_->clipRect(SkRect::MakeLTRB(static_cast<float>(scissor_.Left()),
                                           static_cast<float>(scissor_.Top()),
                                           static_cast<float>(scissor_.Right()),
                                           static_cast<float>(scissor_.Bottom())),
                          true);
    }
};

class SkiaChromeRenderer {
public:
    void drawApp(SkCanvas& c, float width, float height) const {
        drawBackground(c, width, height);
        drawSidebar(c, height);
        drawLayerPanel(c);
        drawCompass(c, width - 94.0f, height - 162.0f);
        drawStatusBar(c, width, height);
        c.drawRoundRect(SkRect::MakeXYWH(2.0f, 2.0f, width - 4.0f, height - 4.0f),
                        10.0f,
                        10.0f,
                        stroke(rgba(125, 155, 173, 105), 1.0f));
    }

private:
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
        p.setStrokeCap(SkPaint::kRound_Cap);
        p.setStrokeJoin(SkPaint::kRound_Join);
        p.setColor(color);
        return p;
    }

    SkRRect rr(const Rect& r, float radius) const {
        return SkRRect::MakeRectXY(r.sk(), radius, radius);
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

    void drawBackground(SkCanvas& c, float width, float height) const {
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

    void drawSidebar(SkCanvas& c, float height) const {
        constexpr float sidebarW = 118.0f;
        const Rect sidebar{0.0f, 0.0f, sidebarW, height - 60.0f};
        const SkColor colors[] = {rgba(5, 10, 16, 238), rgba(10, 18, 27, 218)};
        const SkScalar pos[] = {0.0f, 1.0f};
        c.drawRect(sidebar.sk(),
                   linearPaint(SkPoint::Make(0.0f, 0.0f), SkPoint::Make(sidebarW, 0.0f), colors, pos, 2));
        line(c, sidebarW - 0.5f, 0.0f, sidebarW - 0.5f, height - 60.0f, rgba(83, 118, 143, 46), 1.0f);

        const Rect active{3.0f, 88.0f, sidebarW - 3.0f, 102.0f};
        const SkColor activeColors[] = {rgba(16, 224, 207, 38), rgba(81, 178, 203, 16)};
        c.drawRect(active.sk(),
                   linearPaint(SkPoint::Make(active.x, 0.0f), SkPoint::Make(active.x + active.w, 0.0f), activeColors, pos, 2));
        c.drawRect(SkRect::MakeXYWH(3.0f, 88.0f, 5.0f, 102.0f), fill(rgb(34, 224, 211)));
    }

    void drawLayerPanel(SkCanvas& c) const {
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

        drawActionButton(c, {160.0f, 110.0f, 253.0f, 47.0f}, true);
        drawActionButton(c, {431.0f, 110.0f, 266.0f, 47.0f}, false);
        drawSearchBox(c, {161.0f, 180.0f, 536.0f, 46.0f});
        drawTableRows(c);
        drawLayerDetails(c, {149.0f, 566.0f, 568.0f, 262.0f});
    }

    void drawActionButton(SkCanvas& c, const Rect& r, bool primary) const {
        if (primary) {
            const SkColor colors[] = {rgba(18, 190, 178, 130), rgba(8, 95, 102, 150)};
            const SkScalar pos[] = {0.0f, 1.0f};
            c.drawRRect(rr(r, 7.0f),
                        linearPaint(SkPoint::Make(r.x, r.y), SkPoint::Make(r.x + r.w, r.y + r.h), colors, pos, 2));
            c.drawRRect(rr(r, 7.0f), stroke(rgba(38, 237, 218, 175), 1.1f));
        } else {
            c.drawRRect(rr(r, 7.0f), fill(rgba(19, 32, 45, 104)));
            c.drawRRect(rr(r, 7.0f), stroke(rgba(142, 170, 193, 142), 1.1f));
        }
    }

    void drawSearchBox(SkCanvas& c, const Rect& r) const {
        c.drawRRect(rr(r, 7.0f), fill(rgba(11, 22, 32, 130)));
        c.drawRRect(rr(r, 7.0f), stroke(rgba(132, 164, 190, 130), 1.15f));
    }

    void drawTableRows(SkCanvas& c) const {
        drawLayerRow(c, 280.0f, true);
        drawLayerRow(c, 334.0f, false);
        drawLayerRow(c, 388.0f, false);
        drawLayerRow(c, 442.0f, false);
        drawLayerRow(c, 496.0f, false);
    }

    void drawLayerRow(SkCanvas& c, float y, bool selected) const {
        const Rect row{149.0f, y, 570.0f, 54.0f};
        if (selected) {
            const SkColor colors[] = {rgba(0, 168, 150, 150), rgba(20, 173, 166, 105)};
            const SkScalar pos[] = {0.0f, 1.0f};
            c.drawRect(row.sk(),
                       linearPaint(SkPoint::Make(row.x, y), SkPoint::Make(row.x + row.w, y), colors, pos, 2));
            return;
        }

        c.drawRRect(rr({row.x, row.y, row.w, row.h - 1.0f}, 4.0f), fill(rgba(11, 23, 33, 60)));
        line(c, row.x + 2.0f, y + row.h - 0.5f, row.x + row.w - 2.0f, y + row.h - 0.5f, rgba(120, 149, 169, 32), 1.0f);
    }

    void drawLayerDetails(SkCanvas& c, const Rect& card) const {
        c.drawRRect(rr(card, 10.0f), fill(rgba(11, 23, 33, 108)));
        c.drawRRect(rr(card, 10.0f), stroke(rgba(142, 170, 193, 135), 1.1f));

        const Rect pill{327.0f, 585.0f, 82.0f, 29.0f};
        const SkColor pillColors[] = {rgba(20, 212, 198, 108), rgba(10, 121, 125, 120)};
        const SkScalar pillPos[] = {0.0f, 1.0f};
        c.drawRoundRect(pill.sk(), 15.0f, 15.0f,
                        linearPaint(SkPoint::Make(pill.x, pill.y), SkPoint::Make(pill.x + pill.w, pill.y), pillColors, pillPos, 2));

        line(c, card.x + 20.0f, card.y + 64.0f, card.x + card.w - 20.0f, card.y + 64.0f, rgba(140, 170, 191, 104), 1.0f);
    }

    void drawStatusBar(SkCanvas& c, float width, float height) const {
        const Rect bar{0.0f, height - 60.0f, width, 60.0f};
        const SkColor colors[] = {rgba(8, 16, 24, 214), rgba(4, 10, 16, 236)};
        const SkScalar pos[] = {0.0f, 1.0f};
        c.drawRect(bar.sk(),
                   linearPaint(SkPoint::Make(0.0f, bar.y), SkPoint::Make(0.0f, height), colors, pos, 2));
        line(c, 0.0f, bar.y + 0.5f, width, bar.y + 0.5f, rgba(108, 136, 158, 66), 1.0f);
    }

    void drawCompass(SkCanvas& c, float cx, float cy) const {
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
};

class RmlLayerRenderer {
public:
    RmlLayerRenderer() {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr);

        Rml::SetSystemInterface(&system_);
        Rml::SetRenderInterface(&render_);
        Rml::SetFontEngineInterface(&font_);
        rmlReady_ = Rml::Initialise();
        if (rmlReady_) {
            Rml::Factory::RegisterElementInstancer("icon", &iconInstancer_);
            context_ = Rml::CreateContext("rml_layer_desk", {kBaseWidth, kBaseHeight});
            loadDocument();
        }
    }

    ~RmlLayerRenderer() {
        if (rmlReady_) {
            font_.ReleaseFontResources();
            Rml::RemoveContext("rml_layer_desk");
            Rml::Shutdown();
        }
        if (gdiplusToken_ != 0) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
        }
    }

    void draw(uint32_t* pixels, int width, int height, float dpiScale = 1.0f) {
        if (!pixels || width <= 0 || height <= 0) {
            return;
        }

        const float scale = std::max(0.1f, dpiScale);
        const int logicalWidth = std::max(1, static_cast<int>(std::round(static_cast<float>(width) / scale)));
        const int logicalHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(height) / scale)));

        const SkImageInfo chromeInfo = SkImageInfo::Make(width,
                                                         height,
                                                         kBGRA_8888_SkColorType,
                                                         kPremul_SkAlphaType);
        sk_sp<SkSurface> chromeSurface = SkSurfaces::WrapPixels(chromeInfo,
                                                                pixels,
                                                                static_cast<size_t>(width) * sizeof(uint32_t));
        if (chromeSurface) {
            SkCanvas* canvas = chromeSurface->getCanvas();
            canvas->clear(rgb(7, 12, 18));
            canvas->save();
            canvas->scale(scale, scale);
            chrome_.drawApp(*canvas, static_cast<float>(logicalWidth), static_cast<float>(logicalHeight));
            canvas->restore();
        }

        if (context_) {
            context_->SetDimensions({logicalWidth, logicalHeight});
            const int rmlWidth = width * kRmlSupersample;
            const int rmlHeight = height * kRmlSupersample;
            const float rmlRenderScale = scale * static_cast<float>(kRmlSupersample);
            std::vector<uint32_t> rmlPixels(static_cast<size_t>(rmlWidth) * static_cast<size_t>(rmlHeight), 0x00000000u);
            gRmlTextureScale = rmlRenderScale;
            font_.beginFrame(rmlRenderScale);
            render_.begin(rmlPixels.data(), rmlWidth, rmlHeight, rmlRenderScale);
            context_->Update();
            context_->Render();

            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    float srcR = 0.0f;
                    float srcG = 0.0f;
                    float srcB = 0.0f;
                    float srcA = 0.0f;
                    for (int sy = 0; sy < kRmlSupersample; ++sy) {
                        for (int sx = 0; sx < kRmlSupersample; ++sx) {
                            const uint32_t sample = rmlPixels[(static_cast<size_t>(y * kRmlSupersample + sy) *
                                                               static_cast<size_t>(rmlWidth)) +
                                                              static_cast<size_t>(x * kRmlSupersample + sx)];
                            srcA += static_cast<float>((sample >> 24) & 0xff);
                            srcR += static_cast<float>((sample >> 16) & 0xff);
                            srcG += static_cast<float>((sample >> 8) & 0xff);
                            srcB += static_cast<float>(sample & 0xff);
                        }
                    }

                    constexpr float invSampleCount =
                        1.0f / static_cast<float>(kRmlSupersample * kRmlSupersample);
                    srcA *= invSampleCount;
                    if (srcA <= 0.0f) {
                        continue;
                    }
                    srcR *= invSampleCount;
                    srcG *= invSampleCount;
                    srcB *= invSampleCount;

                    uint32_t& dst = pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                    const float invA = 1.0f - std::clamp(srcA, 0.0f, 255.0f) / 255.0f;
                    const float dstR = static_cast<float>((dst >> 16) & 0xff);
                    const float dstG = static_cast<float>((dst >> 8) & 0xff);
                    const float dstB = static_cast<float>(dst & 0xff);
                    dst = 0xff000000u | (static_cast<uint32_t>(clampByte(srcR + dstR * invA)) << 16) |
                          (static_cast<uint32_t>(clampByte(srcG + dstG * invA)) << 8) |
                          static_cast<uint32_t>(clampByte(srcB + dstB * invA));
                }
            }
        }

    }

    bool savePng(const wchar_t* path, int width = kBaseWidth, int height = kBaseHeight, float dpiScale = 1.0f) {
        if (!path || width <= 0 || height <= 0) {
            return false;
        }

        std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff070c12u);
        draw(pixels.data(), width, height, dpiScale);

        Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(pixels.data()));
        CLSID pngClsid{};
        return getEncoderClsid(L"image/png", pngClsid) && bitmap.Save(path, &pngClsid, nullptr) == Gdiplus::Ok;
    }

private:
    ULONG_PTR gdiplusToken_ = 0;
    RmlSystem system_;
    SkiaFontEngine font_;
    SkiaRenderInterface render_;
    SkiaChromeRenderer chrome_;
    Rml::ElementInstancerGeneric<LayerIconElement> iconInstancer_;
    bool rmlReady_ = false;
    Rml::Context* context_ = nullptr;

    static void setupGraphics(Gdiplus::Graphics& g) {
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    }

    static const char* rmlDocument() {
        return R"RML(
<rml>
<head>
<style>
body {
    display: block;
    margin: 0;
    width: 100%;
    height: 100%;
    background-color: transparent;
}
div { display: block; position: absolute; box-sizing: border-box; }
icon { display: block; position: absolute; box-sizing: border-box; background-color: transparent; }
body {
    font-family: "Microsoft YaHei UI";
    color: #eff7fd;
}
.text {
    background-color: transparent;
    color: #edf5fb;
    font-size: 16px;
}
.bold { font-weight: bold; }
.muted { color: #bccfde; }
.cyan { color: #30eadc; }
.title {
    left: 215px; top: 47px; width: 360px; height: 40px;
    font-size: 27px; font-weight: bold; color: #eff7fd;
}
.button_text {
    top: 122px; height: 28px;
    font-size: 21px; font-weight: bold; color: #f0f7fc;
}
.button_primary_text { left: 260px; width: 140px; }
.button_secondary_text { left: 538px; width: 150px; }
.search_text {
    left: 216px; top: 190px; width: 180px; height: 26px;
    font-size: 18px; font-weight: bold; color: #88a6b9;
}
.header {
    top: 247px; height: 24px; font-size: 17px; font-weight: bold; color: #e5eff7;
}
.h_name { left: 224px; width: 80px; }
.h_visible { left: 356px; width: 54px; }
.h_count { left: 423px; width: 70px; }
.h_type { left: 508px; width: 54px; }
.h_crs { left: 596px; width: 76px; }
.row_text {
    height: 24px; font-size: 15.5px; color: #ebf4f9;
}
.row_name { left: 232px; width: 122px; font-weight: bold; }
.row_count { left: 423px; width: 70px; }
.row_type { left: 505px; width: 82px; }
.row_crs { left: 596px; width: 100px; }
.y0 { top: 297px; } .y1 { top: 351px; } .y2 { top: 405px; } .y3 { top: 459px; } .y4 { top: 513px; }
.detail_title {
    left: 214px; top: 586px; width: 145px; height: 36px;
    font-size: 25px; font-weight: bold; color: #eff8fc;
}
.pill_text {
    left: 341px; top: 591px; width: 70px; height: 22px;
    font-size: 14px; color: #44ece0;
}
.info_label { width: 92px; height: 22px; font-size: 16px; color: #c0cfdd; }
.info_value { width: 220px; height: 22px; font-size: 16px; color: #edf5fb; }
.info_l1 { left: 169px; top: 650px; } .info_l2 { left: 169px; top: 686px; }
.info_l3 { left: 169px; top: 722px; } .info_l4 { left: 169px; top: 774px; }
.info_v1 { left: 245px; top: 650px; } .info_v2 { left: 245px; top: 686px; }
.info_v3 { left: 245px; top: 722px; } .info_v4 { left: 245px; top: 774px; width: 250px; }
.info_r1 { left: 451px; top: 650px; } .info_r2 { left: 451px; top: 686px; }
.info_r3 { left: 451px; top: 722px; } .info_r4 { left: 451px; top: 774px; }
.info_rv1 { left: 527px; top: 650px; } .info_rv2 { left: 527px; top: 686px; }
.info_rv3 { left: 527px; top: 722px; width: 175px; } .info_rv4 { left: 527px; top: 774px; width: 250px; }
.info_range_2 { left: 510px; top: 748px; width: 190px; height: 22px; font-size: 16px; color: #edf5fb; }
.status_text { bottom: 14px; height: 28px; font-size: 20px; color: #deecf1; }
.status_layers { left: 89px; width: 90px; }
.status_visible { left: 323px; width: 90px; }
.status_total { left: 549px; width: 180px; }
.nav_text { left: 47px; width: 55px; height: 24px; font-size: 17px; font-weight: bold; color: #dce6f2; }
.nav_active { color: #30eadc; }
.nav_layer { top: 157px; } .nav_edit { top: 260px; } .nav_draw { top: 363px; }
.nav_highlight { top: 466px; } .nav_attr { top: 570px; } .nav_change { top: 675px; } .nav_setting { top: 782px; }
.icon_menu { left: 47px; top: 37px; width: 30px; height: 32px; color: #d5e0ea; }
.icon_nav { left: 43px; width: 42px; height: 42px; color: #dee8f2; }
.icon_nav_active { color: #30eadc; }
.icon_nav_layer { top: 107px; } .icon_nav_edit { top: 209px; } .icon_nav_draw { top: 312px; }
.icon_nav_highlight { top: 415px; } .icon_nav_attr { top: 519px; } .icon_nav_change { top: 624px; } .icon_nav_setting { top: 731px; }
.icon_title { left: 162px; top: 41px; width: 42px; height: 42px; color: #ddfcff; }
.icon_button_upload { left: 226px; top: 118px; width: 30px; height: 30px; color: #f2fcff; }
.icon_button_plus { left: 502px; top: 118px; width: 30px; height: 30px; color: #f1f7fe; }
.icon_search { left: 175px; top: 191px; width: 28px; height: 28px; color: #d5e0ec; }
.row_icon { width: 24px; height: 24px; }
.row_drag { left: 156px; width: 24px; height: 24px; color: #dce8f1; }
.row_geom { left: 196px; width: 24px; height: 24px; color: #d8e2ee; }
.row_eye { left: 358px; width: 24px; height: 24px; color: #30eadc; }
.row_more { left: 688px; width: 24px; height: 24px; color: #e8f1f8; }
.row_selected_icon { color: #3fede1; }
.row_i0 { top: 295px; } .row_i1 { top: 349px; } .row_i2 { top: 403px; } .row_i3 { top: 457px; } .row_i4 { top: 511px; }
.icon_detail { left: 171px; top: 586px; width: 28px; height: 28px; color: #40ece1; }
.status_icon_layer { left: 48px; bottom: 15px; width: 31px; height: 31px; color: #d8e2ee; }
.status_icon_eye { left: 279px; bottom: 15px; width: 29px; height: 29px; color: #d8e2ee; }
.status_icon_chart { left: 512px; bottom: 18px; width: 24px; height: 24px; color: #d8e2ee; }
.compass_icon { right: 45px; bottom: 115px; width: 102px; height: 102px; color: #c5d5e0; }
.compass_label {
    width: 28px; height: 22px;
    font-size: 16px;
    color: #edf5fb;
}
.compass_n { right: 82px; bottom: 223px; font-weight: bold; font-size: 17px; }
.compass_e { right: 12px; bottom: 150px; }
.compass_s { right: 82px; bottom: 78px; }
.compass_w { right: 153px; bottom: 150px; }
</style>
</head>
<body>
<icon class="icon_menu" name="menu"></icon>
<icon class="icon_nav icon_nav_active icon_nav_layer" name="layer-active"></icon>
<icon class="icon_nav icon_nav_edit" name="edit"></icon>
<icon class="icon_nav icon_nav_draw" name="pencil"></icon>
<icon class="icon_nav icon_nav_highlight" name="target"></icon>
<icon class="icon_nav icon_nav_attr" name="doc"></icon>
<icon class="icon_nav icon_nav_change" name="switch"></icon>
<icon class="icon_nav icon_nav_setting" name="gear"></icon>
<icon class="icon_title" name="layer"></icon>
<icon class="icon_button_upload" name="upload"></icon>
<icon class="icon_button_plus" name="plus"></icon>
<icon class="icon_search" name="search"></icon>
<icon class="row_drag row_i0 row_selected_icon" name="drag"></icon>
<icon class="row_geom row_i0 row_selected_icon" name="geom-poly"></icon>
<icon class="row_eye row_i0" name="eye"></icon>
<icon class="row_more row_i0" name="more"></icon>
<icon class="row_drag row_i1" name="drag"></icon>
<icon class="row_geom row_i1" name="geom-line"></icon>
<icon class="row_eye row_i1" name="eye"></icon>
<icon class="row_more row_i1" name="more"></icon>
<icon class="row_drag row_i2" name="drag"></icon>
<icon class="row_geom row_i2" name="geom-point"></icon>
<icon class="row_eye row_i2" name="eye"></icon>
<icon class="row_more row_i2" name="more"></icon>
<icon class="row_drag row_i3" name="drag"></icon>
<icon class="row_geom row_i3" name="geom-river"></icon>
<icon class="row_eye row_i3" name="eye"></icon>
<icon class="row_more row_i3" name="more"></icon>
<icon class="row_drag row_i4" name="drag"></icon>
<icon class="row_geom row_i4" name="geom-box"></icon>
<icon class="row_eye row_i4" name="eye"></icon>
<icon class="row_more row_i4" name="more"></icon>
<icon class="icon_detail" name="geom-poly"></icon>
<icon class="status_icon_layer" name="layer"></icon>
<icon class="status_icon_eye" name="eye"></icon>
<icon class="status_icon_chart" name="chart"></icon>
<div class="text compass_label compass_n">N</div>
<div class="text compass_label compass_e">E</div>
<div class="text compass_label compass_s">S</div>
<div class="text compass_label compass_w">W</div>
<div class="text title">图层 / 图层管理</div>
<div class="text button_text button_primary_text">导入SHP</div>
<div class="text button_text button_secondary_text">新建图层</div>
<div class="text search_text">搜索图层...</div>
<div class="text header h_name">名称</div>
<div class="text header h_visible">可见</div>
<div class="text header h_count">要素数</div>
<div class="text header h_type">类型</div>
<div class="text header h_crs">坐标系</div>
<div class="text row_text row_name y0">地块边界</div>
<div class="text row_text row_count y0">6,523</div>
<div class="text row_text row_type y0">Polygon</div>
<div class="text row_text row_crs y0">EPSG:4326</div>
<div class="text row_text row_name y1">道路中心线</div>
<div class="text row_text row_count y1">2,431</div>
<div class="text row_text row_type y1">LineString</div>
<div class="text row_text row_crs y1">EPSG:4326</div>
<div class="text row_text row_name y2">控制点</div>
<div class="text row_text row_count y2">1,842</div>
<div class="text row_text row_type y2">Point</div>
<div class="text row_text row_crs y2">EPSG:4326</div>
<div class="text row_text row_name y3">河流</div>
<div class="text row_text row_count y3">312</div>
<div class="text row_text row_type y3">LineString</div>
<div class="text row_text row_crs y3">EPSG:4326</div>
<div class="text row_text row_name y4">建筑物</div>
<div class="text row_text row_count y4">164</div>
<div class="text row_text row_type y4">Polygon</div>
<div class="text row_text row_crs y4">EPSG:4326</div>
<div class="text detail_title">地块边界</div>
<div class="text pill_text">Polygon</div>
<div class="text info_label info_l1">图层名称:</div><div class="text info_value info_v1">地块边界</div>
<div class="text info_label info_l2">数据源:</div><div class="text info_value info_v2">parcels.shp</div>
<div class="text info_label info_l3">坐标系:</div><div class="text info_value info_v3">EPSG:4326</div>
<div class="text info_label info_l4">创建时间:</div><div class="text info_value info_v4">2024-05-16 14:32:18</div>
<div class="text info_label info_r1">要素数量:</div><div class="text info_value info_rv1">6,523</div>
<div class="text info_label info_r2">几何类型:</div><div class="text info_value info_rv2">Polygon</div>
<div class="text info_label info_r3">范围:</div><div class="text info_value info_rv3">113.2142,22.4987</div>
<div class="text info_range_2">-114.9876,23.9145</div>
<div class="text info_label info_r4">最后编辑:</div><div class="text info_value info_rv4">2024-05-16 15:47:09</div>
<div class="text status_text status_layers">图层:5</div>
<div class="text status_text status_visible">可见:5</div>
<div class="text status_text status_total">总要素:11,272</div>
<div class="text nav_text nav_active nav_layer">图层</div>
<div class="text nav_text nav_edit">编辑</div>
<div class="text nav_text nav_draw">绘制</div>
<div class="text nav_text nav_highlight">高亮</div>
<div class="text nav_text nav_attr">属性</div>
<div class="text nav_text nav_change">变更</div>
<div class="text nav_text nav_setting">设置</div>
</body>
</rml>
)RML";
    }

    void loadDocument() {
        if (!context_) {
            return;
        }
        Rml::ElementDocument* document = context_->LoadDocumentFromMemory(rmlDocument(), "rml_layer_desk.rml");
        if (document) {
            document->Show();
        }
    }

    void drawBackground(Gdiplus::Graphics& g, int width, int height) {
        Gdiplus::RectF rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        Gdiplus::LinearGradientBrush base(rect, argb(255, 7, 13, 20), argb(255, 21, 34, 49), 22.0f);
        g.FillRectangle(&base, rect);

        Gdiplus::GraphicsPath glowPath;
        glowPath.AddEllipse(200.0f, -430.0f, 760.0f, 760.0f);
        Gdiplus::PathGradientBrush glow(&glowPath);
        glow.SetCenterColor(argb(95, 86, 122, 151));
        std::array<Gdiplus::Color, 1> surround = {argb(0, 7, 13, 20)};
        int count = static_cast<int>(surround.size());
        glow.SetSurroundColors(surround.data(), &count);
        g.FillPath(&glow, &glowPath);

        Gdiplus::GraphicsPath sideGlowPath;
        sideGlowPath.AddEllipse(width * 0.46f, -210.0f, 1120.0f, 830.0f);
        Gdiplus::PathGradientBrush sideGlow(&sideGlowPath);
        sideGlow.SetCenterColor(argb(46, 53, 91, 121));
        count = static_cast<int>(surround.size());
        sideGlow.SetSurroundColors(surround.data(), &count);
        g.FillPath(&sideGlow, &sideGlowPath);
    }

    void drawOverlay(Gdiplus::Graphics& g, int width, int height) {
        Gdiplus::Pen frame(argb(105, 125, 155, 173), 1.0f);
        drawRoundRect(g, {2.0f, 2.0f, static_cast<float>(width) - 4.0f, static_cast<float>(height) - 4.0f}, 10.0f, nullptr, &frame);
    }

    void drawText(Gdiplus::Graphics& g,
                  std::string_view value,
                  float x,
                  float y,
                  float size,
                  Gdiplus::Color color,
                  bool bold = false) {
        if (suppressOverlayText_) {
            return;
        }
        std::wstring wide = utf8ToWide(value);
        Gdiplus::FontFamily family(L"Microsoft YaHei UI");
        Gdiplus::Font font(&family, size, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush brush(color);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoClip);
        g.DrawString(wide.c_str(), static_cast<int>(wide.size()), &font, Gdiplus::PointF(x, y), &format, &brush);
    }

    bool suppressOverlayText_ = true;

    void drawRoundRect(Gdiplus::Graphics& g, const Rect& r, float radius, Gdiplus::Brush* brush, Gdiplus::Pen* pen) {
        Gdiplus::GraphicsPath path;
        const float d = radius * 2.0f;
        path.AddArc(r.x, r.y, d, d, 180.0f, 90.0f);
        path.AddArc(r.x + r.w - d, r.y, d, d, 270.0f, 90.0f);
        path.AddArc(r.x + r.w - d, r.y + r.h - d, d, d, 0.0f, 90.0f);
        path.AddArc(r.x, r.y + r.h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
        if (brush) {
            g.FillPath(brush, &path);
        }
        if (pen) {
            g.DrawPath(pen, &path);
        }
    }

    void line(Gdiplus::Graphics& g, float x1, float y1, float x2, float y2, Gdiplus::Color color, float width = 1.0f) {
        Gdiplus::Pen pen(color, width);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&pen, x1, y1, x2, y2);
    }

    void drawSidebarOverlay(Gdiplus::Graphics& g) {
        for (int i = 0; i < 3; ++i) {
            line(g, 47.0f, 41.0f + i * 12.0f, 77.0f, 41.0f + i * 12.0f, argb(220, 213, 224, 234), 3.2f);
        }

        drawLayerStack(g, 64.0f, 128.0f, 42.0f, argb(255, 48, 234, 220), true);
        drawText(g, "图层", 47.0f, 157.0f, 17.0f, argb(255, 48, 234, 220), true);

        drawEditIcon(g, 64.0f, 230.0f, argb(215, 222, 230, 242));
        drawText(g, "编辑", 47.0f, 260.0f, 17.0f, argb(215, 222, 230, 242), true);
        drawPencilIcon(g, 64.0f, 333.0f, argb(215, 222, 230, 242));
        drawText(g, "绘制", 47.0f, 363.0f, 17.0f, argb(215, 222, 230, 242), true);
        drawTargetIcon(g, 64.0f, 436.0f, argb(215, 222, 230, 242));
        drawText(g, "高亮", 47.0f, 466.0f, 17.0f, argb(215, 222, 230, 242), true);
        drawDocIcon(g, 64.0f, 540.0f, argb(215, 222, 230, 242));
        drawText(g, "属性", 47.0f, 570.0f, 17.0f, argb(215, 222, 230, 242), true);
        drawSwitchIcon(g, 64.0f, 645.0f, argb(215, 222, 230, 242));
        drawText(g, "变更", 47.0f, 675.0f, 17.0f, argb(215, 222, 230, 242), true);
        drawGearIcon(g, 64.0f, 752.0f, argb(215, 222, 230, 242));
        drawText(g, "设置", 47.0f, 782.0f, 17.0f, argb(215, 222, 230, 242), true);
    }

    void drawPanelOverlay(Gdiplus::Graphics& g) {
        drawLayerStack(g, 183.0f, 62.0f, 42.0f, argb(240, 221, 252, 255), false);
        drawText(g, "图层 / 图层管理", 215.0f, 47.0f, 27.0f, argb(245, 239, 247, 253), true);

        drawUploadIcon(g, 241.0f, 133.0f, argb(235, 242, 252, 255));
        drawText(g, "导入SHP", 260.0f, 122.0f, 20.5f, argb(238, 240, 247, 252), true);
        drawPlusIcon(g, 517.0f, 133.0f, argb(230, 241, 247, 254));
        drawText(g, "新建图层", 538.0f, 122.0f, 20.5f, argb(238, 240, 247, 252), true);

        drawSearchIcon(g, 187.0f, 203.0f, argb(220, 213, 224, 236));
        drawText(g, "搜索图层...", 216.0f, 190.0f, 17.5f, argb(132, 180, 195, 207), true);

        drawText(g, "名称", 224.0f, 247.0f, 16.5f, argb(230, 229, 239, 247), true);
        drawText(g, "可见", 356.0f, 247.0f, 16.5f, argb(230, 229, 239, 247), true);
        drawText(g, "要素数", 423.0f, 247.0f, 16.5f, argb(230, 229, 239, 247), true);
        drawText(g, "类型", 508.0f, 247.0f, 16.5f, argb(230, 229, 239, 247), true);
        drawText(g, "坐标系", 596.0f, 247.0f, 16.5f, argb(230, 229, 239, 247), true);

        drawLayerRow(g, 280.0f, true, 0, "地块边界", "6,523", "Polygon");
        drawLayerRow(g, 334.0f, false, 1, "道路中心线", "2,431", "LineString");
        drawLayerRow(g, 388.0f, false, 2, "控制点", "1,842", "Point");
        drawLayerRow(g, 442.0f, false, 3, "河流", "312", "LineString");
        drawLayerRow(g, 496.0f, false, 4, "建筑物", "164", "Polygon");

        drawGeometryIcon(g, 0, 185.0f, 600.0f, argb(255, 64, 236, 225), 28.0f);
        drawText(g, "地块边界", 214.0f, 586.0f, 24.5f, argb(242, 239, 248, 252), true);
        drawText(g, "Polygon", 341.0f, 591.0f, 14.0f, argb(255, 68, 236, 224), false);

        drawInfo(g, 169.0f, 650.0f, "图层名称:", "地块边界");
        drawInfo(g, 169.0f, 686.0f, "数据源:", "parcels.shp");
        drawInfo(g, 169.0f, 722.0f, "坐标系:", "EPSG:4326");
        drawInfo(g, 169.0f, 774.0f, "创建时间:", "2024-05-16 14:32:18");
        drawInfo(g, 451.0f, 650.0f, "要素数量:", "6,523");
        drawInfo(g, 451.0f, 686.0f, "几何类型:", "Polygon");
        drawInfo(g, 451.0f, 722.0f, "范围:", "113.2142,22.4987");
        drawText(g, "-114.9876,23.9145", 510.0f, 748.0f, 16.0f, argb(232, 238, 246, 251));
        drawInfo(g, 451.0f, 774.0f, "最后编辑:", "2024-05-16 15:47:09");
    }

    void drawLayerRow(Gdiplus::Graphics& g,
                      float y,
                      bool selected,
                      int icon,
                      std::string_view name,
                      std::string_view count,
                      std::string_view type) {
        drawDragDots(g, 168.0f, y + 27.0f, argb(selected ? 215 : 165, 220, 232, 241));
        drawGeometryIcon(g, icon, 208.0f, y + 27.0f, selected ? argb(255, 63, 237, 225) : argb(220, 216, 226, 238), 22.0f);
        drawText(g, name, 232.0f, y + 17.0f, 16.5f, argb(238, 239, 246, 251), true);
        drawEyeIcon(g, 370.0f, y + 27.0f, argb(255, 48, 234, 220), 24.0f);
        drawText(g, count, 423.0f, y + 17.0f, 15.8f, argb(232, 235, 244, 249));
        drawText(g, type, 505.0f, y + 17.0f, 15.0f, argb(232, 235, 244, 249));
        drawText(g, "EPSG:4326", 596.0f, y + 17.0f, 15.0f, argb(232, 235, 244, 249));
        drawMoreIcon(g, 700.0f, y + 27.0f, argb(215, 232, 241, 248));
    }

    void drawInfo(Gdiplus::Graphics& g, float x, float y, std::string_view label, std::string_view value) {
        drawText(g, label, x, y, 16.0f, argb(215, 192, 207, 220));
        drawText(g, value, x + 76.0f, y, 16.0f, argb(232, 238, 246, 251));
    }

    void drawStatusOverlay(Gdiplus::Graphics& g, int height) {
        const float y = static_cast<float>(height) - 30.0f;
        drawLayerStack(g, 63.0f, y, 31.0f, argb(210, 216, 226, 238), false);
        drawText(g, "图层:5", 89.0f, y - 13.0f, 20.0f, argb(235, 222, 232, 241));
        drawEyeIcon(g, 293.0f, y, argb(210, 216, 226, 238), 29.0f);
        drawText(g, "可见:5", 323.0f, y - 13.0f, 20.0f, argb(235, 222, 232, 241));
        drawChartIcon(g, 524.0f, y, argb(210, 216, 226, 238));
        drawText(g, "总要素:11,272", 549.0f, y - 13.0f, 20.0f, argb(235, 222, 232, 241));
    }

    void drawLayerStack(Gdiplus::Graphics& g, float cx, float cy, float size, Gdiplus::Color color, bool glow) {
        if (glow) {
            Gdiplus::SolidBrush glowBrush(argb(42, 41, 233, 220));
            g.FillEllipse(&glowBrush, cx - size * 0.55f, cy - size * 0.50f, size * 1.1f, size * 1.1f);
        }
        Gdiplus::Pen pen(color, 2.0f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush brush(color);
        for (int i = 0; i < 3; ++i) {
            const float yy = cy - size * 0.30f + i * size * 0.25f;
            std::array<Gdiplus::PointF, 4> points = {
                Gdiplus::PointF(cx, yy - size * 0.19f),
                Gdiplus::PointF(cx + size * 0.45f, yy + size * 0.01f),
                Gdiplus::PointF(cx, yy + size * 0.21f),
                Gdiplus::PointF(cx - size * 0.45f, yy + size * 0.01f),
            };
            if (i == 0) {
                g.FillPolygon(&brush, points.data(), static_cast<int>(points.size()));
            } else {
                g.DrawPolygon(&pen, points.data(), static_cast<int>(points.size()));
            }
        }
    }

    void drawSearchIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, 2.5f);
        g.DrawEllipse(&pen, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
        line(g, cx + 6.0f, cy + 6.0f, cx + 15.0f, cy + 15.0f, color, 2.5f);
    }

    void drawUploadIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        line(g, cx, cy + 12.0f, cx, cy - 8.0f, color, 2.2f);
        line(g, cx, cy - 8.0f, cx - 7.0f, cy - 1.0f, color, 2.2f);
        line(g, cx, cy - 8.0f, cx + 7.0f, cy - 1.0f, color, 2.2f);
        line(g, cx - 11.0f, cy + 12.0f, cx + 11.0f, cy + 12.0f, color, 2.2f);
        line(g, cx - 11.0f, cy + 12.0f, cx - 11.0f, cy + 5.0f, color, 2.2f);
        line(g, cx + 11.0f, cy + 12.0f, cx + 11.0f, cy + 5.0f, color, 2.2f);
    }

    void drawPlusIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        line(g, cx - 9.0f, cy, cx + 9.0f, cy, color, 2.0f);
        line(g, cx, cy - 9.0f, cx, cy + 9.0f, color, 2.0f);
    }

    void drawDragDots(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::SolidBrush brush(color);
        for (int row = -1; row <= 1; ++row) {
            g.FillEllipse(&brush, cx - 5.9f, cy + row * 6.0f - 1.9f, 3.8f, 3.8f);
            g.FillEllipse(&brush, cx + 2.1f, cy + row * 6.0f - 1.9f, 3.8f, 3.8f);
        }
    }

    void drawGeometryIcon(Gdiplus::Graphics& g, int icon, float cx, float cy, Gdiplus::Color color, float size) {
        Gdiplus::Pen pen(color, 2.1f);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        switch (icon) {
        case 0: {
            std::array<Gdiplus::PointF, 5> points = {
                Gdiplus::PointF(cx, cy - size * 0.55f),
                Gdiplus::PointF(cx + size * 0.48f, cy - size * 0.20f),
                Gdiplus::PointF(cx + size * 0.35f, cy + size * 0.52f),
                Gdiplus::PointF(cx - size * 0.42f, cy + size * 0.48f),
                Gdiplus::PointF(cx - size * 0.55f, cy - size * 0.12f),
            };
            g.DrawPolygon(&pen, points.data(), static_cast<int>(points.size()));
            break;
        }
        case 1:
            line(g, cx - size * 0.46f, cy - size * 0.32f, cx - size * 0.05f, cy + size * 0.02f, color, 2.2f);
            line(g, cx - size * 0.05f, cy + size * 0.02f, cx + size * 0.46f, cy + size * 0.42f, color, 2.2f);
            break;
        case 2:
            g.DrawEllipse(&pen, cx - size * 0.31f, cy - size * 0.31f, size * 0.62f, size * 0.62f);
            break;
        case 3:
            drawRiver(g, cx, cy, size, color);
            break;
        default:
            g.DrawRectangle(&pen, cx - size * 0.38f, cy - size * 0.38f, size * 0.76f, size * 0.76f);
            break;
        }
    }

    void drawRiver(Gdiplus::Graphics& g, float cx, float cy, float size, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, 2.1f);
        Gdiplus::GraphicsPath p;
        p.AddBezier(cx - size * 0.50f,
                    cy - size * 0.12f,
                    cx - size * 0.22f,
                    cy + size * 0.12f,
                    cx - size * 0.08f,
                    cy + size * 0.12f,
                    cx + size * 0.18f,
                    cy - size * 0.08f);
        p.AddBezier(cx + size * 0.18f,
                    cy - size * 0.08f,
                    cx + size * 0.36f,
                    cy - size * 0.22f,
                    cx + size * 0.52f,
                    cy - size * 0.15f,
                    cx + size * 0.64f,
                    cy - size * 0.04f);
        g.DrawPath(&pen, &p);
        Gdiplus::GraphicsPath q;
        q.AddBezier(cx - size * 0.54f,
                    cy + size * 0.20f,
                    cx - size * 0.26f,
                    cy + size * 0.44f,
                    cx - size * 0.06f,
                    cy + size * 0.44f,
                    cx + size * 0.22f,
                    cy + size * 0.22f);
        q.AddBezier(cx + size * 0.22f,
                    cy + size * 0.22f,
                    cx + size * 0.38f,
                    cy + size * 0.09f,
                    cx + size * 0.52f,
                    cy + size * 0.14f,
                    cx + size * 0.66f,
                    cy + size * 0.26f);
        g.DrawPath(&pen, &q);
    }

    void drawEyeIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color, float size) {
        Gdiplus::Pen pen(color, 2.0f);
        Gdiplus::GraphicsPath p;
        p.AddBezier(cx - size * 0.44f, cy, cx - size * 0.22f, cy - size * 0.30f, cx + size * 0.22f, cy - size * 0.30f, cx + size * 0.44f, cy);
        p.AddBezier(cx + size * 0.44f, cy, cx + size * 0.22f, cy + size * 0.30f, cx - size * 0.22f, cy + size * 0.30f, cx - size * 0.44f, cy);
        g.DrawPath(&pen, &p);
        Gdiplus::SolidBrush brush(color);
        g.FillEllipse(&brush, cx - size * 0.12f, cy - size * 0.12f, size * 0.24f, size * 0.24f);
    }

    void drawMoreIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::SolidBrush brush(color);
        g.FillEllipse(&brush, cx - 2.0f, cy - 11.0f, 4.0f, 4.0f);
        g.FillEllipse(&brush, cx - 2.0f, cy - 2.0f, 4.0f, 4.0f);
        g.FillEllipse(&brush, cx - 2.0f, cy + 7.0f, 4.0f, 4.0f);
    }

    void drawChartIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, 2.0f);
        g.DrawRectangle(&pen, cx - 11.0f, cy - 11.0f, 22.0f, 22.0f);
        line(g, cx - 5.0f, cy + 6.0f, cx - 5.0f, cy + 1.0f, color, 3.0f);
        line(g, cx, cy + 6.0f, cx, cy - 5.0f, color, 3.0f);
        line(g, cx + 5.0f, cy + 6.0f, cx + 5.0f, cy - 1.0f, color, 3.0f);
    }

    void drawEditIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, 2.6f);
        g.DrawRectangle(&pen, cx - 14.0f, cy - 11.0f, 22.0f, 22.0f);
        line(g, cx - 3.0f, cy + 7.0f, cx + 15.0f, cy - 11.0f, color, 3.0f);
        line(g, cx + 10.0f, cy - 16.0f, cx + 17.0f, cy - 9.0f, color, 3.0f);
    }

    void drawPencilIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        line(g, cx - 13.0f, cy + 14.0f, cx + 14.0f, cy - 13.0f, color, 4.0f);
        line(g, cx + 7.0f, cy - 16.0f, cx + 17.0f, cy - 6.0f, color, 3.2f);
        line(g, cx - 16.0f, cy + 17.0f, cx - 6.0f, cy + 14.0f, color, 2.0f);
    }

    void drawTargetIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, 2.8f);
        g.DrawEllipse(&pen, cx - 16.0f, cy - 16.0f, 32.0f, 32.0f);
        Gdiplus::Pen inner(color, 2.2f);
        g.DrawEllipse(&inner, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
        Gdiplus::SolidBrush brush(color);
        g.FillEllipse(&brush, cx - 2.5f, cy - 2.5f, 5.0f, 5.0f);
    }

    void drawDocIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, 2.5f);
        g.DrawRectangle(&pen, cx - 13.0f, cy - 16.0f, 26.0f, 32.0f);
        line(g, cx - 6.0f, cy - 6.0f, cx + 7.0f, cy - 6.0f, color, 1.9f);
        line(g, cx - 6.0f, cy + 1.0f, cx + 7.0f, cy + 1.0f, color, 1.9f);
        line(g, cx - 6.0f, cy + 8.0f, cx + 7.0f, cy + 8.0f, color, 1.9f);
    }

    void drawSwitchIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        line(g, cx - 15.0f, cy - 8.0f, cx + 13.0f, cy - 8.0f, color, 2.5f);
        line(g, cx + 8.0f, cy - 14.0f, cx + 15.0f, cy - 8.0f, color, 2.5f);
        line(g, cx + 8.0f, cy - 2.0f, cx + 15.0f, cy - 8.0f, color, 2.5f);
        line(g, cx + 15.0f, cy + 8.0f, cx - 13.0f, cy + 8.0f, color, 2.5f);
        line(g, cx - 8.0f, cy + 2.0f, cx - 15.0f, cy + 8.0f, color, 2.5f);
        line(g, cx - 8.0f, cy + 14.0f, cx - 15.0f, cy + 8.0f, color, 2.5f);
    }

    void drawGearIcon(Gdiplus::Graphics& g, float cx, float cy, Gdiplus::Color color) {
        Gdiplus::SolidBrush brush(color);
        Gdiplus::GraphicsPath gear;
        constexpr int teeth = 8;
        for (int i = 0; i < teeth * 2; ++i) {
            const float r = (i % 2 == 0) ? 17.0f : 12.5f;
            const float a = -kPi * 0.5f + i * kPi / teeth;
            const float x = cx + std::cos(a) * r;
            const float y = cy + std::sin(a) * r;
            if (i == 0) {
                gear.StartFigure();
            }
            gear.AddLine(i == 0 ? x : cx + std::cos(-kPi * 0.5f + (i - 1) * kPi / teeth) * ((i - 1) % 2 == 0 ? 17.0f : 12.5f),
                         i == 0 ? y : cy + std::sin(-kPi * 0.5f + (i - 1) * kPi / teeth) * ((i - 1) % 2 == 0 ? 17.0f : 12.5f),
                         x,
                         y);
        }
        gear.CloseFigure();
        g.FillPath(&brush, &gear);
        Gdiplus::Pen cut(argb(255, 10, 18, 26), 2.0f);
        g.DrawEllipse(&cut, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
    }

    void drawCompass(Gdiplus::Graphics& g, float cx, float cy) {
        const float r = 51.0f;
        Gdiplus::Pen ring(argb(145, 197, 213, 224), 1.1f);
        g.DrawEllipse(&ring, cx - r, cy - r, r * 2.0f, r * 2.0f);
        Gdiplus::Pen tick(argb(110, 207, 220, 230), 1.0f);
        for (int i = 0; i < 24; ++i) {
            const float angle = -kPi * 0.5f + i * (2.0f * kPi / 24.0f);
            const float len = (i % 6 == 0) ? 11.0f : 6.0f;
            g.DrawLine(&tick,
                       cx + std::cos(angle) * (r - len),
                       cy + std::sin(angle) * (r - len),
                       cx + std::cos(angle) * r,
                       cy + std::sin(angle) * r);
        }
        drawText(g, "N", cx - 8.0f, cy - r - 34.0f, 17.0f, argb(235, 237, 245, 251), true);
        drawText(g, "E", cx + r + 10.0f, cy - 12.0f, 16.0f, argb(220, 237, 245, 251));
        drawText(g, "S", cx - 7.0f, cy + r + 8.0f, 16.0f, argb(220, 237, 245, 251));
        drawText(g, "W", cx - r - 28.0f, cy - 12.0f, 16.0f, argb(220, 237, 245, 251));

        Gdiplus::SolidBrush red(argb(255, 227, 74, 82));
        std::array<Gdiplus::PointF, 4> north = {
            Gdiplus::PointF(cx, cy - r + 12.0f),
            Gdiplus::PointF(cx + 9.0f, cy - 1.0f),
            Gdiplus::PointF(cx, cy - 8.0f),
            Gdiplus::PointF(cx - 9.0f, cy - 1.0f),
        };
        g.FillPolygon(&red, north.data(), static_cast<int>(north.size()));
        Gdiplus::SolidBrush white(argb(230, 224, 232, 239));
        std::array<Gdiplus::PointF, 4> south = {
            Gdiplus::PointF(cx, cy + r - 13.0f),
            Gdiplus::PointF(cx + 9.0f, cy + 1.0f),
            Gdiplus::PointF(cx, cy + 8.0f),
            Gdiplus::PointF(cx - 9.0f, cy + 1.0f),
        };
        g.FillPolygon(&white, south.data(), static_cast<int>(south.size()));
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
        wc.lpszClassName = L"RmlLayerDeskWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&wc);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        int clientWidth = static_cast<int>(std::round(kBaseWidth * dpiScale));
        int clientHeight = static_cast<int>(std::round(kBaseHeight * dpiScale));
        const float fit = std::min(1.0f, std::min((workWidth - 40.0f) / clientWidth, (workHeight - 40.0f) / clientHeight));
        clientWidth = static_cast<int>(std::round(clientWidth * std::max(0.70f, fit)));
        clientHeight = static_cast<int>(std::round(clientHeight * std::max(0.70f, fit)));
        const int x = workArea.left + (workWidth - clientWidth) / 2;
        const int y = workArea.top + (workHeight - clientHeight) / 2;

        HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW,
                                    wc.lpszClassName,
                                    L"RmlLayerDesk",
                                    WS_POPUP,
                                    x,
                                    y,
                                    clientWidth,
                                    clientHeight,
                                    nullptr,
                                    nullptr,
                                    instance,
                                    this);
        if (!hwnd) {
            return 1;
        }

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        ShowWindow(hwnd, showCmd == SW_HIDE ? SW_SHOWNORMAL : showCmd);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    RmlLayerRenderer renderer_;
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
        case WM_LBUTTONDOWN:
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
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
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void resize(int width, int height) {
        surfaceWidth_ = std::max(1, width);
        surfaceHeight_ = std::max(1, height);
        pixels_.assign(static_cast<size_t>(surfaceWidth_) * static_cast<size_t>(surfaceHeight_), 0xff070c12u);
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        if (width != surfaceWidth_ || height != surfaceHeight_ || pixels_.empty()) {
            resize(width, height);
        }
        dpi_ = getWindowDpi(hwnd);
        renderer_.draw(pixels_.data(), surfaceWidth_, surfaceHeight_, dpiScaleForDpi(dpi_));

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
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i + 4 < argc; ++i) {
            if (wcscmp(argv[i], L"--dump-size") == 0) {
                const int width = std::max(1, _wtoi(argv[i + 1]));
                const int height = std::max(1, _wtoi(argv[i + 2]));
                const float dpiScale = std::max(0.1f, static_cast<float>(_wtof(argv[i + 3])));
                RmlLayerRenderer renderer;
                const bool ok = renderer.savePng(argv[i + 4], width, height, dpiScale);
                LocalFree(argv);
                return ok ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
        for (int i = 1; i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--dump") == 0) {
                RmlLayerRenderer renderer;
                const bool ok = renderer.savePng(argv[i + 1]);
                LocalFree(argv);
                return ok ? EXIT_SUCCESS : EXIT_FAILURE;
            }
        }
        LocalFree(argv);
    }

    AppWindow app;
    return app.run(instance, showCmd);
}
