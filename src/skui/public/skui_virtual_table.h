#pragma once

#include "skui_runtime.h"
#include "skui_runtime_helpers.h"
#include "skui_virtual_window.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skui {

struct FlowRowLayoutConfig {
    std::vector<int> itemWidths;
    int itemHeight = 0;
    int gap = 0;
};

[[nodiscard]] inline int wrappedRowHeight(const int* itemWidths,
                                          size_t itemCount,
                                          int availableWidth,
                                          int itemHeight,
                                          int gap) {
    if (!itemWidths || itemCount == 0) {
        return 0;
    }

    availableWidth = std::max(0, availableWidth);
    itemHeight = std::max(0, itemHeight);
    gap = std::max(0, gap);

    int rows = 1;
    int lineWidth = 0;
    for (size_t i = 0; i < itemCount; ++i) {
        const int itemWidth = std::max(0, itemWidths[i]) + gap;
        if (lineWidth > 0 && lineWidth + itemWidth > availableWidth) {
            ++rows;
            lineWidth = 0;
        }
        lineWidth += itemWidth;
    }
    return rows * itemHeight + (rows - 1) * gap;
}

[[nodiscard]] inline int wrappedRowHeight(std::initializer_list<int> itemWidths,
                                          int availableWidth,
                                          int itemHeight,
                                          int gap) {
    return wrappedRowHeight(itemWidths.begin(), itemWidths.size(), availableWidth, itemHeight, gap);
}

[[nodiscard]] inline int wrappedRowHeight(const FlowRowLayoutConfig& config, int availableWidth) {
    return wrappedRowHeight(config.itemWidths.data(),
                            config.itemWidths.size(),
                            availableWidth,
                            config.itemHeight,
                            config.gap);
}

struct VirtualTableColumn {
    std::string id;
    int width = 0;
};

struct VirtualTablePanelConfig {
    int panelLeft = 0;
    int minPanelWidth = 0;
    int rightGap = 0;
    int contentInset = 0;
    int minContentWidth = 0;
    int toolbarExtraWidth = 0;
    int toolbarBaseHeight = 0;
    int tableBaseTop = 0;
    int tableMinHeight = 0;
    int tableBottomGap = 0;
    int reservedBottomHeight = 0;
    FlowRowLayoutConfig toolbar;
};

struct VirtualTablePanelFrame {
    int panelWidth = 0;
    int contentWidth = 0;
    int toolbarWidth = 0;
    int toolbarHeight = 0;
    int tableTop = 0;
    int tableHeight = 0;
};

[[nodiscard]] inline bool sameVirtualTablePanelFrame(const VirtualTablePanelFrame& lhs,
                                                     const VirtualTablePanelFrame& rhs) {
    return lhs.panelWidth == rhs.panelWidth &&
           lhs.contentWidth == rhs.contentWidth &&
           lhs.toolbarWidth == rhs.toolbarWidth &&
           lhs.toolbarHeight == rhs.toolbarHeight &&
           lhs.tableTop == rhs.tableTop;
}

class VirtualTablePanelLayout {
public:
    VirtualTablePanelLayout(VirtualTablePanelConfig config, int initialWidth)
        : config_(std::move(config)),
          requestedWidth_(std::max(0, initialWidth)) {
        frame_.panelWidth = std::max(config_.minPanelWidth, requestedWidth_);
    }

    [[nodiscard]] const VirtualTablePanelConfig& config() const {
        return config_;
    }

    [[nodiscard]] const VirtualTablePanelFrame& frame() const {
        return frame_;
    }

    [[nodiscard]] int panelWidth() const {
        return frame_.panelWidth;
    }

    [[nodiscard]] int requestedWidth() const {
        return requestedWidth_;
    }

    bool update(const Runtime& runtime, int requestedWidth) {
        requestedWidth_ = std::max(0, requestedWidth);
        frame_ = computeFrame(runtime, requestedWidth_);
        return renderNeeded();
    }

    void markRendered() {
        renderedFrame_ = frame_;
        rendered_ = true;
    }

