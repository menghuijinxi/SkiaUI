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
#include <iomanip>
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
constexpr int kAttrPoolRowCount = 18;
constexpr int kAttrTotalRows = 100000;
constexpr int kAttrHeaderHeight = 40;
constexpr int kAttrRowHeight = 36;
constexpr int kAttrVirtualHeight = kAttrHeaderHeight + kAttrTotalRows * kAttrRowHeight;
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
constexpr int kPropertyTableBaseTop = 258;
constexpr int kPropertyToolbarButtonHeight = 39;
constexpr int kPropertyToolbarGap = 8;
constexpr int kPropertyToolbarBaseHeight = 42;
constexpr int kPropertySelectedCardOffset = 322;
constexpr int kPropertySelectedTitleOffset = 14;
constexpr int kPropertySelectedValueOffset = 64;
constexpr int kPropertySelectedTypeOffset = 110;

struct PropertyDemoState {
    bool draggingPanel = false;
    bool attributesVisible = true;
    bool layerDropdownOpen = false;
    int panelWidth = kPropertyPanelDefaultWidth;
    float dragStartX = 0.0f;
    int dragStartWidth = kPropertyPanelDefaultWidth;
    float attrScrollY = 0.0f;
    int firstAttrRow = 0;
    int selectedAttrRow = -1;
    int selectedCellRow = 1;
    std::string selectedCellCol = "landuse";
    std::string sortCol;
    bool sortAsc = true;
    int layerIndex = 0;
};

PropertyDemoState gPropertyState;

void refreshAttrWindow(skui::Runtime& runtime, float scrollY);

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

std::string rowPoolId(int poolIndex) {
    return "attr-row-" + std::to_string(poolIndex + 1);
}

std::string cellPoolId(int poolIndex, std::string_view col) {
    return "attr-cell-" + std::to_string(poolIndex + 1) + "-" + std::string(col);
}

int rowIndexForPool(int poolIndex) {
    return std::clamp(gPropertyState.firstAttrRow + poolIndex, 0, kAttrTotalRows - 1);
}

std::string attrCellValue(int rowIndex, std::string_view col) {
    if (col == "id") {
        return std::to_string(1001 + rowIndex);
    }
    if (col == "name") {
        static constexpr std::string_view names[] = {"地块", "道路", "绿地", "学校", "河流", "仓储", "医院", "码头"};
        return std::string(names[static_cast<size_t>(rowIndex) % std::size(names)]) + std::to_string(rowIndex + 1);
    }
    if (col == "landuse") {
        static constexpr std::string_view uses[] = {"residential", "commercial", "park", "education", "road", "water", "warehouse", "medical"};
        return std::string(uses[static_cast<size_t>(rowIndex) % std::size(uses)]);
    }
    if (col == "height") {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << (static_cast<float>((rowIndex * 37) % 240) / 10.0f);
        return out.str();
    }
    if (col == "type") {
        return rowIndex % 5 == 0 ? "line" : "polygon";
    }
    if (col == "owner") {
        static constexpr std::string_view owners[] = {"城市更新部", "招商服务中心", "公园管理处", "教育局", "交通局", "水务局", "物流园区", "港航中心"};
        return std::string(owners[static_cast<size_t>(rowIndex) % std::size(owners)]);
    }
    if (col == "area") {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << (280.0f + static_cast<float>((rowIndex * 913) % 90000) / 10.0f);
        return out.str();
    }
    if (col == "updated") {
        std::ostringstream out;
        out << "2024-05-" << std::setw(2) << std::setfill('0') << (rowIndex % 28 + 1);
        return out.str();
    }
    if (col == "status") {
        static constexpr std::string_view statuses[] = {"有效", "待核验", "草稿", "冻结"};
        return std::string(statuses[static_cast<size_t>(rowIndex) % std::size(statuses)]);
    }
    static constexpr std::string_view notes[] = {"重点巡检", "数据同步", "规划调整", "外业复核", "批量导入", "边界校准"};
    return std::string(notes[static_cast<size_t>(rowIndex) % std::size(notes)]);
}

std::string attrCellType(std::string_view col) {
    return col == "id" || col == "height" || col == "area" ? "Number" :
           col == "updated" ? "Date" : "Text";
}

int propertyToolbarHeight(int width) {
    constexpr int toolWidths[] = {86, 86, 74, 74, 84, 42, 42};
    int rows = 1;
    int lineWidth = 0;
    for (const int toolWidth : toolWidths) {
        const int itemWidth = toolWidth + kPropertyToolbarGap;
        if (lineWidth > 0 && lineWidth + itemWidth > width) {
            ++rows;
            lineWidth = 0;
        }
        lineWidth += itemWidth;
    }
    return rows * kPropertyToolbarButtonHeight + (rows - 1) * kPropertyToolbarGap;
}

