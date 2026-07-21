#pragma once

#include "skui_internal.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace skui {

class MediaController {
public:
    explicit MediaController(const RuntimeOptions& options);
    ~MediaController();

    MediaController(const MediaController&) = delete;
    MediaController& operator=(const MediaController&) = delete;

    void sync(Document& document);
    void close();
    [[nodiscard]] bool tick(double deltaSeconds);
    [[nodiscard]] bool needsTicks() const;
    bool consumeIntrinsicSizeChange();

    bool prepare(Node& node);
    bool play(Node& node);
    bool pause(Node& node);
    bool seek(Node& node, double seconds);
    bool setMuted(Node& node, bool muted);
    [[nodiscard]] std::optional<MediaPlaybackState> state(const Node& node) const;

private:
    struct Entry {
        Node* node = nullptr;
        std::unique_ptr<MediaPlayer> player;
        std::string source;
        size_t predecodeFrames = 0;
        std::string preloadMode;
        bool loop = false;
        bool muted = false;
        bool autoplayStarted = false;
    };

    void syncNode(Document& document,
                  Node& node,
                  std::unordered_map<const Node*, bool>& liveNodes);
    void refreshNode(Node& node, Entry& entry);
    [[nodiscard]] std::string resolveSource(const Document& document,
                                            std::string_view source) const;
    [[nodiscard]] size_t predecodeFrames(const Node& node) const;
    [[nodiscard]] Entry* entry(Node& node);
    [[nodiscard]] const Entry* entry(const Node& node) const;

    std::string assetRoot_;
    size_t defaultPredecodeFrames_ = 3;
    MediaPlayerFactory playerFactory_;
    std::function<void()> requestRedraw_;
    std::unordered_map<const Node*, Entry> entries_;
    bool intrinsicSizeChanged_ = false;
};

}  // namespace skui
