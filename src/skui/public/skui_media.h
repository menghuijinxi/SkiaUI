#pragma once

#include "include/core/SkImage.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace skui {

enum class MediaReadyState {
    Idle,
    Opening,
    Prebuffering,
    Ready,
    Playing,
    Paused,
    Rebuffering,
    Ended,
    Failed
};

struct MediaSourceOptions {
    std::string source;
    size_t predecodeFrames = 3;
    bool loop = false;
    bool muted = false;
};

struct MediaPlaybackState {
    MediaReadyState readyState = MediaReadyState::Idle;
    double currentSeconds = 0.0;
    double durationSeconds = 0.0;
    int videoWidth = 0;
    int videoHeight = 0;
    size_t bufferedVideoFrames = 0;
    double bufferedAudioSeconds = 0.0;
    uint64_t displayedVideoFrames = 0;
    uint64_t droppedVideoFrames = 0;
    uint64_t audioUnderruns = 0;
    bool hasAudio = false;
    bool hasAlpha = false;
    std::string decoderName;
    std::string error;
};

struct AudioOutputFormat {
    int sampleRate = 48000;
    int channelCount = 2;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // 所有方法都必须可跨解码线程和 UI 线程安全调用。write 不阻塞并返回接收的帧数。
    virtual bool configure(const AudioOutputFormat& format,
                           double bufferSeconds) = 0;
    virtual size_t write(std::span<const float> interleavedSamples) = 0;
    virtual bool start() = 0;
    virtual void pause() = 0;
    virtual void flush() = 0;
    virtual void stop() = 0;
    virtual void setMuted(bool muted) = 0;
    [[nodiscard]] virtual AudioOutputFormat format() const = 0;
    // playedFrames 从最近一次 configure 或 flush 起计数，并反映设备实际消费进度。
    [[nodiscard]] virtual uint64_t playedFrames() const = 0;
    [[nodiscard]] virtual size_t bufferedFrames() const = 0;
    [[nodiscard]] virtual size_t capacityFrames() const = 0;
    [[nodiscard]] virtual std::string lastError() const = 0;
};

using AudioOutputFactory = std::function<std::unique_ptr<AudioOutput>()>;

struct MediaPlayerCreateOptions {
    std::function<void()> requestRedraw;
};

class MediaPlayer {
public:
    virtual ~MediaPlayer() = default;

    virtual bool setSource(const MediaSourceOptions& options) = 0;
    virtual void setLoop(bool loop) = 0;
    virtual void setMuted(bool muted) = 0;
    virtual bool loadMetadata() = 0;
    virtual bool prepare() = 0;
    virtual bool play() = 0;
    virtual void pause() = 0;
    virtual bool seek(double seconds) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool tick(double deltaSeconds) = 0;
    [[nodiscard]] virtual bool needsTicks() const = 0;
    [[nodiscard]] virtual sk_sp<SkImage> currentFrame() const = 0;
    [[nodiscard]] virtual MediaPlaybackState state() const = 0;
};

using MediaPlayerFactory =
    std::function<std::unique_ptr<MediaPlayer>(MediaPlayerCreateOptions)>;

}  // namespace skui
