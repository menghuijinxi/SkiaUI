#pragma once

#include "skui_internal.h"

#include <memory>
#include <shared_mutex>
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
    [[nodiscard]] std::optional<MediaPlaybackState> stateById(
        std::string_view id,
        std::string_view expectedTag) const;

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

    struct PlaybackSnapshot {
        std::string tag;
        MediaPlaybackState state;
    };

    struct TransparentStringHash {
        using is_transparent = void;

        size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    using PlaybackSnapshotMap = std::unordered_map<
        std::string,
        PlaybackSnapshot,
        TransparentStringHash,
        std::equal_to<>>;

    void syncNode(Document& document,
                  Node& node,
                  std::unordered_map<const Node*, bool>& liveNodes);
    void refreshNode(Node& node, Entry& entry);
    [[nodiscard]] std::string resolveSource(const Document& document,
                                            std::string_view source) const;
    [[nodiscard]] size_t predecodeFrames(const Node& node) const;
    [[nodiscard]] Entry* entry(Node& node);
    [[nodiscard]] const Entry* entry(const Node& node) const;
    void publishPlaybackSnapshot(const Entry& entry);
    void publishPlaybackSnapshots();
    void clearPlaybackSnapshots();

    std::string assetRoot_;
    size_t defaultPredecodeFrames_ = 3;
    MediaPlayerFactory playerFactory_;
    std::function<void()> requestRedraw_;
    std::unordered_map<const Node*, Entry> entries_;
    mutable std::shared_mutex playbackSnapshotsMutex_;
    PlaybackSnapshotMap playbackSnapshots_;
    bool intrinsicSizeChanged_ = false;
};

}  // namespace skui
