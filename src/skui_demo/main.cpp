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
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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
constexpr std::string_view kAttrRows[] = {
    "attr-row-1",
    "attr-row-2",
    "attr-row-3",
    "attr-row-4",
    "attr-row-5",
    "attr-row-6",
    "attr-row-7",
    "attr-row-8",
    "attr-row-9",
    "attr-row-10",
    "attr-row-11",
    "attr-row-12",
    "attr-row-13",
    "attr-row-14",
    "attr-row-15",
    "attr-row-16",
    "attr-row-17",
    "attr-row-18",
};
constexpr std::string_view kAttrCols[] = {
    "id",
    "name",
    "landuse",
    "height",
    "type",
    "owner",
    "area",
    "updated",
    "status",
    "note",
};
constexpr std::string_view kPropertyLayers[] = {
    "地块边界.shp",
    "道路中心线.shp",
    "控制点.shp",
    "建筑物.shp",
};
constexpr int kPropertyPanelLeft = 134;
constexpr int kPropertyPanelMinWidth = 500;
constexpr int kPropertyPanelDefaultWidth = 610;
constexpr int kPropertyContentLeft = 160;
constexpr int kAttrRowGutterWidth = 34;
constexpr int kPropertyContentInset = 26;
constexpr int kPropertyRightGap = 8;

struct PropertyDemoState {
    bool draggingPanel = false;
    bool attributesVisible = true;
    bool layerDropdownOpen = false;
    int panelWidth = kPropertyPanelDefaultWidth;
    float dragStartX = 0.0f;
    int dragStartWidth = kPropertyPanelDefaultWidth;
    std::string selectedRow = "attr-row-2";
    std::string selectedCell = "attr-cell-2-landuse";
    std::string sortCol;
    bool sortAsc = true;
    int layerIndex = 0;
};

PropertyDemoState gPropertyState;

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

