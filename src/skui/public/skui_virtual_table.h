#pragma once

#include "skui_runtime.h"
#include "skui_runtime_helpers.h"
#include "skui_virtual_window.h"

#include <algorithm>
#include <cstddef>
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

    [[nodiscard]] int headerHeight() const {
        return headerHeight_;
    }

    [[nodiscard]] int rowHeight() const {
        return rowHeight_;
    }

    [[nodiscard]] int rowTop(const VirtualWindowFrame& frame, int poolIndex) const {
        return frame.scrollOffset + headerHeight_ + poolIndex * rowHeight_;
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
                            std::string_view handleHeaderId) const {
        styles.push_back({std::string(handleHeaderId), style({{"top", px(frame.scroll)}})});
        for (const VirtualTableColumn& column : columns_) {
            styles.push_back({headerId(column.id),
                              style({{"left", px(static_cast<float>(columnLeft(column.id)))},
                                     {"top", px(frame.scroll)},
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

} // namespace skui