    [[nodiscard]] bool renderNeeded() const {
        return !rendered_ || !sameVirtualTablePanelFrame(frame_, renderedFrame_);
    }

private:
    [[nodiscard]] VirtualTablePanelFrame computeFrame(const Runtime& runtime, int requestedWidth) const {
        VirtualTablePanelFrame frame;
        const int maxPanelWidth = std::max(config_.minPanelWidth,
                                           runtimeLogicalWidth(runtime) - config_.panelLeft - config_.rightGap);
        frame.panelWidth = std::clamp(requestedWidth, config_.minPanelWidth, maxPanelWidth);
        frame.contentWidth = std::max(config_.minContentWidth,
                                      frame.panelWidth - config_.contentInset * 2);
        frame.toolbarWidth = frame.contentWidth + config_.toolbarExtraWidth;
        frame.toolbarHeight = config_.toolbar.itemWidths.empty() ?
            config_.toolbarBaseHeight : wrappedRowHeight(config_.toolbar, frame.toolbarWidth);
        frame.tableTop = config_.tableBaseTop + std::max(0, frame.toolbarHeight - config_.toolbarBaseHeight);
        frame.tableHeight = std::max(config_.tableMinHeight,
                                     runtimeLogicalHeight(runtime) -
                                         config_.reservedBottomHeight -
                                         frame.tableTop -
                                         config_.tableBottomGap);
        return frame;
    }

    VirtualTablePanelConfig config_;
    int requestedWidth_ = 0;
    VirtualTablePanelFrame frame_;
    VirtualTablePanelFrame renderedFrame_;
    bool rendered_ = false;
};

class VirtualTableGeometry {
public:
    VirtualTableGeometry(std::string rowPrefix,
                         std::string cellPrefix,
                         std::string headerPrefix,
                         std::vector<VirtualTableColumn> columns,
                         int rowGutterWidth,
                         int headerHeight,
                         int rowHeight)
        : rowPrefix_(std::move(rowPrefix)),
          cellPrefix_(std::move(cellPrefix)),
          headerPrefix_(std::move(headerPrefix)),
          columns_(std::move(columns)),
          rowGutterWidth_(std::max(0, rowGutterWidth)),
          headerHeight_(std::max(0, headerHeight)),
          rowHeight_(std::max(1, rowHeight)) {}

    [[nodiscard]] const std::vector<VirtualTableColumn>& columns() const {
        return columns_;
    }

    [[nodiscard]] size_t columnCount() const {
        return columns_.size();
    }

    [[nodiscard]] int columnLeft(std::string_view columnId) const {
        int left = rowGutterWidth_;
        for (const VirtualTableColumn& column : columns_) {
            if (column.id == columnId) {
                return left;
            }
            left += std::max(0, column.width);
        }
        return rowGutterWidth_;
    }

    [[nodiscard]] int columnWidth(std::string_view columnId) const {
        for (const VirtualTableColumn& column : columns_) {
            if (column.id == columnId) {
                return std::max(0, column.width);
            }
        }
        return 0;
    }

    [[nodiscard]] int contentWidth() const {
        int width = rowGutterWidth_;
        for (const VirtualTableColumn& column : columns_) {
            width += std::max(0, column.width);
        }
        return width;
    }

    [[nodiscard]] int contentHeight(int itemCount) const {
        return headerHeight_ + std::max(0, itemCount) * rowHeight_;
    }

    [[nodiscard]] int bodyContentHeight(int itemCount) const {
        return std::max(0, itemCount) * rowHeight_;
    }

    [[nodiscard]] int headerHeight() const {
        return headerHeight_;
    }

    [[nodiscard]] int rowHeight() const {
        return rowHeight_;
    }

    [[nodiscard]] int rowTop(const VirtualWindowFrame& frame, int poolIndex) const {
        return frame.scrollOffset + headerHeight_ + poolIndex * rowHeight_;
    }

    [[nodiscard]] int bodyRowTop(const VirtualWindowFrame& frame, int poolIndex) const {
        return frame.scrollOffset + poolIndex * rowHeight_;
    }

    [[nodiscard]] std::string rowId(int poolIndex) const {
        return rowPrefix_ + std::to_string(poolIndex + 1);
    }

    [[nodiscard]] std::string rowBackgroundId(int poolIndex) const {
        return rowId(poolIndex) + "-bg";
    }

    [[nodiscard]] std::string rowHandleId(int poolIndex) const {
        return rowId(poolIndex) + "-handle";
    }

    [[nodiscard]] std::string cellId(int poolIndex, std::string_view columnId) const {
        return cellPrefix_ + std::to_string(poolIndex + 1) + "-" + std::string(columnId);
    }

    [[nodiscard]] std::string headerId(std::string_view columnId) const {
        return headerPrefix_ + std::string(columnId);
    }

