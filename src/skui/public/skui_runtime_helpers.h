#pragma once

#include "skui_runtime.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skui {

class RuntimeUpdateBatch {
public:
    explicit RuntimeUpdateBatch(Runtime& runtime) : runtime_(&runtime) {
        runtime_->beginUpdate();
    }

    ~RuntimeUpdateBatch() {
        if (runtime_) {
            runtime_->endUpdate();
        }
    }

    RuntimeUpdateBatch(const RuntimeUpdateBatch&) = delete;
    RuntimeUpdateBatch& operator=(const RuntimeUpdateBatch&) = delete;

    RuntimeUpdateBatch(RuntimeUpdateBatch&& other) noexcept : runtime_(other.runtime_) {
        other.runtime_ = nullptr;
    }

    RuntimeUpdateBatch& operator=(RuntimeUpdateBatch&&) = delete;

private:
    Runtime* runtime_ = nullptr;
};

[[nodiscard]] inline int runtimeLogicalWidth(const Runtime& runtime) {
    return static_cast<int>(std::lround(static_cast<float>(runtime.width()) / runtime.effectiveScale()));
}

[[nodiscard]] inline int runtimeLogicalHeight(const Runtime& runtime) {
    return static_cast<int>(std::lround(static_cast<float>(runtime.height()) / runtime.effectiveScale()));
}

[[nodiscard]] inline std::vector<std::string_view> splitActionPayload(std::string_view payload, char delimiter) {
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

[[nodiscard]] inline std::string px(float value) {
    std::ostringstream out;
    out << static_cast<int>(std::lround(value)) << "px";
    return out.str();
}

[[nodiscard]] inline std::string style(std::initializer_list<std::pair<std::string_view, std::string_view>> declarations) {
    std::string out;
    for (const auto& [name, value] : declarations) {
        out += std::string(name);
        out += ":";
        out += value;
        out += ";";
    }
    return out;
}

} // namespace skui
