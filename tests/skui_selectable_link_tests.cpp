#include "skui_runtime.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
    }
    return condition;
}

bool sendMouse(skui::Runtime& runtime, skui::EventType type, float x, float y) {
    skui::Event event;
    event.type = type;
    event.x = x;
    event.y = y;
    if (type == skui::EventType::MouseDown ||
        type == skui::EventType::MouseUp ||
        type == skui::EventType::MouseDoubleClick) {
        event.button = skui::MouseButton::Left;
    }
    return runtime.handleEvent(event);
}

void sendKey(skui::Runtime& runtime, unsigned key, bool ctrl) {
    skui::Event event;
    event.type = skui::EventType::KeyDown;
    event.key = key;
    event.ctrlKey = ctrl;
    runtime.handleEvent(event);
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
      width: 180px;
      height: 120px;
      background-color: #000000;
    }
    selectable {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 150px;
      height: 70px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable value="first&#10;open link&#10;last" data-links="6:15:open-url:https://example.com"></selectable>
  </div>
</body>
</html>
)html";

    std::string clipboard;
    std::string action;
    skui::RuntimeOptions options;
    options.clearColor = SK_ColorBLACK;
    options.writeClipboardText = [&](std::string_view text) {
        clipboard = std::string(text);
    };
    options.onElementEvent = [&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Click) {
            action = event.action;
        }
    };

    skui::Runtime runtime(options);
    runtime.resize(180, 120, 1.0f);

    bool ok = true;
    if (!runtime.loadDocumentFromString(html, "")) {
        std::cerr << "load failed: " << runtime.lastError() << "\n";
        return 1;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 12.0f, 20.0f);
    sendMouse(runtime, skui::EventType::MouseMove, 160.0f, 58.0f);
    sendMouse(runtime, skui::EventType::MouseUp, 160.0f, 58.0f);
    sendKey(runtime, 'C', true);
    if (clipboard != "first\nopen link\nlast") {
        std::cerr << "clipboard=[" << clipboard << "]\n";
    }
    ok = expect(clipboard == "first\nopen link\nlast",
                "link ranges should preserve multiline selectable copy") && ok;

    clipboard.clear();
    action.clear();
    sendMouse(runtime, skui::EventType::MouseDown, 22.0f, 33.0f);
    sendMouse(runtime, skui::EventType::MouseUp, 22.0f, 33.0f);
    if (action != "open-url:https://example.com") {
        std::cerr << "action=[" << action << "]\n";
    }
    ok = expect(action == "open-url:https://example.com",
                "clicking a link range should emit its action") && ok;
    sendKey(runtime, 'C', true);
    ok = expect(clipboard.empty(), "clicking a link should not leave selected text") && ok;

    constexpr std::string_view mixedLinkHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 260px;
      height: 180px;
      background-color: #000000;
    }
    selectable {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 170px;
      height: 150px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable value="prefix https://igoutu.cn/icon/lchz7JPUz9qU/%E8%AE%BE%E7%BD%AE suffix" data-links="7:61:open-url:https://igoutu.cn/icon/lchz7JPUz9qU/%E8%AE%BE%E7%BD%AE"></selectable>
  </div>
</body>
</html>
)html";

    clipboard.clear();
    action.clear();
    skui::Runtime mixedLinkRuntime(options);
    mixedLinkRuntime.resize(260, 120, 1.0f);
    if (!mixedLinkRuntime.loadDocumentFromString(mixedLinkHtml, "")) {
        std::cerr << "mixed link load failed: " << mixedLinkRuntime.lastError() << "\n";
        return 1;
    }

    sendMouse(mixedLinkRuntime, skui::EventType::MouseDown, 12.0f, 20.0f);
    sendMouse(mixedLinkRuntime, skui::EventType::MouseMove, 175.0f, 145.0f);
    sendMouse(mixedLinkRuntime, skui::EventType::MouseUp, 175.0f, 145.0f);
    sendKey(mixedLinkRuntime, 'C', true);
    if (clipboard != "prefix https://igoutu.cn/icon/lchz7JPUz9qU/%E8%AE%BE%E7%BD%AE suffix") {
        std::cerr << "mixed clipboard=[" << clipboard << "]\n";
    }
    ok = expect(clipboard == "prefix https://igoutu.cn/icon/lchz7JPUz9qU/%E8%AE%BE%E7%BD%AE suffix",
                "mixed text and links should remain one selectable value") && ok;

    clipboard.clear();
    action.clear();
    sendMouse(mixedLinkRuntime, skui::EventType::MouseDown, 70.0f, 20.0f);
    sendMouse(mixedLinkRuntime, skui::EventType::MouseUp, 70.0f, 20.0f);
    ok = expect(action == "open-url:https://igoutu.cn/icon/lchz7JPUz9qU/%E8%AE%BE%E7%BD%AE",
                "clicking a mixed inline link should emit its action") && ok;

    return ok ? 0 : 1;
}
