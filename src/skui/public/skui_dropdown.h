#pragma once

#include "skui_runtime.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace skui {

struct DropdownConfig {
    std::string selectedTextId;
    std::string arrowId;
    std::string menuId;
    std::string backdropId;
    std::string optionIdPrefix;
    std::string hiddenClass = "page-hidden";
    std::string selectedClass;
    std::string openArrow = "^";
    std::string closedArrow = "v";
    size_t optionCount = 0;
};

class DropdownState {
public:
    DropdownState() = default;

    explicit DropdownState(DropdownConfig config, int selectedIndex = 0)
        : config_(std::move(config)) {
        selectedIndex_ = clampIndex(selectedIndex);
    }

    [[nodiscard]] bool open() const {
        return open_;
    }

    [[nodiscard]] int selectedIndex() const {
        return selectedIndex_;
    }

    void setOpen(Runtime& runtime, bool open) {
        open_ = open;
        applyOpenState(runtime);
    }

    void toggle(Runtime& runtime) {
        setOpen(runtime, !open_);
    }

    bool select(Runtime& runtime, int index, std::string_view selectedText) {
        if (!isValidIndex(index)) {
            return false;
        }

        selectedIndex_ = index;
        if (!config_.selectedTextId.empty()) {
            runtime.setTextById(config_.selectedTextId, selectedText);
        }
        applySelectedOption(runtime);
        setOpen(runtime, false);
        return true;
    }

    void sync(Runtime& runtime, std::string_view selectedText) {
        if (!config_.selectedTextId.empty()) {
            runtime.setTextById(config_.selectedTextId, selectedText);
        }
        applySelectedOption(runtime);
        applyOpenState(runtime);
    }

private:
    [[nodiscard]] bool isValidIndex(int index) const {
        return index >= 0 && static_cast<size_t>(index) < config_.optionCount;
    }

    [[nodiscard]] int clampIndex(int index) const {
        if (config_.optionCount == 0) {
            return -1;
        }
        return std::clamp(index, 0, static_cast<int>(config_.optionCount - 1));
    }

    [[nodiscard]] std::string optionId(size_t index) const {
        return config_.optionIdPrefix + std::to_string(index);
    }

    void applyOpenState(Runtime& runtime) const {
        if (!config_.arrowId.empty()) {
            runtime.setTextById(config_.arrowId, open_ ? config_.openArrow : config_.closedArrow);
        }
        if (!config_.backdropId.empty() && !config_.hiddenClass.empty()) {
            if (open_) {
                runtime.removeClassById(config_.backdropId, config_.hiddenClass);
            } else {
                runtime.addClassById(config_.backdropId, config_.hiddenClass);
            }
        }
        if (!config_.menuId.empty() && !config_.hiddenClass.empty()) {
            if (open_) {
                runtime.removeClassById(config_.menuId, config_.hiddenClass);
            } else {
                runtime.addClassById(config_.menuId, config_.hiddenClass);
            }
        }
    }

    void applySelectedOption(Runtime& runtime) const {
        if (config_.optionIdPrefix.empty() || config_.selectedClass.empty()) {
            return;
        }
        for (size_t i = 0; i < config_.optionCount; ++i) {
            const std::string id = optionId(i);
            if (static_cast<int>(i) == selectedIndex_) {
                runtime.addClassById(id, config_.selectedClass);
            } else {
                runtime.removeClassById(id, config_.selectedClass);
            }
        }
    }

    DropdownConfig config_;
    int selectedIndex_ = -1;
    bool open_ = false;
};

} // namespace skui
