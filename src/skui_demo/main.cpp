#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_win32_app.h"
#include "skui_runtime_helpers.h"
#include "skui_virtual_table.h"
#include "skui_virtual_window.h"

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
const skui::VirtualTableGeometry kAttrTable("attr-row-",
                                            "attr-cell-",
                                            "attr-header-",
                                            {{"id", 80},
                                             {"name", 120},
                                             {"landuse", 150},
                                             {"height", 110},
                                             {"type", 120},
                                             {"owner", 160},
                                             {"area", 120},
                                             {"updated", 160},
                                             {"status", 120},
                                             {"note", 220}},
                                            44,
                                            42,
                                            40);
constexpr int kAttrPoolRowCount = 48;
constexpr int kAttrTotalRows = 100000;
constexpr std::string_view kPropertyLayers[] = {
    "地块边界.shp",
    "道路中心线.shp",
    "控制点.shp",
    "建筑物.shp",
};
constexpr int kPropertyPanelLeft = 134;
constexpr int kPropertyPanelMinWidth = 500;
constexpr int kPropertyPanelDefaultWidth = 610;
constexpr int kPropertyContentInset = 26;
constexpr int kPropertyRightGap = 8;
constexpr int kPropertyStatusHeight = 60;
constexpr int kPropertyTableBaseTop = 258;
constexpr int kPropertyTableMinHeight = 180;
constexpr int kPropertyTableBottomGap = 252;
constexpr int kPropertyToolbarButtonHeight = 39;
constexpr int kPropertyToolbarGap = 8;
constexpr int kPropertyToolbarBaseHeight = 42;
const skui::VirtualTablePanelConfig kPropertyPanelLayoutConfig{
    kPropertyPanelLeft,
    kPropertyPanelMinWidth,
    kPropertyRightGap,
    kPropertyContentInset,
    320,
    8,
    kPropertyToolbarBaseHeight,
    kPropertyTableBaseTop,
    kPropertyTableMinHeight,
    kPropertyTableBottomGap,
    kPropertyStatusHeight,
    {{100, 100, 88, 88, 98, 78, 78}, kPropertyToolbarButtonHeight, kPropertyToolbarGap},
};

struct PropertyDemoState {
    bool draggingPanel = false;
    bool attributesVisible = true;
    bool layerDropdownOpen = false;
    bool propertiesPageVisible = false;
    skui::VirtualTablePanelLayout panelLayout{kPropertyPanelLayoutConfig, kPropertyPanelDefaultWidth};
    float dragStartX = 0.0f;
    int dragStartWidth = kPropertyPanelDefaultWidth;
    float attrScrollY = 0.0f;
    skui::VirtualWindowState attrWindow{{kAttrTotalRows,
                                          kAttrTable.rowHeight(),
                                          kAttrTable.headerHeight(),
                                          kAttrPoolRowCount,
                                          7,
                                          1}};
    int selectedAttrRow = -1;
    int selectedCellRow = 1;
    std::string selectedCellCol = "landuse";
    std::string sortCol;
    std::string copiedAttrCol;
    bool sortAsc = true;
    int layerIndex = 0;
    int layerCount = 5;
    int visibleLayerCount = 5;
    int totalFeatureCount = 11272;
};

PropertyDemoState gPropertyState;

using skui::px;
using skui::splitActionPayload;
using skui::style;

bool setPropertyPanelWidth(skui::Runtime& runtime, int width);
void refreshAttrWindow(skui::Runtime& runtime, float scrollY, bool force = false);
void layoutPropertyPage(skui::Runtime& runtime);
void sortAttributes(skui::Runtime& runtime, std::string_view colId);

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

std::string groupedNumber(int value) {
    std::string digits = std::to_string(std::max(0, value));
    for (int insert = static_cast<int>(digits.size()) - 3; insert > 0; insert -= 3) {
        digits.insert(static_cast<size_t>(insert), ",");
    }
    return digits;
}

void setStatusSummary(skui::Runtime& runtime, std::string_view message = {}) {
    runtime.setTextById("status-layer-count", "图层:" + std::to_string(gPropertyState.layerCount));
    runtime.setTextById("status-visible-count", "可见:" + std::to_string(gPropertyState.visibleLayerCount));
    if (message.empty()) {
        runtime.setTextById("status-feature-count", "总要素:" + groupedNumber(gPropertyState.totalFeatureCount));
    } else {
        runtime.setTextById("status-feature-count", message);
    }
}

std::string rowPoolId(int poolIndex) {
    return kAttrTable.rowId(poolIndex);
}

std::string cellPoolId(int poolIndex, std::string_view col) {
    return kAttrTable.cellId(poolIndex, col);
}