    void appendHeaderStyles(std::vector<StyleUpdate>& styles,
                            const VirtualWindowFrame& frame,
                            std::string_view handleHeaderId,
                            bool headerInScroll) const {
        const std::string top = headerInScroll ? px(frame.scroll) : "0px";
        styles.push_back({std::string(handleHeaderId), style({{"top", top}})});
        for (const VirtualTableColumn& column : columns_) {
            styles.push_back({headerId(column.id),
                              style({{"left", px(static_cast<float>(columnLeft(column.id)))},
                                     {"top", top},
                                     {"width", px(static_cast<float>(columnWidth(column.id)))}})});
        }
    }

    void appendRowStyles(std::vector<StyleUpdate>& styles,
                         int poolIndex,
                         int top,
                         bool visible) const {
        styles.push_back({rowBackgroundId(poolIndex), style({{"top", px(static_cast<float>(top))},
                                                             {"display", visible ? "flex" : "none"}})});
        styles.push_back({rowHandleId(poolIndex), style({{"top", px(static_cast<float>(top))},
                                                         {"display", visible ? "flex" : "none"}})});
    }

    void appendCellStyle(std::vector<StyleUpdate>& styles,
                         int poolIndex,
                         std::string_view columnId,
                         int top,
                         bool visible) const {
        styles.push_back({cellId(poolIndex, columnId),
                          style({{"left", px(static_cast<float>(columnLeft(columnId)))},
                                 {"top", px(static_cast<float>(top))},
                                 {"width", px(static_cast<float>(columnWidth(columnId)))},
                                 {"display", visible ? "flex" : "none"}})});
    }

private:
    std::string rowPrefix_;
    std::string cellPrefix_;
    std::string headerPrefix_;
    std::vector<VirtualTableColumn> columns_;
    int rowGutterWidth_ = 0;
    int headerHeight_ = 0;
    int rowHeight_ = 1;
};

struct VirtualTableRenderConfig {
    std::string viewportId;
    std::string handleHeaderId;
    std::string rowBaseClass;
    std::string rowSelectedClass;
    std::string rowActionPrefix;
    std::string rowHandleActionPrefix;
    std::string cellBaseClass;
    std::string cellSelectedClass;
    std::string cellColumnClassPrefix;
    std::string headerContentId;
    bool headerInScroll = true;
};

struct VirtualTableRowContext {
    int rowIndex = 0;
    int poolIndex = 0;
    bool visible = false;
    std::string rowBackgroundId;
    std::string rowHandleId;
};

struct VirtualTableCellContext {
    int rowIndex = 0;
    int poolIndex = 0;
    bool visible = false;
    std::string_view columnId;
    std::string cellId;
};

struct VirtualTableRowData {
    std::string action;
    std::string handleAction;
    std::string className;
    bool selected = false;
};

struct VirtualTableCellData {
    std::string text;
    std::string action;
    std::string className;
    bool selected = false;
};

struct VirtualTableDataSource {
    int itemCount = 0;
    std::function<VirtualTableRowData(const VirtualTableRowContext&)> row;
    std::function<VirtualTableCellData(const VirtualTableCellContext&)> cell;
};

class VirtualTableAdapter {
public:
    VirtualTableAdapter(VirtualTableGeometry geometry,
                        VirtualWindowConfig windowConfig,
                        VirtualTableRenderConfig renderConfig)
        : geometry_(std::move(geometry)),
          renderConfig_(std::move(renderConfig)) {
        itemCount_ = std::max(0, windowConfig.itemCount);
        if (!renderConfig_.headerInScroll) {
            windowConfig.leadingExtent = 0;
        }
        window_.configure(windowConfig);
    }

    [[nodiscard]] const VirtualTableGeometry& geometry() const {
        return geometry_;
    }

    [[nodiscard]] const VirtualWindowFrame& frame() const {
        return frame_;
    }

    [[nodiscard]] int firstItem() const {
        return frame_.firstItem;
    }

    [[nodiscard]] float scroll() const {
        return frame_.scroll;
    }

    [[nodiscard]] int rowIndexForPool(int poolIndex) const {
        if (itemCount_ <= 0) {
            return 0;
        }
        return std::clamp(frame_.firstItem + poolIndex, 0, itemCount_ - 1);
    }

    [[nodiscard]] std::string rowBackgroundId(int poolIndex) const {
        return geometry_.rowBackgroundId(poolIndex);
    }

