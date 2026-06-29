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
#include <utility>
#include <vector>

namespace {

constexpr SkColor kDemoClearColor = SkColorSetRGB(248, 250, 252);
constexpr std::string_view kDevices[] = {
    "device-alex",
    "device-desktop",
    "device-laptop",
    "device-server",
    "device-mark",
};
constexpr std::string_view kTabs[] = {
    "tab-chat",
    "tab-history",
    "tab-transfer",
};

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
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

std::string defaultDocumentPath() {
    std::filesystem::path working = std::filesystem::current_path() / "assets" / "skui_relay_demo" / "relaydesk.html";
    if (std::filesystem::exists(working)) {
        return working.string();
    }

    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) {
        return "assets/skui_relay_demo/relaydesk.html";
    }
    std::filesystem::path exe = modulePath;
    std::filesystem::path local = exe.parent_path() / "assets" / "skui_relay_demo" / "relaydesk.html";
    if (std::filesystem::exists(local)) {
        return local.string();
    }
    return (std::filesystem::current_path() / "assets" / "skui_relay_demo" / "relaydesk.html").string();
}

void selectDevice(skui::Runtime& runtime, std::string_view id) {
    for (std::string_view device : kDevices) {
        runtime.removeClassById(device, "device-selected");
    }
    runtime.addClassById(id, "device-selected");
}

void selectTab(skui::Runtime& runtime, std::string_view id) {
    for (std::string_view tab : kTabs) {
        runtime.removeClassById(tab, "tab-active");
    }
    runtime.addClassById(id, "tab-active");
}

void installDemoInteractions(skui::Runtime& runtime) {
    runtime.setElementEventCallback([&runtime](const skui::ElementEvent& event) {
        if (event.type != skui::ElementEventType::Click || event.action.empty()) {
            return;
        }

        constexpr std::string_view devicePrefix = "select-device:";
        constexpr std::string_view tabPrefix = "tab:";
        const std::string_view action(event.action);
        if (action.size() >= devicePrefix.size() && action.substr(0, devicePrefix.size()) == devicePrefix) {
            selectDevice(runtime, action.substr(devicePrefix.size()));
        } else if (action.size() >= tabPrefix.size() && action.substr(0, tabPrefix.size()) == tabPrefix) {
            selectTab(runtime, action.substr(tabPrefix.size()));
        } else if (action == "send-message") {
            runtime.setAttributeById("composer", "value", "");
            runtime.setAttributeById("composer", "placeholder", "消息已发送，可以继续输入...");
        } else if (action == "finish-transfer") {
            runtime.setAttributeById("upload-progress", "value", "100");
            runtime.setAttributeById("download-progress", "value", "100");
            runtime.setAttributeById("upload-percent", "class", "transfer-percent done");
            runtime.setAttributeById("download-percent", "class", "transfer-percent done");
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
        runtimeOptions.assetRoot = "assets/skui_relay_demo";
        runtimeOptions.clearColor = kDemoClearColor;
        skui::Runtime runtime(runtimeOptions);
        installDemoInteractions(runtime);
        const std::string documentPath = defaultDocumentPath();
        if (!runtime.loadDocument(documentPath)) {
            std::cerr << "load html failed: " << runtime.lastError() << "\n";
            result = 2;
        } else {
            runtime.resize(width, height, dpiScale);
            std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height),
                                         SkColorSetARGB(255, 248, 250, 252));
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
        const int width = argc >= 4 ? parseIntArg(argv[3], 1600) : 1600;
        const int height = argc >= 5 ? parseIntArg(argv[4], 900) : 900;
        const float dpiScale = argc >= 6 ? parseFloatArg(argv[5], 1.0f) : 1.0f;
        const int result = exportDocument(argv[2], width, height, dpiScale);
        LocalFree(argv);
        return result;
    }
    if (argv) {
        LocalFree(argv);
    }

    skui::win32::WindowOptions options;
    options.title = L"RelayDesk";
    options.logicalWidth = 1600;
    options.logicalHeight = 900;
    options.clearColor = colorRefFromSkColor(kDemoClearColor);
    options.documentPath = defaultDocumentPath();
    options.runtime.assetRoot = "assets/skui_relay_demo";
    options.runtime.clearColor = kDemoClearColor;
    options.onRuntimeReady = [](skui::Runtime& runtime) {
        installDemoInteractions(runtime);
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
