#pragma once

#include "include/core/SkColor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class SkCanvas;

namespace skui {

struct Theme {
    SkColor text = SkColorSetARGB(255, 239, 247, 253);
    SkColor mutedText = SkColorSetARGB(210, 192, 207, 220);
    SkColor accent = SkColorSetARGB(255, 48, 234, 220);

    static Theme dark();
};

struct RuntimeOptions {
    std::string assetRoot;
    float scale = 1.0f;
    SkColor clearColor = SkColorSetRGB(7, 12, 18);
    Theme theme = Theme::dark();
};

enum class EventType {
    None,
    MouseMove,
    MouseDown,
    MouseUp,
    MouseWheel,
    KeyDown,
    KeyUp,
    TextInput
};

enum class MouseButton {
    None,
    Left,
    Middle,
    Right
};

struct Event {
    EventType type = EventType::None;
    float x = 0.0f;
    float y = 0.0f;
    float wheelDelta = 0.0f;
    MouseButton button = MouseButton::None;
    unsigned key = 0;
    std::string text;
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
    bool loadDocumentFromString(std::string_view html, std::string_view basePath = {});
    void resize(int width, int height, float dpiScale);
    bool handleEvent(const Event& event);
    void render(SkCanvas& canvas);
    bool renderToBgraPixels(uint32_t* pixels, int width, int height, size_t rowBytes, float dpiScale);

    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    [[nodiscard]] float dpiScale() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] std::string lastError() const;
    void clearDirty();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace skui
