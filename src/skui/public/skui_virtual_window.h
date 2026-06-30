#pragma once

#include <algorithm>
#include <cmath>

namespace skui {

struct VirtualWindowConfig {
    int itemCount = 0;
    int itemExtent = 1;
    int leadingExtent = 0;
    int poolSize = 0;
    int minCachedItems = 1;
    int overscanItems = 1;
};

struct VirtualWindowFrame {
    float scroll = 0.0f;
    int firstItem = 0;
    int scrollOffset = 0;
    int cachedItems = 0;
    bool cacheExpanded = false;
    bool renderNeeded = true;
};

class VirtualWindowState {
public:
    VirtualWindowState() = default;
    explicit VirtualWindowState(VirtualWindowConfig config) {
        configure(config);
    }

    void configure(VirtualWindowConfig config) {
        config.itemCount = std::max(0, config.itemCount);
        config.itemExtent = std::max(1, config.itemExtent);
        config.leadingExtent = std::max(0, config.leadingExtent);
        config.poolSize = std::max(1, config.poolSize);
        config.minCachedItems = std::clamp(config.minCachedItems, 1, config.poolSize);
        config.overscanItems = std::max(0, config.overscanItems);
        config_ = config;
        cachedItems_ = config_.minCachedItems;
        current_.cachedItems = cachedItems_;
        invalidate();
    }

    [[nodiscard]] const VirtualWindowConfig& config() const {
        return config_;
    }

    [[nodiscard]] VirtualWindowFrame update(float scroll, int viewportExtent, bool force = false) {
        viewportExtent = std::max(0, viewportExtent);
        const int nextCachedItems = requiredCachedItems(viewportExtent);
        const bool cacheExpanded = nextCachedItems > cachedItems_;
        if (cacheExpanded) {
            cachedItems_ = nextCachedItems;
        }

        current_.scroll = std::clamp(scroll, 0.0f, static_cast<float>(maxScroll(viewportExtent)));
        current_.firstItem = firstItemForScroll(current_.scroll);
        current_.scrollOffset = static_cast<int>(std::lround(current_.scroll));
        current_.cachedItems = cachedItems_;
        current_.cacheExpanded = cacheExpanded;
        current_.renderNeeded = force ||
            !rendered_ ||
            current_.firstItem != renderedFirstItem_ ||
            current_.scrollOffset != renderedScrollOffset_ ||
            current_.cachedItems != renderedCachedItems_;
        return current_;
    }

    void markRendered(const VirtualWindowFrame& frame) {
        rendered_ = true;
        renderedFirstItem_ = frame.firstItem;
        renderedScrollOffset_ = frame.scrollOffset;
        renderedCachedItems_ = frame.cachedItems;
    }

    void invalidate() {
        rendered_ = false;
        renderedFirstItem_ = -1;
        renderedScrollOffset_ = -1;
        renderedCachedItems_ = -1;
    }

    [[nodiscard]] int cachedItems() const {
        return cachedItems_;
    }

    [[nodiscard]] int firstItem() const {
        return current_.firstItem;
    }

private:
    [[nodiscard]] int contentExtent() const {
        return config_.leadingExtent + config_.itemCount * config_.itemExtent;
    }

    [[nodiscard]] int maxScroll(int viewportExtent) const {
        return std::max(0, contentExtent() - std::max(0, viewportExtent));
    }

    [[nodiscard]] int requiredCachedItems(int viewportExtent) const {
        const int bodyExtent = std::max(0, viewportExtent - config_.leadingExtent);
        const int visibleItems = (bodyExtent + config_.itemExtent - 1) / config_.itemExtent;
        return std::clamp(visibleItems + config_.overscanItems, config_.minCachedItems, config_.poolSize);
    }

    [[nodiscard]] int firstItemForScroll(float scroll) const {
        if (config_.itemCount <= 0) {
            return 0;
        }
        return std::clamp(static_cast<int>(scroll / static_cast<float>(config_.itemExtent)),
                          0,
                          config_.itemCount - 1);
    }

    VirtualWindowConfig config_;
    int cachedItems_ = 1;
    VirtualWindowFrame current_;
    bool rendered_ = false;
    int renderedFirstItem_ = -1;
    int renderedScrollOffset_ = -1;
    int renderedCachedItems_ = -1;
};

}  // namespace skui
