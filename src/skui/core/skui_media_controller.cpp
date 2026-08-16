#include "skui_media_controller.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace skui {
namespace {

bool hasAttribute(const Node& node, std::string_view name) {
    return node.attributes.contains(std::string(name));
}

std::string attributeValue(const Node& node, std::string_view name) {
    const auto it = node.attributes.find(std::string(name));
    return it == node.attributes.end() ? std::string{} : it->second;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return value;
}

bool isExternalSource(std::string_view source) {
    const size_t scheme = source.find(':');
    if (scheme == std::string_view::npos || scheme == 1) {
        return false;
    }
    return source.find_first_of("/\\") == std::string_view::npos ||
           source.find("://") != std::string_view::npos;
}

bool isMediaNode(const Node& node) {
    return node.tag == "audio" || node.tag == "video";
}

}  // namespace

MediaController::MediaController(const RuntimeOptions& options)
    : assetRoot_(options.assetRoot),
      defaultPredecodeFrames_(std::max<size_t>(1, options.videoPredecodeFrames)),
      playerFactory_(options.mediaPlayerFactory),
      requestRedraw_(options.requestRedraw) {}

MediaController::~MediaController() {
    close();
}

void MediaController::sync(Document& document) {
    if (!document.root) {
        close();
        return;
    }

    std::unordered_map<const Node*, bool> liveNodes;
    syncNode(document, *document.root, liveNodes);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!liveNodes.contains(it->first)) {
            it->second.player->close();
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    publishPlaybackSnapshots();
}

void MediaController::close() {
    clearPlaybackSnapshots();
    for (auto& [node, playback] : entries_) {
        (void)node;
        playback.player->close();
    }
    entries_.clear();
}

bool MediaController::tick(double deltaSeconds) {
    bool changed = false;
    for (auto& [nodePointer, playback] : entries_) {
        (void)nodePointer;
        Node& node = *playback.node;
        changed = playback.player->tick(deltaSeconds) || changed;
        const sk_sp<SkImage> previousFrame = node.videoFrame;
        refreshNode(node, playback);
        publishPlaybackSnapshot(playback);
        changed = previousFrame != node.videoFrame || changed;
    }
    return changed;
}

bool MediaController::needsTicks() const {
    return std::any_of(entries_.begin(), entries_.end(), [](const auto& item) {
        return item.second.player->needsTicks();
    });
}

bool MediaController::consumeIntrinsicSizeChange() {
    return std::exchange(intrinsicSizeChanged_, false);
}

bool MediaController::prepare(Node& node) {
    Entry* playback = entry(node);
    if (!playback) {
        return false;
    }
    const bool accepted = playback->player->prepare();
    if (accepted) {
        playback->preloadMode = "auto";
    }
    publishPlaybackSnapshot(*playback);
    return accepted;
}

bool MediaController::play(Node& node) {
    Entry* playback = entry(node);
    if (!playback) {
        return false;
    }
    const bool accepted = playback->player->play();
    publishPlaybackSnapshot(*playback);
    return accepted;
}

bool MediaController::pause(Node& node) {
    Entry* playback = entry(node);
    if (!playback) {
        return false;
    }
    playback->player->pause();
    publishPlaybackSnapshot(*playback);
    return true;
}

bool MediaController::seek(Node& node, double seconds) {
    Entry* playback = entry(node);
    if (!playback) {
        return false;
    }
    const bool accepted = playback->player->seek(seconds);
    publishPlaybackSnapshot(*playback);
    return accepted;
}

bool MediaController::setMuted(Node& node, bool muted) {
    Entry* playback = entry(node);
    if (!playback) {
        return false;
    }
    playback->muted = muted;
    playback->player->setMuted(muted);
    publishPlaybackSnapshot(*playback);
    return true;
}

std::optional<MediaPlaybackState> MediaController::stateById(
    std::string_view id,
    std::string_view expectedTag) const {
    std::shared_lock snapshotsLock(playbackSnapshotsMutex_);
    const auto snapshot = playbackSnapshots_.find(id);
    if (snapshot == playbackSnapshots_.end() || snapshot->second.tag != expectedTag) {
        return std::nullopt;
    }
    return snapshot->second.state;
}

