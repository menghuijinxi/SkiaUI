#include "skui_runtime.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr int kWidth = 140;
constexpr int kHeight = 90;

uint32_t pixelAt(const std::vector<uint32_t>& pixels, int x, int y) {
    return pixels[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)];
}

bool renderPixel(skui::Runtime& runtime, int x, int y, uint32_t& out) {
    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight, 0);
    if (!runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight, static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.0f)) {
        std::cerr << "render failed: " << runtime.lastError() << "\n";
        return false;
    }
    out = pixelAt(pixels, x, y);
    return true;
}

void sendMouse(skui::Runtime& runtime, skui::EventType type, float x, float y) {
    skui::Event event;
    event.type = type;
    event.x = x;
    event.y = y;
    if (type == skui::EventType::MouseDown || type == skui::EventType::MouseUp) {
        event.button = skui::MouseButton::Left;
    }
    runtime.handleEvent(event);
}

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    constexpr std::string_view html = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .tile {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 80px;
      height: 40px;
      background-color: #112233;
    }
    .tile:hover {
      background-color: #334455;
    }
    .tile:active {
      background-color: #556677;
    }
    .selected {
      background-color: #778899;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="tile" class="tile" data-action="tile-click"></div>
  </div>
</body>
</html>
)html";

    skui::RuntimeOptions options;
    options.clearColor = SK_ColorBLACK;
    skui::Runtime runtime(options);
    runtime.resize(kWidth, kHeight, 1.0f);

    int clickCount = 0;
    std::string lastAction;
    runtime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Click) {
            ++clickCount;
            lastAction = event.action;
            runtime.addClassById(event.id, "selected");
        }
    });

    if (!runtime.loadDocumentFromString(html, "")) {
        std::cerr << "load failed: " << runtime.lastError() << "\n";
        return 1;
    }

    bool ok = true;
    uint32_t normal = 0;
    uint32_t hover = 0;
    uint32_t active = 0;
    uint32_t selected = 0;

    ok = renderPixel(runtime, 20, 20, normal) && ok;
    sendMouse(runtime, skui::EventType::MouseMove, 20.0f, 20.0f);
    ok = renderPixel(runtime, 20, 20, hover) && ok;
    sendMouse(runtime, skui::EventType::MouseDown, 20.0f, 20.0f);
    ok = renderPixel(runtime, 20, 20, active) && ok;
    sendMouse(runtime, skui::EventType::MouseUp, 20.0f, 20.0f);
    ok = renderPixel(runtime, 20, 20, selected) && ok;

    ok = expect(normal != hover, "div:hover should change rendered output") && ok;
    ok = expect(hover != active, "div:active should change rendered output") && ok;
    ok = expect(clickCount == 1, "click should be emitted for data-action div") && ok;
    ok = expect(lastAction == "tile-click", "click action should be routed from data-action") && ok;
    ok = expect(runtime.hasClassById("tile", "selected"), "click callback should be able to mutate classes") && ok;
    ok = expect(selected != normal, "class mutation after click should repaint") && ok;

    return ok ? 0 : 1;
}
