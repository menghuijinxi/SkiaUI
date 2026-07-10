#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_win32_app.h"

#include "include/core/SkColor.h"

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr SkColor kClearColor = SkColorSetRGB(244, 247, 251);

struct DemoState {
    bool noticeVisible = true;
    bool messageDisplayHidden = false;
    bool messageVisibilityHidden = false;
    bool motionEnabled = false;
    bool fadeDimmed = false;
    int nextMessageId = 9;
    std::vector<int> messageIds = {1, 2, 3, 4, 5, 6, 7, 8};
};

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int requiredLength = MultiByteToWideChar(CP_UTF8,
                                                    MB_ERR_INVALID_CHARS,
                                                    text.data(),
                                                    static_cast<int>(text.size()),
                                                    nullptr,
                                                    0);
    if (requiredLength <= 0) {
        return {};
    }

    std::wstring wideText(static_cast<std::size_t>(requiredLength), L'\0');
    const int convertedLength = MultiByteToWideChar(CP_UTF8,
                                                     MB_ERR_INVALID_CHARS,
                                                     text.data(),
                                                     static_cast<int>(text.size()),
                                                     wideText.data(),
                                                     requiredLength);
    if (convertedLength != requiredLength) {
        return {};
    }
    return wideText;
}

void openUrl(std::string_view url) {
    const std::wstring wideUrl = utf8ToWide(url);
    if (wideUrl.empty()) {
        return;
    }
    ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string defaultDocumentPath() {
    const std::filesystem::path working =
        std::filesystem::current_path() / "assets" / "skui_dynamic_dom_demo" / "dynamic_dom.html";
    if (std::filesystem::exists(working)) {
        return working.string();
    }

    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) {
        return "assets/skui_dynamic_dom_demo/dynamic_dom.html";
    }
    const std::filesystem::path exe = modulePath;
    const std::filesystem::path local =
        exe.parent_path() / "assets" / "skui_dynamic_dom_demo" / "dynamic_dom.html";
    if (std::filesystem::exists(local)) {
        return local.string();
    }
    return working.string();
}

bool writePngBgra(const wchar_t* path, const uint32_t* pixels, int width, int height) {
    if (!path || !pixels || width <= 0 || height <= 0) {
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;

    bool ok = false;
    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comReady = SUCCEEDED(initResult) || initResult == RPC_E_CHANGED_MODE;
    const bool shouldUninitializeCom = SUCCEEDED(initResult);
    if (comReady) {
        const HRESULT factoryResult = CoCreateInstance(CLSID_WICImagingFactory,
                                                       nullptr,
                                                       CLSCTX_INPROC_SERVER,
                                                       IID_PPV_ARGS(&factory));
        if (SUCCEEDED(factoryResult) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) &&
            SUCCEEDED(frame->Initialize(properties)) &&
            SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)))) {
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (SUCCEEDED(frame->SetPixelFormat(&format)) &&
                IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
                SUCCEEDED(frame->WritePixels(static_cast<UINT>(height),
                                             static_cast<UINT>(width * sizeof(uint32_t)),
                                             static_cast<UINT>(static_cast<size_t>(width) *
                                                               static_cast<size_t>(height) *
                                                               sizeof(uint32_t)),
                                             reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels)))) &&
                SUCCEEDED(frame->Commit()) &&
                SUCCEEDED(encoder->Commit())) {
                ok = true;
            }
        }
    }

    if (properties) {
        properties->Release();
    }
    if (frame) {
        frame->Release();
    }
    if (encoder) {
        encoder->Release();
    }
    if (stream) {
        stream->Release();
    }
    if (factory) {
        factory->Release();
    }
    if (shouldUninitializeCom) {
        CoUninitialize();
    }
    return ok;
}

int parseIntArg(const wchar_t* value, int fallback) {
    if (!value) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value, &end, 10);
    return end && *end == L'\0' ? static_cast<int>(parsed) : fallback;
}

float parseFloatArg(const wchar_t* value, float fallback) {
    if (!value) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value, &end);
    return end && *end == L'\0' ? parsed : fallback;
}

bool hasMessage(const DemoState& state, int id) {
    return std::find(state.messageIds.begin(), state.messageIds.end(), id) != state.messageIds.end();
}