int rowIndexForPool(int poolIndex) {
    return std::clamp(gPropertyState.attrWindow.firstItem() + poolIndex, 0, kAttrTotalRows - 1);
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

std::string cellActionFor(int poolIndex, int rowIndex, std::string_view col) {
    return "select-attr-cell:" + cellPoolId(poolIndex, col) + "|" +
           std::to_string(rowIndex) + "|" + std::string(col) + "|" +
           attrCellValue(rowIndex, col) + "|" + attrCellType(col);
}

void showPage(skui::Runtime& runtime, std::string_view page) {
    if (page == "properties") {
        gPropertyState.propertiesPageVisible = true;
        runtime.addClassById("layer-page", "page-hidden");
        runtime.removeClassById("properties-page", "page-hidden");
        runtime.setAttributesById({{"property-table-viewport", "data-virtual-width", std::to_string(kAttrTable.contentWidth())},
                                   {"property-table-viewport", "data-virtual-height", std::to_string(kAttrTable.contentHeight(kAttrTotalRows))}});
        setPropertyPanelWidth(runtime, gPropertyState.panelLayout.requestedWidth());
        refreshAttrWindow(runtime, gPropertyState.attrScrollY, true);
    } else {
        gPropertyState.propertiesPageVisible = false;
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

bool setPropertyPanelWidth(skui::Runtime& runtime, int width) {
    const bool panelChanged = gPropertyState.panelLayout.update(runtime, width);
    const skui::VirtualTablePanelFrame& panel = gPropertyState.panelLayout.frame();
    const skui::VirtualWindowFrame frame = gPropertyState.attrWindow.update(gPropertyState.attrScrollY, panel.tableHeight);
    if (panelChanged) {
        runtime.setStylesById({
            {"property-panel", style({{"width", px(static_cast<float>(panel.panelWidth))}})},
            {"property-search", style({{"width", px(static_cast<float>(panel.contentWidth))}})},
            {"property-toolbar", style({{"width", px(static_cast<float>(panel.toolbarWidth))},
                                        {"height", px(static_cast<float>(panel.toolbarHeight))}})},
            {"property-table-viewport", style({{"top", px(static_cast<float>(panel.tableTop))},
                                               {"width", px(static_cast<float>(panel.contentWidth))}})},
            {"selected-cell-card", style({{"width", px(static_cast<float>(panel.contentWidth))}})},
            {"selected-cell-value-row", style({{"bottom", "132px"},
                                               {"width", px(static_cast<float>(std::max(0, panel.contentWidth - 40)))}})},
            {"selected-cell-type-row", style({{"bottom", "86px"},
                                              {"width", px(static_cast<float>(std::max(0, panel.contentWidth - 40)))}})},
        });
        gPropertyState.panelLayout.markRendered();
    }
    return frame.cacheExpanded;
}

void layoutPropertyPage(skui::Runtime& runtime) {
    if (!gPropertyState.propertiesPageVisible) {
        return;
    }
    const bool visibleRowsChanged = setPropertyPanelWidth(runtime, gPropertyState.panelLayout.requestedWidth());
    refreshAttrWindow(runtime, gPropertyState.attrScrollY, visibleRowsChanged);
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

void invalidateAttrWindow() {
    gPropertyState.attrWindow.invalidate();
}

void selectAttrRow(skui::Runtime& runtime, int rowIndex) {
    invalidateAttrWindow();
    clearAttrRows(runtime);
    clearAttrCells(runtime);
    runtime.setAttributeById("properties-page", "data-selected-col", "");
    gPropertyState.selectedCellCol.clear();
    gPropertyState.selectedCellRow = -1;
    gPropertyState.selectedAttrRow = std::clamp(rowIndex, 0, kAttrTotalRows - 1);
    clearSelectedCellDetails(runtime);
    const int poolIndex = gPropertyState.selectedAttrRow - gPropertyState.attrWindow.firstItem();
    if (poolIndex >= 0 && poolIndex < kAttrPoolRowCount) {
        runtime.addClassById(rowPoolId(poolIndex) + "-bg", "row-selected-highlight");
    }
}

void selectAttrCol(skui::Runtime& runtime, std::string_view colId) {
    invalidateAttrWindow();
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
    invalidateAttrWindow();
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

void selectVisibleCell(skui::Runtime& runtime, int rowIndex, std::string_view colId) {
    const int poolIndex = rowIndex - gPropertyState.attrWindow.firstItem();
    if (poolIndex < 0 || poolIndex >= kAttrPoolRowCount) {
        setStatusSummary(runtime, "单元格不在当前视图");
        return;
    }
    const std::string cellId = cellPoolId(poolIndex, colId);
    selectAttrCell(runtime, cellId, rowIndex, colId, attrCellValue(rowIndex, colId), attrCellType(colId));
}

void handleImportShp(skui::Runtime& runtime) {
    skui::RuntimeUpdateBatch batch(runtime);
    ++gPropertyState.layerCount;
    ++gPropertyState.visibleLayerCount;
    gPropertyState.totalFeatureCount += 1846;
    setStatusSummary(runtime, "已导入: survey_" + std::to_string(gPropertyState.layerCount) + ".shp");
}

void handleCreateLayer(skui::Runtime& runtime) {
    skui::RuntimeUpdateBatch batch(runtime);
    ++gPropertyState.layerCount;
    ++gPropertyState.visibleLayerCount;
    setStatusSummary(runtime, "已新建图层:" + std::to_string(gPropertyState.layerCount));
}

void handleAddField(skui::Runtime& runtime) {
    skui::RuntimeUpdateBatch batch(runtime);
    selectAttrCol(runtime, "note");
    setStatusSummary(runtime, "已新增字段: note");
}

void handleDeleteField(skui::Runtime& runtime) {
    skui::RuntimeUpdateBatch batch(runtime);
    if (gPropertyState.selectedCellCol.empty()) {
        setStatusSummary(runtime, "请先选择字段");
        return;
    }
    const std::string deleted = gPropertyState.selectedCellCol;
    selectAttrCol(runtime, "");
    setStatusSummary(runtime, "已删除字段: " + deleted);
}

void handleCopyColumn(skui::Runtime& runtime) {
    if (gPropertyState.selectedCellCol.empty()) {
        setStatusSummary(runtime, "请先选择字段");
        return;
    }
    gPropertyState.copiedAttrCol = gPropertyState.selectedCellCol;
    setStatusSummary(runtime, "已复制列: " + gPropertyState.copiedAttrCol);
}

void handlePasteColumn(skui::Runtime& runtime) {
    if (gPropertyState.copiedAttrCol.empty()) {
        setStatusSummary(runtime, "没有可粘贴的列");
        return;
    }
    skui::RuntimeUpdateBatch batch(runtime);
    selectAttrCol(runtime, gPropertyState.copiedAttrCol);
    setStatusSummary(runtime, "已粘贴列: " + gPropertyState.copiedAttrCol);
}

void handleHeightField(skui::Runtime& runtime) {
    skui::RuntimeUpdateBatch batch(runtime);
    selectAttrCol(runtime, "height");
    sortAttributes(runtime, "height");
    setStatusSummary(runtime, "已设为高度字段: height");
}

void handleFocusSelection(skui::Runtime& runtime) {
    skui::RuntimeUpdateBatch batch(runtime);
    if (gPropertyState.selectedCellRow >= 0 && !gPropertyState.selectedCellCol.empty()) {
        selectVisibleCell(runtime, gPropertyState.selectedCellRow, gPropertyState.selectedCellCol);
        setStatusSummary(runtime, "已对焦行:" + std::to_string(gPropertyState.selectedCellRow + 1));
        return;
    }
    if (gPropertyState.selectedAttrRow >= 0) {
        selectAttrRow(runtime, gPropertyState.selectedAttrRow);
        setStatusSummary(runtime, "已对焦行:" + std::to_string(gPropertyState.selectedAttrRow + 1));
        return;
    }
    selectAttrRow(runtime, rowIndexForPool(0));
    setStatusSummary(runtime, "已对焦首行");
}

void refreshAttrWindow(skui::Runtime& runtime, float scrollY, bool force) {
    const skui::VirtualTablePanelFrame& panel = gPropertyState.panelLayout.frame();
    const skui::VirtualWindowFrame frame = gPropertyState.attrWindow.update(scrollY, panel.tableHeight, force);
    gPropertyState.attrScrollY = frame.scroll;
    if (!frame.renderNeeded) {
        return;
    }

    std::vector<skui::StyleUpdate> styles;
    std::vector<skui::TextUpdate> texts;
    std::vector<skui::AttributeUpdate> attributes;
    styles.reserve(static_cast<size_t>(kAttrPoolRowCount) * (std::size(kAttrCols) + 2));
    texts.reserve(static_cast<size_t>(kAttrPoolRowCount) * std::size(kAttrCols));
    attributes.reserve(static_cast<size_t>(kAttrPoolRowCount) * (std::size(kAttrCols) + 1));

    kAttrTable.appendHeaderStyles(styles, frame, "attr-handle-header");

    for (int poolIndex = 0; poolIndex < kAttrPoolRowCount; ++poolIndex) {
        const int rowIndex = rowIndexForPool(poolIndex);
        const int top = kAttrTable.rowTop(frame, poolIndex);
        const std::string row = rowPoolId(poolIndex);
        const bool cached = poolIndex < frame.cachedItems;
        const bool rowSelected = rowIndex == gPropertyState.selectedAttrRow;
        kAttrTable.appendRowStyles(styles, poolIndex, top, cached);
        attributes.push_back({row + "-bg", "data-action", "select-attr-row:" + std::to_string(rowIndex)});
        attributes.push_back({row + "-handle", "data-action", "select-attr-row:" + std::to_string(rowIndex)});
        attributes.push_back({row + "-bg", "class", rowSelected ? "attr-row-bg row-selected-highlight" : "attr-row-bg"});

        for (std::string_view col : kAttrCols) {
            const std::string cell = cellPoolId(poolIndex, col);
            const bool cellSelected = rowIndex == gPropertyState.selectedCellRow && col == gPropertyState.selectedCellCol;
            kAttrTable.appendCellStyle(styles, poolIndex, col, top, cached);
            texts.push_back({cell, attrCellValue(rowIndex, col)});
            attributes.push_back({cell, "data-action", cellActionFor(poolIndex, rowIndex, col)});
            attributes.push_back({cell, "class", "attr-cell col-" + std::string(col) + (cellSelected ? " cell-selected-highlight" : "")});
        }
    }

    runtime.applyUpdates({std::move(styles), std::move(texts), std::move(attributes)});
    gPropertyState.attrWindow.markRendered(frame);
}

void sortAttributes(skui::Runtime& runtime, std::string_view colId) {
    invalidateAttrWindow();
    const bool sameColumn = gPropertyState.sortCol == colId;
    gPropertyState.sortAsc = sameColumn ? !gPropertyState.sortAsc : true;
    gPropertyState.sortCol = std::string(colId);
    std::vector<skui::TextUpdate> updates;
    updates.reserve(std::size(kAttrCols));
    for (std::string_view col : kAttrCols) {
        updates.push_back({"sort-" + std::string(col), col == colId ? (gPropertyState.sortAsc ? "^" : "v") : "-"});
    }
    runtime.setTextsById(updates);
    refreshAttrWindow(runtime, gPropertyState.attrScrollY, true);
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
            gPropertyState.dragStartWidth = gPropertyState.panelLayout.panelWidth();
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
        skui::RuntimeUpdateBatch batch(runtime);
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
        skui::RuntimeUpdateBatch batch(runtime);
        selectAttributeLayer(runtime, index);
    } else if (action.rfind(rowPrefix, 0) == 0) {
        const std::string_view value = action.substr(rowPrefix.size());
        int rowIndex = -1;
        std::from_chars(value.data(), value.data() + value.size(), rowIndex);
        skui::RuntimeUpdateBatch batch(runtime);
        selectAttrRow(runtime, rowIndex);
    } else if (action.rfind(colPrefix, 0) == 0) {
        skui::RuntimeUpdateBatch batch(runtime);
        selectAttrCol(runtime, action.substr(colPrefix.size()));
    } else if (action.rfind(cellPrefix, 0) == 0) {
        const std::vector<std::string_view> parts = splitActionPayload(action.substr(cellPrefix.size()), '|');
        if (parts.size() >= 5) {
            int rowIndex = -1;
            std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), rowIndex);
            skui::RuntimeUpdateBatch batch(runtime);
            selectAttrCell(runtime, parts[0], rowIndex, parts[2], parts[3], parts[4]);
        }
    } else if (action.rfind(sortPrefix, 0) == 0) {
        skui::RuntimeUpdateBatch batch(runtime);
        sortAttributes(runtime, action.substr(sortPrefix.size()));
    } else if (action == "attr-add-field") {
        handleAddField(runtime);
    } else if (action == "attr-delete-field") {
        handleDeleteField(runtime);
    } else if (action == "attr-copy-column") {
        handleCopyColumn(runtime);
    } else if (action == "attr-paste-column") {
        handlePasteColumn(runtime);
    } else if (action == "attr-highlight") {
        handleHeightField(runtime);
    } else if (action == "attr-focus") {
        handleFocusSelection(runtime);
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
            skui::RuntimeUpdateBatch batch(runtime);
            selectNavItem(runtime, action.substr(navPrefix.size()));
        } else if (action == "import-shp") {
            handleImportShp(runtime);
        } else if (action == "create-layer") {
            handleCreateLayer(runtime);
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
    } else if (state == L"properties-hidden") {
        clickAt(runtime, 63.0f, 557.0f, dpiScale);
        clickAt(runtime, 292.0f, 272.0f, dpiScale);
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
    options.onRuntimeResize = [](skui::Runtime& runtime) {
        layoutPropertyPage(runtime);
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