std::vector<std::string_view> splitActionPayload(std::string_view payload, char delimiter) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= payload.size()) {
        const size_t end = payload.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.push_back(payload.substr(start));
            break;
        }
        parts.push_back(payload.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string px(float value) {
    std::ostringstream out;
    out << static_cast<int>(std::lround(value)) << "px";
    return out.str();
}

std::string style(std::initializer_list<std::pair<std::string_view, std::string_view>> declarations) {
    std::string out;
    for (const auto& [name, value] : declarations) {
        out += std::string(name);
        out += ":";
        out += value;
        out += ";";
    }
    return out;
}

int attrColumnBaseLeft(std::string_view col) {
    return col == "id" ? 0 :
           col == "name" ? 80 :
           col == "landuse" ? 200 :
           col == "height" ? 350 :
           col == "type" ? 460 :
           col == "owner" ? 580 :
           col == "area" ? 740 :
           col == "updated" ? 860 :
           col == "status" ? 1020 : 1140;
}

int attrColumnLeft(std::string_view col) {
    return kAttrRowGutterWidth + attrColumnBaseLeft(col);
}

int attrColumnWidth(std::string_view col) {
    return col == "id" ? 80 :
           col == "name" ? 120 :
           col == "landuse" ? 150 :
           col == "height" ? 110 :
           col == "type" ? 120 :
           col == "owner" ? 160 :
           col == "area" ? 120 :
           col == "updated" ? 160 :
           col == "status" ? 120 : 220;
}

void showPage(skui::Runtime& runtime, std::string_view page) {
    if (page == "properties") {
        runtime.addClassById("layer-page", "page-hidden");
        runtime.removeClassById("properties-page", "page-hidden");
    } else {
        runtime.removeClassById("layer-page", "page-hidden");
        runtime.addClassById("properties-page", "page-hidden");
    }
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
    showPage(runtime, navName == "properties" ? "properties" : "layers");
}

void setPropertyPanelWidth(skui::Runtime& runtime, int width) {
    const int maxWidth = std::max(kPropertyPanelMinWidth, runtime.width() - kPropertyPanelLeft - kPropertyRightGap);
    gPropertyState.panelWidth = std::clamp(width, kPropertyPanelMinWidth, maxWidth);
    const int contentWidth = std::max(320, gPropertyState.panelWidth - kPropertyContentInset * 2);
    runtime.setStylesById({
        {"property-panel", style({{"width", px(static_cast<float>(gPropertyState.panelWidth))},
                                  {"height", "825px"}})},
        {"property-search", style({{"width", px(static_cast<float>(contentWidth))}})},
        {"property-toolbar", style({{"width", px(static_cast<float>(contentWidth + 8))}})},
        {"property-table-viewport", style({{"width", px(static_cast<float>(contentWidth))}})},
        {"selected-cell-card", style({{"width", px(static_cast<float>(contentWidth))}})},
    });
}

void clearAttrRows(skui::Runtime& runtime) {
    for (std::string_view row : kAttrRows) {
        std::string rowBg = std::string(row) + "-bg";
        runtime.removeClassById(rowBg, "row-selected-highlight");
    }
}

void clearAttrCells(skui::Runtime& runtime) {
    for (std::string_view row : kAttrRows) {
        for (std::string_view col : kAttrCols) {
            std::string cell = std::string(row);
            cell.replace(0, 8, "attr-cell");
            cell += "-";
            cell += col;
            runtime.removeClassById(cell, "cell-selected-highlight");
        }
    }
}

void clearSelectedCellDetails(skui::Runtime& runtime) {
    runtime.setTextById("selected-cell-value", "");
    runtime.setTextById("selected-cell-type", "");
}

void selectAttrRow(skui::Runtime& runtime, std::string_view rowId) {
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", "");
    gPropertyState.selectedCell.clear();
    gPropertyState.selectedRow = std::string(rowId);
    clearSelectedCellDetails(runtime);
    runtime.addClassById(std::string(rowId) + "-bg", "row-selected-highlight");
}

void selectAttrCol(skui::Runtime& runtime, std::string_view colId) {
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    gPropertyState.selectedRow.clear();
    gPropertyState.selectedCell.clear();
    clearSelectedCellDetails(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", colId);
}

void selectAttrCell(skui::Runtime& runtime, std::string_view cellId, std::string_view value, std::string_view type) {
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", "");
    gPropertyState.selectedRow.clear();
    gPropertyState.selectedCell = std::string(cellId);
    runtime.addClassById(cellId, "cell-selected-highlight");
    runtime.setTextById("selected-cell-value", value);
    runtime.setTextById("selected-cell-type", type);
}

void sortAttributes(skui::Runtime& runtime, std::string_view colId) {
    const bool sameColumn = gPropertyState.sortCol == colId;
    gPropertyState.sortAsc = sameColumn ? !gPropertyState.sortAsc : true;
    gPropertyState.sortCol = std::string(colId);
    for (std::string_view col : kAttrCols) {
        runtime.setTextById("sort-" + std::string(col), "-");
    }
    runtime.setTextById("sort-" + std::string(colId), gPropertyState.sortAsc ? "↑" : "↓");

    for (size_t i = 0; i < std::size(kAttrRows); ++i) {
        const size_t source = gPropertyState.sortAsc ? i : std::size(kAttrRows) - 1 - i;
        const int top = 40 + static_cast<int>(i) * 36;
        const std::string row = std::string(kAttrRows[source]);
        runtime.setStyleById(row + "-bg", style({{"top", px(static_cast<float>(top))}}));
        runtime.setStyleById(row + "-handle", style({{"top", px(static_cast<float>(top))}}));
        for (std::string_view col : kAttrCols) {
            std::string cell = row;
            cell.replace(0, 8, "attr-cell");
            cell += "-";
            cell += col;
            const int left = attrColumnLeft(col);
            const int width = attrColumnWidth(col);
            runtime.setStyleById(cell, style({{"left", px(static_cast<float>(left))},
                                              {"top", px(static_cast<float>(top))},
                                              {"width", px(static_cast<float>(width))}}));
        }
    }
}

void setLayerDropdownOpen(skui::Runtime& runtime, bool open) {
    gPropertyState.layerDropdownOpen = open;
    runtime.setTextById("property-layer-arrow", open ? "^" : "v");
    if (open) {
        runtime.removeClassById("property-layer-backdrop", "page-hidden");
        runtime.removeClassById("property-layer-menu", "page-hidden");
    } else {
        runtime.addClassById("property-layer-backdrop", "page-hidden");
        runtime.addClassById("property-layer-menu", "page-hidden");
    }
}

void selectAttributeLayer(skui::Runtime& runtime, int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kPropertyLayers))) {
        return;
    }
    gPropertyState.layerIndex = index;
    runtime.setTextById("property-layer-name", kPropertyLayers[static_cast<size_t>(index)]);
    for (int i = 0; i < static_cast<int>(std::size(kPropertyLayers)); ++i) {
        runtime.removeClassById("property-layer-option-" + std::to_string(i), "property-layer-option-selected");
    }
    runtime.addClassById("property-layer-option-" + std::to_string(index), "property-layer-option-selected");
    setLayerDropdownOpen(runtime, false);
}