std::string messageElementHtml(int id) {
    std::ostringstream html;
    html << "<div id=\"message-" << id << "\" class=\"message\">";
    html << "<selectable class=\"message-name\">Message " << id << "</selectable>";
    html << "<selectable class=\"message-text\">Added at runtime with appendHtmlById.</selectable>";
    html << "</div>";
    return html.str();
}

std::string replacementHtml(int id) {
    std::ostringstream html;
    html << "<div id=\"message-" << id << "\" class=\"message message-replaced\">";
    html << "<selectable class=\"message-name\">Message " << id << " replaced</selectable>";
    html << "<selectable class=\"message-text\">This node was recreated with replaceHtmlById.</selectable>";
    html << "</div>";
    return html.str();
}

void refreshDemoStatus(skui::Runtime& runtime, const DemoState& state) {
    runtime.setTextById("message-count", "Messages: " + std::to_string(state.messageIds.size()));
    std::string status = "Message 2 display:";
    status += state.messageDisplayHidden ? " none" : " flex";
    status += ". Message 3 visibility:";
    status += state.messageVisibilityHidden ? " hidden." : " visible.";
    status += state.motionEnabled ? " Motion on," : " Motion off,";
    status += state.fadeDimmed ? " fade dimmed." : " fade visible.";
    runtime.setTextById("demo-status", status);
}

void updateMultilineSample(skui::Runtime& runtime) {
    runtime.setValueById("multiline-sample",
                         "First selectable line\n"
                         "Second line stays in the same node\n"
                         "Third line should copy with line breaks");
}

void updateMotionCard(skui::Runtime& runtime, const DemoState& state) {
    std::string style = "transition: transform 600ms ease, opacity 600ms ease;";
    if (state.motionEnabled) {
        style += "transform: translateX(48px) rotate(8deg) scale(1.08);";
    } else {
        style += "transform: none;";
    }
    style += state.fadeDimmed ? "opacity:0.25;" : "opacity:1;";
    runtime.setStyleById("motion-card", style);
}

void addMessage(skui::Runtime& runtime, DemoState& state) {
    const int id = state.nextMessageId++;
    if (runtime.appendHtmlById("message-list", messageElementHtml(id))) {
        state.messageIds.push_back(id);
        refreshDemoStatus(runtime, state);
    }
}

void removeLatestMessage(skui::Runtime& runtime, DemoState& state) {
    if (state.messageIds.empty()) {
        return;
    }

    const int id = state.messageIds.back();
    if (runtime.removeElementById("message-" + std::to_string(id))) {
        state.messageIds.pop_back();
        refreshDemoStatus(runtime, state);
    }
}

void replaceFirstMessage(skui::Runtime& runtime, const DemoState& state) {
    if (state.messageIds.empty()) {
        return;
    }

    const int id = state.messageIds.front();
    runtime.replaceHtmlById("message-" + std::to_string(id), replacementHtml(id));
}

void toggleNotice(skui::Runtime& runtime, DemoState& state) {
    state.noticeVisible = !state.noticeVisible;
    runtime.setVisibleById("notice-card", state.noticeVisible);
}

void toggleDisplayHidden(skui::Runtime& runtime, DemoState& state) {
    if (!hasMessage(state, 2)) {
        return;
    }
    state.messageDisplayHidden = !state.messageDisplayHidden;
    runtime.setVisibleById("message-2", !state.messageDisplayHidden);
    refreshDemoStatus(runtime, state);
}

void toggleVisibilityHidden(skui::Runtime& runtime, DemoState& state) {
    if (!hasMessage(state, 3)) {
        return;
    }
    state.messageVisibilityHidden = !state.messageVisibilityHidden;
    runtime.setStyleById("message-3", state.messageVisibilityHidden ? "visibility:hidden;" : "");
    refreshDemoStatus(runtime, state);
}

void toggleMotion(skui::Runtime& runtime, DemoState& state) {
    state.motionEnabled = !state.motionEnabled;
    updateMotionCard(runtime, state);
    refreshDemoStatus(runtime, state);
}

void toggleFade(skui::Runtime& runtime, DemoState& state) {
    state.fadeDimmed = !state.fadeDimmed;
    updateMotionCard(runtime, state);
    refreshDemoStatus(runtime, state);
}