std::string cellActionFor(int poolIndex, int rowIndex, std::string_view col) {
    return "select-attr-cell:" + cellPoolId(poolIndex, col) + "|" +
           std::to_string(rowIndex) + "|" + std::string(col) + "|" +
           attrCellValue(rowIndex, col) + "|" + attrCellType(col);
}

void showPage(skui::Runtime& runtime, std::string_view page) {
    if (page == "properties") {
        runtime.addClassById("layer-page", "page-hidden");
        runtime.removeClassById("properties-page", "page-hidden");
        refreshAttrWindow(runtime, gPropertyState.attrScrollY);
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
    const int toolbarWidth = contentWidth + 8;
    const int toolbarHeight = propertyToolbarHeight(toolbarWidth);
    const int tableTop = kPropertyTableBaseTop + std::max(0, toolbarHeight - kPropertyToolbarBaseHeight);
    const int selectedCardTop = tableTop + kPropertySelectedCardOffset;
    runtime.setStylesById({
        {"property-panel", style({{"width", px(static_cast<float>(gPropertyState.panelWidth))},
                                  {"height", "825px"}})},
        {"property-search", style({{"width", px(static_cast<float>(contentWidth))}})},
        {"property-toolbar", style({{"width", px(static_cast<float>(toolbarWidth))},
                                    {"height", px(static_cast<float>(toolbarHeight))}})},
        {"property-table-viewport", style({{"top", px(static_cast<float>(tableTop))},
                                           {"width", px(static_cast<float>(contentWidth))}})},
        {"selected-cell-card", style({{"top", px(static_cast<float>(selectedCardTop))},
                                      {"width", px(static_cast<float>(contentWidth))}})},
        {"selected-cell-title", style({{"top", px(static_cast<float>(selectedCardTop + kPropertySelectedTitleOffset))}})},
        {"selected-cell-value-row", style({{"top", px(static_cast<float>(selectedCardTop + kPropertySelectedValueOffset))},
                                           {"width", px(static_cast<float>(contentWidth - 40))}})},
        {"selected-cell-type-row", style({{"top", px(static_cast<float>(selectedCardTop + kPropertySelectedTypeOffset))},
                                          {"width", px(static_cast<float>(contentWidth - 40))}})},
    });
}

void clearAttrRows(skui::Runtime& runtime) {
    for (int i = 0; i < kAttrPoolRowCount; ++i) {
        std::string rowBg = rowPoolId(i) + "-bg";
        runtime.removeClassById(rowBg, "row-selected-highlight");
    }
}

void clearAttrCells(skui::Runtime& runtime) {
    for (int i = 0; i < kAttrPoolRowCount; ++i) {
        for (std::string_view col : kAttrCols) {
            runtime.removeClassById(cellPoolId(i, col), "cell-selected-highlight");
        }
    }
}

void clearSelectedCellDetails(skui::Runtime& runtime) {
    runtime.setTextById("selected-cell-value", "");
    runtime.setTextById("selected-cell-type", "");
}

void selectAttrRow(skui::Runtime& runtime, int rowIndex) {
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", "");
    gPropertyState.selectedCellCol.clear();
    gPropertyState.selectedCellRow = -1;
    gPropertyState.selectedAttrRow = std::clamp(rowIndex, 0, kAttrTotalRows - 1);
    clearSelectedCellDetails(runtime);
    const int poolIndex = gPropertyState.selectedAttrRow - gPropertyState.firstAttrRow;
    if (poolIndex >= 0 && poolIndex < kAttrPoolRowCount) {
        runtime.addClassById(rowPoolId(poolIndex) + "-bg", "row-selected-highlight");
    }
}

void selectAttrCol(skui::Runtime& runtime, std::string_view colId) {
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    gPropertyState.selectedAttrRow = -1;
    gPropertyState.selectedCellRow = -1;
    gPropertyState.selectedCellCol.clear();
    clearSelectedCellDetails(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", colId);
}

void selectAttrCell(skui::Runtime& runtime,
                    std::string_view cellId,
                    int rowIndex,
                    std::string_view colId,
                    std::string_view value,
                    std::string_view type) {
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", "");
    gPropertyState.selectedAttrRow = -1;
    gPropertyState.selectedCellRow = std::clamp(rowIndex, 0, kAttrTotalRows - 1);
    gPropertyState.selectedCellCol = std::string(colId);
    runtime.addClassById(cellId, "cell-selected-highlight");
    runtime.setTextById("selected-cell-value", value);
    runtime.setTextById("selected-cell-type", type);
}

void refreshAttrWindow(skui::Runtime& runtime, float scrollY) {
    gPropertyState.attrScrollY = std::clamp(scrollY, 0.0f, static_cast<float>(kAttrVirtualHeight));
    gPropertyState.firstAttrRow = std::clamp(static_cast<int>(gPropertyState.attrScrollY / kAttrRowHeight), 0, kAttrTotalRows - 1);

    std::vector<skui::StyleUpdate> styles;
    std::vector<skui::TextUpdate> texts;
    std::vector<skui::AttributeUpdate> attributes;
    styles.reserve(static_cast<size_t>(kAttrPoolRowCount) * (std::size(kAttrCols) + 2));
    texts.reserve(static_cast<size_t>(kAttrPoolRowCount) * std::size(kAttrCols));
    attributes.reserve(static_cast<size_t>(kAttrPoolRowCount) * (std::size(kAttrCols) + 1));

    styles.push_back({"attr-handle-header", style({{"top", px(gPropertyState.attrScrollY)}})});
    for (std::string_view col : kAttrCols) {
        styles.push_back({"attr-header-" + std::string(col), style({{"left", px(static_cast<float>(attrColumnLeft(col)))},
                                                                     {"top", px(gPropertyState.attrScrollY)},
                                                                     {"width", px(static_cast<float>(attrColumnWidth(col)))}})});
    }

    for (int poolIndex = 0; poolIndex < kAttrPoolRowCount; ++poolIndex) {
        const int rowIndex = rowIndexForPool(poolIndex);
        const int top = static_cast<int>(std::lround(gPropertyState.attrScrollY)) + kAttrHeaderHeight + poolIndex * kAttrRowHeight;
        const std::string row = rowPoolId(poolIndex);
        const bool rowSelected = rowIndex == gPropertyState.selectedAttrRow;
        styles.push_back({row + "-bg", style({{"top", px(static_cast<float>(top))}})});
        styles.push_back({row + "-handle", style({{"top", px(static_cast<float>(top))}})});
        attributes.push_back({row + "-bg", "data-action", "select-attr-row:" + std::to_string(rowIndex)});
        attributes.push_back({row + "-handle", "data-action", "select-attr-row:" + std::to_string(rowIndex)});
        attributes.push_back({row + "-bg", "class", rowSelected ? "attr-row-bg row-selected-highlight" : "attr-row-bg"});

        for (std::string_view col : kAttrCols) {
            const std::string cell = cellPoolId(poolIndex, col);
            const bool cellSelected = rowIndex == gPropertyState.selectedCellRow && col == gPropertyState.selectedCellCol;
            styles.push_back({cell, style({{"left", px(static_cast<float>(attrColumnLeft(col)))},
                                           {"top", px(static_cast<float>(top))},
                                           {"width", px(static_cast<float>(attrColumnWidth(col)))}})});
            texts.push_back({cell, attrCellValue(rowIndex, col)});
            attributes.push_back({cell, "data-action", cellActionFor(poolIndex, rowIndex, col)});
            attributes.push_back({cell, "class", "attr-cell col-" + std::string(col) + (cellSelected ? " cell-selected-highlight" : "")});
        }
    }

    runtime.applyUpdates({std::move(styles), std::move(texts), std::move(attributes)});
}

void sortAttributes(skui::Runtime& runtime, std::string_view colId) {
    const bool sameColumn = gPropertyState.sortCol == colId;
    gPropertyState.sortAsc = sameColumn ? !gPropertyState.sortAsc : true;
    gPropertyState.sortCol = std::string(colId);
    std::vector<skui::TextUpdate> updates;
    updates.reserve(std::size(kAttrCols));
    for (std::string_view col : kAttrCols) {
        updates.push_back({"sort-" + std::string(col), col == colId ? (gPropertyState.sortAsc ? "^" : "v") : "-"});
    }
    runtime.setTextsById(updates);
    refreshAttrWindow(runtime, gPropertyState.attrScrollY);
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
    if (event.type == skui::ElementEventType::Scroll && action == "attr-table-scroll") {
        if (std::abs(event.scrollY - gPropertyState.attrScrollY) > 0.01f) {
            refreshAttrWindow(runtime, event.scrollY);
        }
        return;
    }

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
        const std::string_view value = action.substr(rowPrefix.size());
        int rowIndex = -1;
        std::from_chars(value.data(), value.data() + value.size(), rowIndex);
        selectAttrRow(runtime, rowIndex);
    } else if (action.rfind(colPrefix, 0) == 0) {
        selectAttrCol(runtime, action.substr(colPrefix.size()));
    } else if (action.rfind(cellPrefix, 0) == 0) {
        const std::vector<std::string_view> parts = splitActionPayload(action.substr(cellPrefix.size()), '|');
        if (parts.size() >= 5) {
            int rowIndex = -1;
            std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), rowIndex);
            selectAttrCell(runtime, parts[0], rowIndex, parts[2], parts[3], parts[4]);
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
    } else if (state == L"properties-min") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        setPropertyPanelWidth(runtime, kPropertyPanelMinWidth);
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