    [[nodiscard]] std::string rowHandleId(int poolIndex) const {
        return geometry_.rowHandleId(poolIndex);
    }

    [[nodiscard]] std::string cellId(int poolIndex, std::string_view columnId) const {
        return geometry_.cellId(poolIndex, columnId);
    }

    [[nodiscard]] int contentWidth() const {
        return geometry_.contentWidth();
    }

    [[nodiscard]] int contentHeight() const {
        return geometry_.contentHeight(itemCount_);
    }

    [[nodiscard]] int scrollContentHeight() const {
        return renderConfig_.headerInScroll
            ? geometry_.contentHeight(itemCount_)
            : geometry_.bodyContentHeight(itemCount_);
    }

    [[nodiscard]] const VirtualWindowConfig& windowConfig() const {
        return window_.config();
    }

    void invalidate() {
        window_.invalidate();
    }

    bool syncViewport(Runtime& runtime, int itemCount) {
        const bool itemCountChanged = setItemCount(itemCount);
        const int nextContentWidth = contentWidth();
        const int nextContentHeight = scrollContentHeight();
        const bool viewportChanged = nextContentWidth != syncedContentWidth_ ||
            nextContentHeight != syncedContentHeight_;
        if (!renderConfig_.viewportId.empty() && viewportChanged) {
            runtime.setAttributesById({
                {renderConfig_.viewportId, "data-virtual-width", std::to_string(nextContentWidth)},
                {renderConfig_.viewportId, "data-virtual-height", std::to_string(nextContentHeight)},
            });
            syncedContentWidth_ = nextContentWidth;
            syncedContentHeight_ = nextContentHeight;
        }
        return itemCountChanged;
    }

    bool syncHeaderScrollX(Runtime& runtime, float scrollX, bool force = false) {
        if (renderConfig_.headerContentId.empty()) {
            return false;
        }
        scrollX = std::max(0.0f, scrollX);
        if (!force && headerScrollSynced_ && std::abs(scrollX - syncedHeaderScrollX_) <= 0.01f) {
            return false;
        }

        runtime.setStylesById({
            {renderConfig_.headerContentId, style({{"left", px(-scrollX)}})},
        });
        syncedHeaderScrollX_ = scrollX;
        headerScrollSynced_ = true;
        return true;
    }

    [[nodiscard]] VirtualWindowFrame updateWindow(float scroll, int viewportExtent, bool force = false) {
        frame_ = window_.update(scroll, viewportExtent, force);
        return frame_;
    }

    bool refresh(Runtime& runtime,
                 float scroll,
                 int viewportExtent,
                 const VirtualTableDataSource& dataSource,
                 bool force = false) {
        const bool itemCountChanged = syncViewport(runtime, dataSource.itemCount);
        const VirtualWindowFrame nextFrame = updateWindow(scroll, viewportExtent, force || itemCountChanged);
        if (!nextFrame.renderNeeded) {
            return false;
        }

        std::vector<StyleUpdate> styles;
        std::vector<TextUpdate> texts;
        std::vector<AttributeUpdate> attributes;
        const size_t poolSize = static_cast<size_t>(window_.config().poolSize);
        styles.reserve(poolSize * (geometry_.columnCount() + 2));
        texts.reserve(poolSize * geometry_.columnCount());
        attributes.reserve(poolSize * (geometry_.columnCount() * 2 + 3));

        geometry_.appendHeaderStyles(styles,
                                     nextFrame,
                                     renderConfig_.handleHeaderId,
                                     renderConfig_.headerInScroll);

        for (int poolIndex = 0; poolIndex < window_.config().poolSize; ++poolIndex) {
            const bool visible = poolIndex < nextFrame.cachedItems &&
                nextFrame.firstItem + poolIndex < itemCount_;
            const int rowIndex = rowIndexForPool(poolIndex);
            const int top = renderConfig_.headerInScroll
                ? geometry_.rowTop(nextFrame, poolIndex)
                : geometry_.bodyRowTop(nextFrame, poolIndex);
            geometry_.appendRowStyles(styles, poolIndex, top, visible);
            appendRowAttributes(attributes, dataSource, rowIndex, poolIndex, visible);

            for (const VirtualTableColumn& column : geometry_.columns()) {
                const std::string cell = geometry_.cellId(poolIndex, column.id);
                geometry_.appendCellStyle(styles, poolIndex, column.id, top, visible);
                if (!visible) {
                    continue;
                }
                const VirtualTableCellData cellData = loadCell(dataSource,
                                                               rowIndex,
                                                               poolIndex,
                                                               column.id,
                                                               cell,
                                                               visible);
                texts.push_back({cell, cellData.text});
                attributes.push_back({cell, "data-action", cellData.action});
                attributes.push_back({cell, "class", cellClassName(column.id, cellData)});
            }
        }

        runtime.applyUpdates({std::move(styles), std::move(texts), std::move(attributes)});
        window_.markRendered(nextFrame);
        return true;
    }

private:
    bool setItemCount(int itemCount) {
        itemCount = std::max(0, itemCount);
        if (itemCount == itemCount_) {
            return false;
        }
        itemCount_ = itemCount;
        VirtualWindowConfig config = window_.config();
        config.itemCount = itemCount_;
        window_.configure(config);
        return true;
    }