void MediaController::syncNode(Document& document,
                               Node& node,
                               std::unordered_map<const Node*, bool>& liveNodes) {
    if (isMediaNode(node) && !node.src.empty() && playerFactory_) {
        liveNodes.emplace(&node, true);
        const std::string source = resolveSource(document, node.src);
        const size_t frameCount = predecodeFrames(node);
        const bool loop = hasAttribute(node, "loop");
        const bool muted = hasAttribute(node, "muted");

        auto [it, inserted] = entries_.try_emplace(&node);
        Entry& playback = it->second;
        if (inserted) {
            playback.node = &node;
            playback.player = playerFactory_(MediaPlayerCreateOptions{requestRedraw_});
        }
        if (!playback.player) {
            entries_.erase(it);
            liveNodes.erase(&node);
        } else {
            const bool sourceChanged = playback.source != source ||
                                       playback.predecodeFrames != frameCount;
            if (sourceChanged) {
                playback.source = source;
                playback.predecodeFrames = frameCount;
                playback.loop = loop;
                playback.muted = muted;
                playback.preloadMode.clear();
                playback.autoplayStarted = false;
                playback.player->setSource(MediaSourceOptions{
                    source,
                    frameCount,
                    loop,
                    muted,
                    node.tag == "video",
                });
            } else {
                if (playback.loop != loop) {
                    playback.loop = loop;
                    playback.player->setLoop(loop);
                }
                if (playback.muted != muted) {
                    playback.muted = muted;
                    playback.player->setMuted(muted);
                }
            }

            const std::string preload = lowerAscii(attributeValue(node, "preload"));
            if (playback.preloadMode != preload) {
                bool accepted = true;
                if (preload == "auto") {
                    accepted = playback.player->prepare();
                } else if (preload == "metadata") {
                    accepted = playback.player->loadMetadata();
                }
                if (accepted) {
                    playback.preloadMode = preload;
                }
            }
            if (!playback.autoplayStarted && hasAttribute(node, "autoplay")) {
                playback.autoplayStarted = playback.player->play();
            }
            refreshNode(node, playback);
        }
    }

    for (auto& child : node.children) {
        syncNode(document, *child, liveNodes);
    }
}

void MediaController::refreshNode(Node& node, Entry& playback) {
    node.videoFrame = playback.player->currentFrame();
    const MediaPlaybackState state = playback.player->state();
    intrinsicSizeChanged_ =
        intrinsicSizeChanged_ || node.videoFrameWidth != state.videoWidth ||
        node.videoFrameHeight != state.videoHeight;
    node.videoFrameWidth = state.videoWidth;
    node.videoFrameHeight = state.videoHeight;
}

std::string MediaController::resolveSource(const Document& document,
                                           std::string_view source) const {
    namespace fs = std::filesystem;
    if (source.empty() || isExternalSource(source)) {
        return std::string(source);
    }

    const fs::path path = pathFromUtf8(source);
    if (path.is_absolute()) {
        return pathToUtf8(path);
    }
    if (!document.basePath.empty()) {
        const fs::path candidate = pathFromUtf8(document.basePath) / path;
        if (fs::exists(candidate)) {
            return pathToUtf8(candidate);
        }
    }
    if (!assetRoot_.empty()) {
        const fs::path candidate = pathFromUtf8(assetRoot_) / path;
        if (fs::exists(candidate)) {
            return pathToUtf8(candidate);
        }
    }
    return pathToUtf8(path);
}

size_t MediaController::predecodeFrames(const Node& node) const {
    const std::string value = attributeValue(node, "data-predecode-frames");
    if (value.empty()) {
        return defaultPredecodeFrames_;
    }
    size_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return defaultPredecodeFrames_;
    }
    return std::clamp<size_t>(parsed, 1, 100);
}

MediaController::Entry* MediaController::entry(Node& node) {
    const auto it = entries_.find(&node);
    return it == entries_.end() ? nullptr : &it->second;
}

const MediaController::Entry* MediaController::entry(const Node& node) const {
    const auto it = entries_.find(&node);
    return it == entries_.end() ? nullptr : &it->second;
}

void MediaController::publishPlaybackSnapshot(const Entry& playback) {
    if (playback.node->id.empty()) {
        return;
    }
    PlaybackSnapshot snapshot{playback.node->tag, playback.player->state()};

    std::unique_lock snapshotsLock(playbackSnapshotsMutex_);
    playbackSnapshots_.insert_or_assign(playback.node->id, std::move(snapshot));
}

void MediaController::publishPlaybackSnapshots() {
    std::unordered_set<std::string> liveIds;
    liveIds.reserve(entries_.size());
    for (const auto& [node, playback] : entries_) {
        (void)node;
        publishPlaybackSnapshot(playback);
        if (!playback.node->id.empty()) {
            liveIds.insert(playback.node->id);
        }
    }

    std::unique_lock snapshotsLock(playbackSnapshotsMutex_);
    std::erase_if(playbackSnapshots_, [&liveIds](const auto& item) {
        return !liveIds.contains(item.first);
    });
}

void MediaController::clearPlaybackSnapshots() {
    std::unique_lock snapshotsLock(playbackSnapshotsMutex_);
    playbackSnapshots_.clear();
}

}  // namespace skui
