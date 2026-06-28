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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr SkColor kDemoClearColor = SkColorSetRGB(7, 12, 18);
constexpr std::string_view kLayerRows[] = {
    "layer-row-parcels",
    "layer-row-roads",
    "layer-row-control",
    "layer-row-rivers",
    "layer-row-buildings",
};
constexpr std::string_view kNavItems[] = {
    "nav-item-layers",
    "nav-item-edit",
    "nav-item-draw",
    "nav-item-highlight",
    "nav-item-properties",
    "nav-item-change",
    "nav-item-settings",
};
constexpr std::string_view kNavPositions[] = {
    "nav-pos-layers",
    "nav-pos-edit",
    "nav-pos-draw",
    "nav-pos-highlight",
    "nav-pos-properties",
    "nav-pos-change",
    "nav-pos-settings",
};

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::string defaultDocumentPath() {
    std::filesystem::path working = std::filesystem::current_path() / "assets" / "skui_demo" / "layers.html";
    if (std::filesystem::exists(working)) {
        return working.string();
    }

    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) {
        return "assets/skui_demo/layers.html";
    }
    std::filesystem::path exe = modulePath;
    std::filesystem::path local = exe.parent_path() / "assets" / "skui_demo" / "layers.html";
    if (std::filesystem::exists(local)) {
        return local.string();
    }
    return (std::filesystem::current_path() / "assets" / "skui_demo" / "layers.html").string();
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
                                             static_cast<UINT>(static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t)),
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

float parseFloatArg(const wchar_t* value, float fallback) {
    if (!value) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value, &end);
    return end && *end == L'\0' ? parsed : fallback;
}

int parseIntArg(const wchar_t* value, int fallback) {
    if (!value) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value, &end, 10);
    return end && *end == L'\0' ? static_cast<int>(parsed) : fallback;
}

void selectLayerRow(skui::Runtime& runtime, std::string_view rowId) {
    for (std::string_view id : kLayerRows) {
        runtime.removeClassById(id, "row-selected");
    }
    runtime.addClassById(rowId, "row-selected");
}

void selectNavItem(skui::Runtime& runtime, std::string_view navName) {
    const std::string itemClass = "nav-item-" + std::string(navName);
    const std::string positionClass = "nav-pos-" + std::string(navName);
    for (std::string_view id : kNavItems) {
        runtime.removeClassById(id, "nav-item-active");
    }
    for (std::string_view position : kNavPositions) {
        runtime.removeClassById("nav-active-bg", position);
        runtime.removeClassById("nav-active-line", position);
    }
    runtime.addClassById(itemClass, "nav-item-active");
    runtime.addClassById("nav-active-bg", positionClass);
    runtime.addClassById("nav-active-line", positionClass);
}

void installDemoInteractions(skui::Runtime& runtime) {
    runtime.setElementEventCallback([&runtime](const skui::ElementEvent& event) {
        if (event.type != skui::ElementEventType::Click || event.action.empty()) {
            return;
        }

        constexpr std::string_view layerPrefix = "select-layer:";
        constexpr std::string_view navPrefix = "nav-";
        const std::string_view action(event.action);
        if (action.size() >= layerPrefix.size() && action.substr(0, layerPrefix.size()) == layerPrefix) {
            selectLayerRow(runtime, action.substr(layerPrefix.size()));
        } else if (action.size() >= navPrefix.size() && action.substr(0, navPrefix.size()) == navPrefix) {
            selectNavItem(runtime, action.substr(navPrefix.size()));
        }

        std::string message = "SkiaUiDesk click: " + event.action + "\n";
        OutputDebugStringA(message.c_str());
    });
}

void sendMouse(skui::Runtime& runtime, skui::EventType type, float x, float y, float dpiScale) {
    skui::Event event;
    event.type = type;
    event.x = x * dpiScale;
    event.y = y * dpiScale;
    if (type == skui::EventType::MouseDown || type == skui::EventType::MouseUp) {
        event.button = skui::MouseButton::Left;
    }
    runtime.handleEvent(event);
}

void clickAt(skui::Runtime& runtime, float x, float y, float dpiScale) {
    sendMouse(runtime, skui::EventType::MouseDown, x, y, dpiScale);
    sendMouse(runtime, skui::EventType::MouseUp, x, y, dpiScale);
}

void applyExportState(skui::Runtime& runtime, const std::wstring& state, float dpiScale) {
    if (state.empty() || state == L"default") {
        return;
    }

    if (state == L"hover" || state == L"button-hover") {
        sendMouse(runtime, skui::EventType::MouseMove, 260.0f, 133.0f, dpiScale);
    } else if (state == L"active" || state == L"button-active") {
        sendMouse(runtime, skui::EventType::MouseDown, 260.0f, 133.0f, dpiScale);
    } else if (state == L"nav-hover") {
        sendMouse(runtime, skui::EventType::MouseMove, 63.0f, 248.0f, dpiScale);
    } else if (state == L"nav-active") {
        sendMouse(runtime, skui::EventType::MouseDown, 63.0f, 248.0f, dpiScale);
    } else if (state == L"nav-click-edit") {
        clickAt(runtime, 63.0f, 248.0f, dpiScale);
    } else if (state == L"row-hover") {
        sendMouse(runtime, skui::EventType::MouseMove, 260.0f, 362.0f, dpiScale);
    } else if (state == L"row-active") {
        sendMouse(runtime, skui::EventType::MouseDown, 260.0f, 362.0f, dpiScale);
    } else if (state == L"row-click-rivers") {
        clickAt(runtime, 260.0f, 466.0f, dpiScale);
    }
}

int exportDocument(const wchar_t* outputPath, int width, int height, float dpiScale, const std::wstring& state) {
    width = std::max(1, width);
    height = std::max(1, height);
    dpiScale = std::max(0.1f, dpiScale);

    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(initResult);

    int result = 0;
    {
        skui::RuntimeOptions runtimeOptions;
        runtimeOptions.assetRoot = "assets/skui_demo";
        runtimeOptions.clearColor = kDemoClearColor;
        skui::Runtime runtime(runtimeOptions);
        installDemoInteractions(runtime);
        const std::string documentPath = defaultDocumentPath();
        if (!runtime.loadDocument(documentPath)) {
            std::cerr << "load html failed: " << runtime.lastError() << "\n";
            result = 2;
        } else {
            runtime.resize(width, height, dpiScale);
            applyExportState(runtime, state, dpiScale);
            std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0xff070c12u);
            if (!runtime.renderToBgraPixels(pixels.data(), width, height, static_cast<size_t>(width) * sizeof(uint32_t), dpiScale)) {
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
        const int width = argc >= 4 ? parseIntArg(argv[3], 1672) : 1672;
        const int height = argc >= 5 ? parseIntArg(argv[4], 941) : 941;
        const float dpiScale = argc >= 6 ? parseFloatArg(argv[5], 1.0f) : 1.0f;
        const std::wstring state = argc >= 7 ? argv[6] : L"default";
        const int result = exportDocument(argv[2], width, height, dpiScale, state);
        LocalFree(argv);
        return result;
    }
    if (argv) {
        LocalFree(argv);
    }

    skui::win32::WindowOptions options;
    options.title = L"SkiaUiDesk";
    options.logicalWidth = 1672;
    options.logicalHeight = 941;
    options.clearColor = colorRefFromSkColor(kDemoClearColor);
    options.documentPath = defaultDocumentPath();
    options.runtime.assetRoot = "assets/skui_demo";
    options.runtime.clearColor = kDemoClearColor;
    options.onRuntimeReady = [](skui::Runtime& runtime) {
        installDemoInteractions(runtime);
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