    [[nodiscard]] VirtualTableRowData loadRow(const VirtualTableDataSource& dataSource,
                                              int rowIndex,
                                              int poolIndex,
                                              bool visible) const {
        if (!dataSource.row) {
            return {};
        }
        return dataSource.row({rowIndex,
                               poolIndex,
                               visible,
                               geometry_.rowBackgroundId(poolIndex),
                               geometry_.rowHandleId(poolIndex)});
    }

    [[nodiscard]] VirtualTableCellData loadCell(const VirtualTableDataSource& dataSource,
                                                int rowIndex,
                                                int poolIndex,
                                                std::string_view columnId,
                                                const std::string& cell,
                                                bool visible) const {
        if (!dataSource.cell) {
            return {};
        }
        return dataSource.cell({rowIndex, poolIndex, visible, columnId, cell});
    }

    void appendRowAttributes(std::vector<AttributeUpdate>& attributes,
                             const VirtualTableDataSource& dataSource,
                             int rowIndex,
                             int poolIndex,
                             bool visible) const {
        if (!visible) {
            return;
        }
        VirtualTableRowData rowData = loadRow(dataSource, rowIndex, poolIndex, visible);
        if (rowData.action.empty() && !renderConfig_.rowActionPrefix.empty()) {
            rowData.action = renderConfig_.rowActionPrefix + std::to_string(rowIndex);
        }
        if (rowData.handleAction.empty()) {
            rowData.handleAction = !renderConfig_.rowHandleActionPrefix.empty()
                ? renderConfig_.rowHandleActionPrefix + std::to_string(rowIndex)
                : rowData.action;
        }

        const std::string rowBg = geometry_.rowBackgroundId(poolIndex);
        const std::string rowHandle = geometry_.rowHandleId(poolIndex);
        attributes.push_back({rowBg, "data-action", rowData.action});
        attributes.push_back({rowHandle, "data-action", rowData.handleAction});
        attributes.push_back({rowBg, "class", rowClassName(rowData)});
    }

    [[nodiscard]] std::string rowClassName(const VirtualTableRowData& rowData) const {
        if (!rowData.className.empty()) {
            return rowData.className;
        }
        std::string result = renderConfig_.rowBaseClass;
        if (rowData.selected) {
            appendClass(result, renderConfig_.rowSelectedClass);
        }
        return result;
    }

    [[nodiscard]] std::string cellClassName(std::string_view columnId,
                                            const VirtualTableCellData& cellData) const {
        if (!cellData.className.empty()) {
            return cellData.className;
        }
        std::string result = renderConfig_.cellBaseClass;
        const std::string columnClass = renderConfig_.cellColumnClassPrefix + std::string(columnId);
        appendClass(result, columnClass);
        if (cellData.selected) {
            appendClass(result, renderConfig_.cellSelectedClass);
        }
        return result;
    }

    static void appendClass(std::string& className, std::string_view nextClass) {
        if (nextClass.empty()) {
            return;
        }
        if (!className.empty()) {
            className.push_back(' ');
        }
        className.append(nextClass);
    }

    VirtualTableGeometry geometry_;
    VirtualTableRenderConfig renderConfig_;
    VirtualWindowState window_;
    VirtualWindowFrame frame_;
    int itemCount_ = 0;
    int syncedContentWidth_ = -1;
    int syncedContentHeight_ = -1;
    float syncedHeaderScrollX_ = 0.0f;
    bool headerScrollSynced_ = false;
};

} // namespace skui