void installInteractions(skui::Runtime& runtime, DemoState& state) {
    runtime.setElementEventCallback([&runtime, &state](const skui::ElementEvent& event) {
        if (event.type != skui::ElementEventType::Click || event.action.empty()) {
            return;
        }

        constexpr std::string_view kOpenUrlPrefix = "open-url:";
        if (event.action.starts_with(kOpenUrlPrefix)) {
            openUrl(std::string_view(event.action).substr(kOpenUrlPrefix.size()));
        } else if (event.action == "toggle-notice") {
            toggleNotice(runtime, state);
        } else if (event.action == "toggle-display") {
            toggleDisplayHidden(runtime, state);
        } else if (event.action == "toggle-visibility") {
            toggleVisibilityHidden(runtime, state);
        } else if (event.action == "add-message") {
            addMessage(runtime, state);
        } else if (event.action == "replace-message") {
            replaceFirstMessage(runtime, state);
        } else if (event.action == "remove-message") {
            removeLatestMessage(runtime, state);
        } else if (event.action == "toggle-motion") {
            toggleMotion(runtime, state);
        } else if (event.action == "toggle-fade") {
            toggleFade(runtime, state);
        }
    });
}

int exportDocument(const wchar_t* outputPath, int width, int height, float dpiScale) {
    width = std::max(1, width);
    height = std::max(1, height);
    dpiScale = std::max(0.1f, dpiScale);

    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(initResult);

    int result = 0;
    {
        skui::RuntimeOptions runtimeOptions;
        runtimeOptions.assetRoot = "assets/skui_dynamic_dom_demo";
        runtimeOptions.clearColor = kClearColor;
        skui::Runtime runtime(runtimeOptions);
        DemoState state;
        installInteractions(runtime, state);

        const std::string documentPath = defaultDocumentPath();
        if (!runtime.loadDocument(documentPath)) {
            std::cerr << "load html failed: " << runtime.lastError() << "\n";
            result = 2;
        } else {
            updateMultilineSample(runtime);
            runtime.resize(width, height, dpiScale);
            addMessage(runtime, state);
            toggleNotice(runtime, state);
            toggleDisplayHidden(runtime, state);
            toggleVisibilityHidden(runtime, state);
            replaceFirstMessage(runtime, state);
            toggleMotion(runtime, state);
            toggleFade(runtime, state);

            std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height),
                                         kClearColor);
            if (!runtime.renderToBgraPixels(pixels.data(),
                                            width,
                                            height,
                                            static_cast<size_t>(width) * sizeof(uint32_t),
                                            dpiScale)) {
                std::cerr << "render failed: " << runtime.lastError() << "\n";
                result = 3;
            } else if (!writePngBgra(outputPath, pixels.data(), width, height)) {
                std::cerr << "write png failed\n";
                result = 4;
            }
        }
    }

    if (shouldUninitializeCom) {
        CoUninitialize();
    }
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && std::wstring(argv[1]) == L"--export") {
        const int width = argc >= 4 ? parseIntArg(argv[3], 1180) : 1180;
        const int height = argc >= 5 ? parseIntArg(argv[4], 720) : 720;
        const float dpiScale = argc >= 6 ? parseFloatArg(argv[5], 1.0f) : 1.0f;
        const int result = exportDocument(argv[2], width, height, dpiScale);
        LocalFree(argv);
        return result;
    }
    if (argv) {
        LocalFree(argv);
    }

    DemoState state;
    skui::win32::WindowOptions options;
    options.title = L"SkiaDynamicDomDemo";
    options.logicalWidth = 1180;
    options.logicalHeight = 720;
    options.clearColor = colorRefFromSkColor(kClearColor);
    options.runtime.assetRoot = "assets/skui_dynamic_dom_demo";
    options.runtime.clearColor = kClearColor;
    options.onRuntimeReady = [&state](skui::Runtime& runtime) {
        installInteractions(runtime, state);
        if (!runtime.loadDocument(defaultDocumentPath())) {
            std::cerr << "load html failed: " << runtime.lastError() << "\n";
            return;
        }
        updateMultilineSample(runtime);
        refreshDemoStatus(runtime, state);
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
