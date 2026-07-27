#include "skui_runtime.h"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

bool expect(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool sendEnter(skui::Runtime& runtime, bool shiftKey)
{
    skui::Event event;
    event.type = skui::EventType::KeyDown;
    event.key = 0x0D;
    event.shiftKey = shiftKey;
    return runtime.handleEvent(event);
}

}  // namespace

int main()
{
    constexpr std::string_view kDocument = R"html(
<!doctype html>
<html>
<body>
  <div id="editor" contenteditable="true">
    <p id="paragraph">hello</p>
  </div>
</body>
</html>
)html";

    skui::Runtime runtime;
    runtime.resize(240, 120, 1.0f);

    int callbackCount = 0;
    bool handledEnterObserved = false;
    bool shiftEnterObserved = false;
    runtime.setElementKeyDownCallback(
        [&](const skui::ElementEvent& event) {
            ++callbackCount;
            if (event.id != "editor" || event.key != 0x0D) {
                return false;
            }
            if (event.shiftKey) {
                shiftEnterObserved = true;
                return false;
            }
            handledEnterObserved = true;
            return true;
        });

    if (!runtime.loadDocumentFromString(kDocument, "")) {
        std::cerr << "document load failed: " << runtime.lastError() << '\n';
        return 1;
    }

    bool ok = expect(runtime.collapseSelection("paragraph", 5),
                     "contenteditable paragraph should accept focus");
    const std::optional<skui::LayoutRect> caretAtEnd =
        runtime.editingCaretRect();
    ok = expect(caretAtEnd.has_value() && caretAtEnd->width > 0.0f &&
                    caretAtEnd->height > 0.0f,
                "focused contenteditable should expose its caret rectangle") &&
         ok;
    ok = expect(runtime.collapseSelection("paragraph", 0),
                "contenteditable should move the caret to the start") &&
         ok;
    const std::optional<skui::LayoutRect> caretAtStart =
        runtime.editingCaretRect();
    ok = expect(caretAtStart.has_value() && caretAtEnd.has_value() &&
                    caretAtStart->x < caretAtEnd->x,
                "editing caret rectangle should follow the text cursor") &&
         ok;
    ok = expect(runtime.collapseSelection("paragraph", 5),
                "contenteditable should restore the caret before Enter") &&
         ok;
    ok = expect(sendEnter(runtime, false),
                "handled Enter should be consumed") &&
         ok;
    ok = expect(handledEnterObserved && callbackCount == 1,
                "Enter should reach the editing host callback") &&
         ok;
    ok = expect(runtime.childElementIdsById("editor").size() == 1 &&
                    runtime.textContentById("paragraph") ==
                        std::optional<std::string>("hello"),
                "handled Enter should prevent the default paragraph split") &&
         ok;

    ok = expect(sendEnter(runtime, true),
                "Shift+Enter default editing should be consumed") &&
         ok;
    ok = expect(shiftEnterObserved && callbackCount == 2,
                "Shift state should reach the editing host callback") &&
         ok;
    ok = expect(runtime.childElementIdsById("editor").size() == 2 &&
                    runtime.textContentById("paragraph") ==
                        std::optional<std::string>("hello"),
                "unhandled Shift+Enter should preserve default paragraph splitting") &&
         ok;

    return ok ? 0 : 1;
}