void handlePropertyAction(skui::Runtime& runtime, const skui::ElementEvent& event, std::string_view action) {
    if (action == "resize-properties") {
        if (event.type == skui::ElementEventType::MouseDown) {
            gPropertyState.draggingPanel = true;
            gPropertyState.dragStartX = event.x;
            gPropertyState.dragStartWidth = gPropertyState.panelWidth;
        } else if (event.type == skui::ElementEventType::MouseMove && gPropertyState.draggingPanel) {
            const int nextWidth = gPropertyState.dragStartWidth + static_cast<int>(std::lround(event.x - gPropertyState.dragStartX));
            setPropertyPanelWidth(runtime, nextWidth);
        } else if (event.type == skui::ElementEventType::MouseUp) {
            gPropertyState.draggingPanel = false;
        }
        return;
    }

    if (event.type != skui::ElementEventType::Click) {
        return;
    }

    constexpr std::string_view rowPrefix = "select-attr-row:";
    constexpr std::string_view colPrefix = "select-attr-col:";
    constexpr std::string_view cellPrefix = "select-attr-cell:";
    constexpr std::string_view sortPrefix = "sort-attr:";
    constexpr std::string_view layerPrefix = "select-attr-layer:";
    if (action == "attr-toggle-visibility") {
        gPropertyState.attributesVisible = !gPropertyState.attributesVisible;
        if (gPropertyState.attributesVisible) {
            runtime.removeClassById("attr-visibility-button", "visibility-hidden");
        } else {
            runtime.addClassById("attr-visibility-button", "visibility-hidden");
        }
    } else if (action == "choose-attr-layer") {
        setLayerDropdownOpen(runtime, !gPropertyState.layerDropdownOpen);
    } else if (action == "close-attr-layer-menu") {
        setLayerDropdownOpen(runtime, false);
    } else if (action.rfind(layerPrefix, 0) == 0) {
        const std::string_view value = action.substr(layerPrefix.size());
        int index = -1;
        std::from_chars(value.data(), value.data() + value.size(), index);
        selectAttributeLayer(runtime, index);
    } else if (action.rfind(rowPrefix, 0) == 0) {
        selectAttrRow(runtime, action.substr(rowPrefix.size()));
    } else if (action.rfind(colPrefix, 0) == 0) {
        selectAttrCol(runtime, action.substr(colPrefix.size()));
    } else if (action.rfind(cellPrefix, 0) == 0) {
        const std::vector<std::string_view> parts = splitActionPayload(action.substr(cellPrefix.size()), '|');
        if (parts.size() >= 3) {
            selectAttrCell(runtime, parts[0], parts[1], parts[2]);
        }
    } else if (action.rfind(sortPrefix, 0) == 0) {
        sortAttributes(runtime, action.substr(sortPrefix.size()));
    }
}

void installDemoInteractions(skui::Runtime& runtime) {
    runtime.setElementEventCallback([&runtime](const skui::ElementEvent& event) {
        if (event.action.empty()) {
            return;
        }

        constexpr std::string_view layerPrefix = "select-layer:";
        constexpr std::string_view navPrefix = "nav-";
        const std::string_view action(event.action);
        handlePropertyAction(runtime, event, action);
        if (event.type != skui::ElementEventType::Click) {
            return;
        }

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
    } else if (state == L"properties") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
    } else if (state == L"properties-dropdown") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        clickAt(runtime, 360.0f, 122.0f, dpiScale);
    } else if (state == L"properties-layer-roads") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        clickAt(runtime, 360.0f, 122.0f, dpiScale);
        clickAt(runtime, 360.0f, 190.0f, dpiScale);
    } else if (state == L"properties-col") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        clickAt(runtime, 250.0f, 316.0f, dpiScale);
    } else if (state == L"properties-row") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        clickAt(runtime, 176.0f, 426.0f, dpiScale);
    } else if (state == L"properties-sort") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        clickAt(runtime, 333.0f, 316.0f, dpiScale);
    } else if (state == L"properties-scroll") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        skui::Event wheel;
        wheel.type = skui::EventType::MouseWheel;
        wheel.x = 460.0f * dpiScale;
        wheel.y = 430.0f * dpiScale;
        wheel.wheelDelta = -720.0f;
        runtime.handleEvent(wheel);
        wheel.shiftKey = true;
        runtime.handleEvent(wheel);
    } else if (state == L"properties-wide") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        sendMouse(runtime, skui::EventType::MouseDown, 744.0f, 430.0f, dpiScale);
        sendMouse(runtime, skui::EventType::MouseMove, 960.0f, 430.0f, dpiScale);
        sendMouse(runtime, skui::EventType::MouseUp, 960.0f, 430.0f, dpiScale);
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
