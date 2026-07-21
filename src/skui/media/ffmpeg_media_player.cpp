#include "skui_ffmpeg.h"

#include "ffmpeg_decoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace skui::ffmpeg {
namespace {

constexpr AudioOutputFormat kRequestedAudioFormat{48000, 2};
constexpr double kAudioBufferSeconds = 2.0;
constexpr double kMinimumPrebufferAudioSeconds = 0.05;
constexpr double kMaximumPrebufferAudioSeconds = 0.25;
constexpr double kLoopHeadMaximumSeconds = 0.5;
constexpr size_t kPrebufferFrameSlack = 8;
constexpr size_t kLoopHeadMaximumVideoFrames = 8;

enum class LoadRequest {
    None,
    Metadata,
    Prepare,
    Play,
};

struct AudioSnapshot {
    bool enabled = false;
    AudioOutputFormat format;
    uint64_t playedFrames = 0;
    size_t bufferedFrames = 0;
    size_t capacityFrames = 0;
};

struct LoopHeadCache {
    std::deque<detail::DecodedVideoFrame> videoFrames;
    std::vector<float> audioSamples;
    double coverageSeconds = 0.0;
};

class FfmpegMediaPlayer final : public MediaPlayer {
public:
    FfmpegMediaPlayer(MediaPlayerCreateOptions options,
                      AudioOutputFactory audioOutputFactory)
        : requestRedraw_(std::move(options.requestRedraw)),
          audioOutputFactory_(std::move(audioOutputFactory)) {}

    ~FfmpegMediaPlayer() override {
        close();
    }

    bool setSource(const MediaSourceOptions& options) override {
        close();
        if (options.source.empty()) {
            return false;
        }

        std::lock_guard lock(stateMutex_);
        sourceOptions_ = options;
        sourceOptions_.predecodeFrames =
            std::clamp<size_t>(options.predecodeFrames, 1, 100);
        playbackState_ = {};
        playbackState_.readyState = MediaReadyState::Idle;
        pausedTimelineSeconds_ = 0.0;
        return true;
    }

    void setLoop(bool loop) override {
        std::lock_guard lock(stateMutex_);
        sourceOptions_.loop = loop;
        wakeDecoder_.notify_all();
    }

    void setMuted(bool muted) override {
        {
            std::lock_guard lock(stateMutex_);
            sourceOptions_.muted = muted;
        }
        std::lock_guard audioLock(audioMutex_);
        if (audioOutput_) {
            audioOutput_->setMuted(muted);
        }
    }

    bool loadMetadata() override {
        return requestLoad(LoadRequest::Metadata, false);
    }

    bool prepare() override {
        return requestLoad(LoadRequest::Prepare, true);
    }

    bool play() override {
        bool replayFromStart = false;
        {
            std::lock_guard lock(stateMutex_);
            replayFromStart =
                playbackState_.readyState == MediaReadyState::Ended;
        }
        if (replayFromStart && !seek(0.0)) {
            return false;
        }
        const bool accepted = requestLoad(LoadRequest::Play, false);
        if (accepted) {
            requestRedraw();
        }
        return accepted;
    }

    void pause() override {
        const double timelineSeconds = playbackClockSeconds();
        {
            std::lock_guard audioLock(audioMutex_);
            if (audioOutput_) {
                audioOutput_->pause();
            }
        }

        std::lock_guard lock(stateMutex_);
        pausedTimelineSeconds_ = timelineSeconds;
        playbackActive_ = false;
        playRequested_ = false;
        if (playbackState_.readyState != MediaReadyState::Failed &&
            playbackState_.readyState != MediaReadyState::Ended) {
            playbackState_.readyState = MediaReadyState::Paused;
        }
    }

    bool seek(double seconds) override {
        if (!std::isfinite(seconds) || seconds < 0.0) {
            return false;
        }
        commandSerial_.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard audioLock(audioMutex_);
            if (audioOutput_) {
                audioOutput_->pause();
                audioOutput_->flush();
            }
        }

        {
            std::lock_guard lock(stateMutex_);
            if (sourceOptions_.source.empty()) {
                return false;
            }
            const double target =
                playbackState_.durationSeconds > 0.0
                    ? std::min(seconds, playbackState_.durationSeconds)
                    : seconds;
            pendingSeekSeconds_ = target;
            if (requestedLoad_ == LoadRequest::None ||
                requestedLoad_ == LoadRequest::Metadata) {
                requestedLoad_ = LoadRequest::Prepare;
            }
            pausedTimelineSeconds_ = target;
            playbackBaseSeconds_ = target;
            playbackActive_ = false;
            bufferReady_ = false;
            decodeEnded_ = false;
            currentFrame_.reset();
            videoFrames_.clear();
            playbackState_.currentSeconds = target;
            playbackState_.bufferedVideoFrames = 0;
            playbackState_.readyState = MediaReadyState::Prebuffering;
        }
        ensureWorkerStarted();
        wakeDecoder_.notify_all();
        requestRedraw();
        return true;
    }

    void close() override {
        cancelRequested_.store(true, std::memory_order_relaxed);
        commandSerial_.fetch_add(1, std::memory_order_relaxed);
        wakeDecoder_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }

        {
            std::lock_guard audioLock(audioMutex_);
            if (audioOutput_) {
                audioOutput_->stop();
                audioOutput_.reset();
            }
        }

        std::lock_guard lock(stateMutex_);
        videoFrames_.clear();
        currentFrame_.reset();
        requestedLoad_ = LoadRequest::None;
        opened_ = false;
        explicitPrepareRequested_ = false;
        playRequested_ = false;
        playbackActive_ = false;
        bufferReady_ = false;
        decodeEnded_ = false;
        pendingSeekSeconds_.reset();
        playbackState_.bufferedVideoFrames = 0;
        playbackState_.bufferedAudioSeconds = 0.0;
        if (playbackState_.readyState != MediaReadyState::Failed) {
            playbackState_.readyState = MediaReadyState::Idle;
        }
        cancelRequested_.store(false, std::memory_order_relaxed);
    }

    bool tick(double deltaSeconds) override {
        (void)deltaSeconds;
        bool changed = activatePlaybackIfReady();
        const AudioSnapshot audio = audioSnapshot();
        bool shouldStopAudio = false;
        bool shouldPauseForRebuffer = false;
        bool consumedFrame = false;

        {
            std::lock_guard lock(stateMutex_);
            if (!playbackActive_) {
                updateBufferStateLocked(audio);
                return changed;
            }

            const double clockSeconds = playbackClockSecondsLocked(audio);
            size_t consumedCount = 0;
            while (!videoFrames_.empty() &&
                   videoFrames_.front().presentationSeconds <= clockSeconds + 0.0005) {
                currentFrame_ = videoFrames_.front().image;
                lastDisplayedEndSeconds_ =
                    videoFrames_.front().presentationSeconds +
                    videoFrames_.front().durationSeconds;
                videoFrames_.pop_front();
                ++consumedCount;
            }
            if (consumedCount > 0) {
                playbackState_.displayedVideoFrames += 1;
                if (consumedCount > 1) {
                    playbackState_.droppedVideoFrames += consumedCount - 1;
                }
                consumedFrame = true;
                changed = true;
            }

            playbackState_.currentSeconds = displayClockSecondsLocked(clockSeconds);
            updateBufferStateLocked(audio);

            const bool audioEmpty = audio.enabled && audio.bufferedFrames == 0;
            const bool videoExpired = videoFrames_.empty() && !decodeEnded_ &&
                                      clockSeconds >= lastDecodedEndSeconds_;
            const bool underrun = !decodeEnded_ && (audioEmpty || videoExpired);
            if (underrun && !underrunActive_) {
                ++playbackState_.audioUnderruns;
            }
            underrunActive_ = underrun;
            if (underrun) {
                pausedTimelineSeconds_ = clockSeconds;
                playbackActive_ = false;
                bufferReady_ = false;
                playbackState_.readyState = MediaReadyState::Rebuffering;
                shouldPauseForRebuffer = audio.enabled;
                changed = true;
            } else {
                playbackState_.readyState = MediaReadyState::Playing;
            }

            const double endSeconds = playbackState_.durationSeconds > 0.0
                                          ? playbackState_.durationSeconds
                                          : lastDecodedEndSeconds_;
            if (decodeEnded_ && videoFrames_.empty() &&
                (!audio.enabled || audio.bufferedFrames == 0) &&
                clockSeconds >= endSeconds) {
                playbackActive_ = false;
                playRequested_ = false;
                pausedTimelineSeconds_ = endSeconds;
                playbackState_.currentSeconds = endSeconds;
                playbackState_.readyState = MediaReadyState::Ended;
                shouldStopAudio = audio.enabled;
                changed = true;
            }
        }

        if (consumedFrame || shouldPauseForRebuffer) {
            wakeDecoder_.notify_all();
        }
        if (shouldStopAudio || shouldPauseForRebuffer) {
            std::lock_guard audioLock(audioMutex_);
            if (audioOutput_) {
                audioOutput_->pause();
            }
        }
        return changed;
    }

    [[nodiscard]] bool needsTicks() const override {
        std::lock_guard lock(stateMutex_);
        switch (playbackState_.readyState) {
        case MediaReadyState::Opening:
        case MediaReadyState::Prebuffering:
        case MediaReadyState::Playing:
        case MediaReadyState::Rebuffering:
            return true;
        default:
            return playRequested_;
        }
    }

    [[nodiscard]] sk_sp<SkImage> currentFrame() const override {
        std::lock_guard lock(stateMutex_);
        if (currentFrame_) {
            return currentFrame_;
        }
        return videoFrames_.empty() ? nullptr : videoFrames_.front().image;
    }

    [[nodiscard]] MediaPlaybackState state() const override {
        const AudioSnapshot audio = audioSnapshot();
        std::lock_guard lock(stateMutex_);
        MediaPlaybackState result = playbackState_;
        result.bufferedVideoFrames = videoFrames_.size();
        if (audio.enabled && audio.format.sampleRate > 0) {
            result.bufferedAudioSeconds =
                static_cast<double>(audio.bufferedFrames) / audio.format.sampleRate;
        }
        return result;
    }

private:
    bool requestLoad(LoadRequest request, bool explicitPrepare) {
        {
            std::lock_guard lock(stateMutex_);
            if (sourceOptions_.source.empty()) {
                return false;
            }
            if (playbackState_.readyState == MediaReadyState::Failed) {
                playbackState_.error.clear();
            }
            if (static_cast<int>(request) > static_cast<int>(requestedLoad_)) {
                requestedLoad_ = request;
            }
            explicitPrepareRequested_ =
                explicitPrepareRequested_ || explicitPrepare;
            if (request == LoadRequest::Play) {
                playRequested_ = true;
            }
            if (!opened_) {
                playbackState_.readyState = MediaReadyState::Opening;
            } else if (request != LoadRequest::Metadata && !bufferReady_) {
                playbackState_.readyState = MediaReadyState::Prebuffering;
            }
        }
        ensureWorkerStarted();
        wakeDecoder_.notify_all();
        return true;
    }

    void ensureWorkerStarted() {
        std::lock_guard lock(workerMutex_);
        if (worker_.joinable()) {
            return;
        }
        cancelRequested_.store(false, std::memory_order_relaxed);
        worker_ = std::jthread([this] {
            decoderMain();
        });
    }

    void decoderMain() {
        std::string source;
        {
            std::lock_guard lock(stateMutex_);
            source = sourceOptions_.source;
        }

        detail::DecoderSession decoder(cancelRequested_);
        std::string error;
        if (!decoder.open(source, error)) {
            fail(std::move(error));
            return;
        }

        if (!configureAudio(decoder, error)) {
            fail(std::move(error));
            return;
        }
        publishMetadata(decoder.metadata());

        double decodeSegmentStartSeconds = 0.0;
        double loopOffsetSeconds = 0.0;
        double nextQueuedAudioSeconds = 0.0;
        LoopHeadCache loopHead;
        const double frameBoundedLoopHeadSeconds = std::max(
            decoder.metadata().frameDurationSeconds,
            decoder.metadata().frameDurationSeconds *
                kLoopHeadMaximumVideoFrames);
        loopHead.coverageSeconds = std::min(
            {kLoopHeadMaximumSeconds,
             frameBoundedLoopHeadSeconds,
             decoder.metadata().durationSeconds > 0.0
                 ? decoder.metadata().durationSeconds
                 : kLoopHeadMaximumSeconds});
        while (!cancelRequested_.load(std::memory_order_relaxed)) {
            std::optional<double> seekTarget;
            uint64_t commandSerial = 0;
            {
                std::unique_lock lock(stateMutex_);
                wakeDecoder_.wait(lock, [this] {
                    return cancelRequested_.load(std::memory_order_relaxed) ||
                           pendingSeekSeconds_.has_value() || shouldDecodeLocked();
                });
                if (cancelRequested_.load(std::memory_order_relaxed)) {
                    return;
                }
                seekTarget = pendingSeekSeconds_;
                pendingSeekSeconds_.reset();
                commandSerial = commandSerial_.load(std::memory_order_relaxed);
            }

            if (seekTarget) {
                if (!decoder.seek(*seekTarget, error)) {
                    fail(std::move(error));
                    return;
                }
                decodeSegmentStartSeconds = *seekTarget;
                loopOffsetSeconds = 0.0;
                nextQueuedAudioSeconds = *seekTarget;
                {
                    std::lock_guard lock(stateMutex_);
                    decodeEnded_ = false;
                    lastDecodedEndSeconds_ = *seekTarget;
                }
            }

            detail::DecodeBatch batch;
            const detail::DecodeStatus status = decoder.decodeNext(batch, error);
            if (status == detail::DecodeStatus::Interrupted) {
                return;
            }
            if (status == detail::DecodeStatus::Failed) {
                fail(std::move(error));
                return;
            }
            if (status == detail::DecodeStatus::EndOfStream) {
                if (restartLoop(decoder,
                                decodeSegmentStartSeconds,
                                loopOffsetSeconds,
                                nextQueuedAudioSeconds,
                                loopHead,
                                commandSerial,
                                error)) {
                    continue;
                }
                if (!error.empty()) {
                    fail(std::move(error));
                    return;
                }
                const AudioSnapshot audio = audioSnapshot();
                {
                    std::lock_guard lock(stateMutex_);
                    decodeEnded_ = true;
                    refreshBufferReadyLocked(audio);
                }
                requestRedraw();
                wakeDecoder_.notify_all();
                continue;
            }

            if (!processAudio(batch,
                              decodeSegmentStartSeconds,
                              loopOffsetSeconds,
                              nextQueuedAudioSeconds,
                              loopHead,
                              commandSerial)) {
                continue;
            }
            publishVideoFrames(batch,
                               decodeSegmentStartSeconds,
                               loopOffsetSeconds,
                               loopHead);
        }
    }

    bool configureAudio(detail::DecoderSession& decoder, std::string& error) {
        if (!decoder.metadata().hasAudio || !audioOutputFactory_) {
            return true;
        }
        std::unique_ptr<AudioOutput> output = audioOutputFactory_();
        if (!output) {
            error = "audio output factory returned no output device";
            return false;
        }
        if (!output->configure(kRequestedAudioFormat, kAudioBufferSeconds)) {
            error = "audio output configuration failed: " + output->lastError();
            return false;
        }
        bool muted = false;
        {
            std::lock_guard lock(stateMutex_);
            muted = sourceOptions_.muted;
        }
        output->setMuted(muted);
        const AudioOutputFormat actualFormat = output->format();
        if (!decoder.configureAudio(actualFormat, error)) {
            return false;
        }

        std::lock_guard audioLock(audioMutex_);
        audioOutput_ = std::move(output);
        return true;
    }

    void publishMetadata(const detail::StreamMetadata& metadata) {
        {
            std::lock_guard lock(stateMutex_);
            playbackState_.durationSeconds = metadata.durationSeconds;
            playbackState_.videoWidth = metadata.videoWidth;
            playbackState_.videoHeight = metadata.videoHeight;
            playbackState_.hasAudio = metadata.hasAudio;
            playbackState_.hasAlpha = metadata.hasAlpha;
            playbackState_.decoderName = metadata.videoDecoderName;
            frameDurationSeconds_ = metadata.frameDurationSeconds;
            opened_ = true;
            if (requestedLoad_ == LoadRequest::Metadata) {
                playbackState_.readyState = MediaReadyState::Ready;
            } else {
                playbackState_.readyState = MediaReadyState::Prebuffering;
            }
        }
        requestRedraw();
    }

    bool processAudio(const detail::DecodeBatch& batch,
                      double segmentStartSeconds,
                      double loopOffsetSeconds,
                      double& nextQueuedAudioSeconds,
                      LoopHeadCache& loopHead,
                      uint64_t commandSerial) {
        const AudioSnapshot audio = audioSnapshot();
        if (!audio.enabled) {
            return true;
        }

        for (const detail::DecodedAudioChunk& chunk : batch.audioChunks) {
            if (loopOffsetSeconds == 0.0 && segmentStartSeconds == 0.0) {
                cacheLoopHeadAudio(chunk, audio.format, loopHead);
            }
            const double presentationSeconds =
                chunk.presentationSeconds + loopOffsetSeconds;
            if (!writeTimedAudioSamples(chunk.samples,
                                        presentationSeconds,
                                        audio.format,
                                        nextQueuedAudioSeconds,
                                        commandSerial)) {
                return false;
            }
        }
        return true;
    }

    void publishVideoFrames(const detail::DecodeBatch& batch,
                            double segmentStartSeconds,
                            double loopOffsetSeconds,
                            LoopHeadCache& loopHead) {
        const AudioSnapshot audio = audioSnapshot();
        bool addedFrame = false;
        {
            std::lock_guard lock(stateMutex_);
            for (const detail::DecodedVideoFrame& decoded : batch.videoFrames) {
                const double frameEnd =
                    decoded.presentationSeconds + decoded.durationSeconds;
                if (loopOffsetSeconds == 0.0 && segmentStartSeconds == 0.0 &&
                    decoded.presentationSeconds < loopHead.coverageSeconds &&
                    loopHead.videoFrames.size() < kLoopHeadMaximumVideoFrames) {
                    loopHead.videoFrames.push_back(decoded);
                }
                if (frameEnd + 0.0005 < segmentStartSeconds) {
                    continue;
                }
                detail::DecodedVideoFrame frame = decoded;
                frame.presentationSeconds += loopOffsetSeconds;
                videoFrames_.push_back(std::move(frame));
                lastDecodedEndSeconds_ = std::max(
                    lastDecodedEndSeconds_,
                    frameEnd + loopOffsetSeconds);
                addedFrame = true;
            }
            playbackState_.hasAlpha =
                playbackState_.hasAlpha || decoderFrameHasAlpha(batch);
            playbackState_.bufferedVideoFrames = videoFrames_.size();
            refreshBufferReadyLocked(audio);
        }
        if (addedFrame) {
            requestRedraw();
        }
    }

    bool restartLoop(detail::DecoderSession& decoder,
                     double& segmentStartSeconds,
                     double& loopOffsetSeconds,
                     double& nextQueuedAudioSeconds,
                     const LoopHeadCache& loopHead,
                     uint64_t commandSerial,
                     std::string& error) {
        double durationSeconds = 0.0;
        bool loop = false;
        {
            std::lock_guard lock(stateMutex_);
            loop = sourceOptions_.loop;
            durationSeconds = playbackState_.durationSeconds > 0.0
                                  ? playbackState_.durationSeconds
                                  : lastDecodedEndSeconds_ - loopOffsetSeconds;
        }
        if (!loop || durationSeconds <= 0.0) {
            return false;
        }
        const double nextLoopOffsetSeconds = loopOffsetSeconds + durationSeconds;
        double cachedCoverageSeconds = 0.0;
        if (!loopHead.videoFrames.empty()) {
            const detail::DecodedVideoFrame& last = loopHead.videoFrames.back();
            cachedCoverageSeconds = std::min(
                loopHead.coverageSeconds,
                last.presentationSeconds + last.durationSeconds);
        }
        const AudioSnapshot audio = audioSnapshot();
        if (audio.enabled && !loopHead.audioSamples.empty() &&
            cachedCoverageSeconds > 0.0) {
            const size_t cachedAudioFrames = std::min(
                loopHead.audioSamples.size() /
                    static_cast<size_t>(audio.format.channelCount),
                static_cast<size_t>(std::ceil(
                    cachedCoverageSeconds * audio.format.sampleRate)));
            const std::span<const float> cachedAudio(
                loopHead.audioSamples.data(),
                cachedAudioFrames * audio.format.channelCount);
            if (!writeTimedAudioSamples(cachedAudio,
                                    nextLoopOffsetSeconds,
                                    audio.format,
                                    nextQueuedAudioSeconds,
                                    commandSerial)) {
                return true;
            }
        }

        bool injectedVideo = false;
        {
            std::lock_guard lock(stateMutex_);
            for (const detail::DecodedVideoFrame& cached : loopHead.videoFrames) {
                detail::DecodedVideoFrame frame = cached;
                frame.presentationSeconds += nextLoopOffsetSeconds;
                lastDecodedEndSeconds_ = std::max(
                    lastDecodedEndSeconds_,
                    frame.presentationSeconds + frame.durationSeconds);
                videoFrames_.push_back(std::move(frame));
                injectedVideo = true;
            }
            playbackState_.bufferedVideoFrames = videoFrames_.size();
        }
        if (injectedVideo) {
            requestRedraw();
        }
        if (!decoder.seek(0.0, error)) {
            return false;
        }
        segmentStartSeconds =
            injectedVideo ? cachedCoverageSeconds : 0.0;
        loopOffsetSeconds = nextLoopOffsetSeconds;
        return true;
    }

    void cacheLoopHeadAudio(const detail::DecodedAudioChunk& chunk,
                            const AudioOutputFormat& format,
                            LoopHeadCache& loopHead) const {
        if (loopHead.coverageSeconds <= 0.0 || format.sampleRate <= 0 ||
            format.channelCount <= 0) {
            return;
        }
        const size_t channelCount = static_cast<size_t>(format.channelCount);
        const size_t targetFrames = static_cast<size_t>(
            std::ceil(loopHead.coverageSeconds * format.sampleRate));
        if (loopHead.audioSamples.empty()) {
            loopHead.audioSamples.assign(targetFrames * channelCount, 0.0f);
        }

        const int64_t timelineStart = static_cast<int64_t>(std::llround(
            chunk.presentationSeconds * format.sampleRate));
        const size_t sourceStart = timelineStart < 0
                                       ? static_cast<size_t>(-timelineStart)
                                       : 0;
        const size_t destinationStart = timelineStart > 0
                                            ? static_cast<size_t>(timelineStart)
                                            : 0;
        if (sourceStart >= static_cast<size_t>(chunk.frameCount) ||
            destinationStart >= targetFrames) {
            return;
        }
        const size_t framesToCopy = std::min({
            static_cast<size_t>(chunk.frameCount) - sourceStart,
            targetFrames - destinationStart,
            chunk.samples.size() / channelCount - sourceStart,
        });
        std::copy_n(
            chunk.samples.data() + sourceStart * channelCount,
            framesToCopy * channelCount,
            loopHead.audioSamples.data() + destinationStart * channelCount);
    }

    bool writeAudioSamples(std::span<const float> samples,
                           const AudioOutputFormat& format,
                           uint64_t commandSerial) {
        const size_t channelCount = static_cast<size_t>(format.channelCount);
        size_t writtenSamples = 0;
        while (writtenSamples < samples.size()) {
            if (cancelRequested_.load(std::memory_order_relaxed) ||
                commandSerial_.load(std::memory_order_relaxed) != commandSerial) {
                return false;
            }
            size_t writtenFrames = 0;
            {
                std::lock_guard audioLock(audioMutex_);
                if (!audioOutput_) {
                    return false;
                }
                writtenFrames = audioOutput_->write(samples.subspan(writtenSamples));
            }
            if (writtenFrames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            writtenSamples += writtenFrames * channelCount;
        }
        return true;
    }

    bool writeTimedAudioSamples(std::span<const float> samples,
                                double presentationSeconds,
                                const AudioOutputFormat& format,
                                double& nextQueuedAudioSeconds,
                                uint64_t commandSerial) {
        const size_t channelCount = static_cast<size_t>(format.channelCount);
        const int64_t presentationFrame = static_cast<int64_t>(std::llround(
            presentationSeconds * format.sampleRate));
        int64_t queuedFrame = static_cast<int64_t>(std::llround(
            nextQueuedAudioSeconds * format.sampleRate));

        if (presentationFrame > queuedFrame) {
            size_t silenceFrames =
                static_cast<size_t>(presentationFrame - queuedFrame);
            constexpr size_t kSilenceBlockFrames = 1024;
            const std::vector<float> silence(
                kSilenceBlockFrames * channelCount, 0.0f);
            while (silenceFrames > 0) {
                const size_t blockFrames =
                    std::min(silenceFrames, kSilenceBlockFrames);
                if (!writeAudioSamples(
                        std::span<const float>(
                            silence.data(), blockFrames * channelCount),
                        format,
                        commandSerial)) {
                    return false;
                }
                queuedFrame += static_cast<int64_t>(blockFrames);
                silenceFrames -= blockFrames;
            }
        }

        const size_t availableFrames = samples.size() / channelCount;
        const size_t skippedFrames = queuedFrame > presentationFrame
                                         ? std::min<size_t>(
                                               queuedFrame - presentationFrame,
                                               availableFrames)
                                         : 0;
        const std::span<const float> remaining = samples.subspan(
            skippedFrames * channelCount);
        if (!writeAudioSamples(remaining, format, commandSerial)) {
            return false;
        }
        queuedFrame += static_cast<int64_t>(remaining.size() / channelCount);
        nextQueuedAudioSeconds =
            static_cast<double>(queuedFrame) / format.sampleRate;
        return true;
    }

    static bool decoderFrameHasAlpha(const detail::DecodeBatch& batch) {
        return std::any_of(batch.videoFrames.begin(),
                           batch.videoFrames.end(),
                           [](const detail::DecodedVideoFrame& frame) {
                               return frame.hasAlpha;
                           });
    }

    bool shouldDecodeLocked() const {
        if (!opened_ || decodeEnded_ || requestedLoad_ == LoadRequest::None ||
            requestedLoad_ == LoadRequest::Metadata) {
            return false;
        }
        const size_t target = targetVideoFramesLocked();
        const size_t maximum = bufferReady_ ? target : target + kPrebufferFrameSlack;
        return videoFrames_.size() < std::max<size_t>(1, maximum);
    }

    size_t targetVideoFramesLocked() const {
        return explicitPrepareRequested_ ? sourceOptions_.predecodeFrames : 1;
    }

    void refreshBufferReadyLocked(const AudioSnapshot& audio) {
        const size_t targetFrames = targetVideoFramesLocked();
        const bool videoReady = videoFrames_.size() >= targetFrames || decodeEnded_;
        bool audioReady = true;
        if (audio.enabled && audio.format.sampleRate > 0) {
            const double targetSeconds = std::clamp(
                static_cast<double>(targetFrames) * frameDurationSeconds_,
                kMinimumPrebufferAudioSeconds,
                kMaximumPrebufferAudioSeconds);
            const size_t targetAudioFrames = static_cast<size_t>(
                std::ceil(targetSeconds * audio.format.sampleRate));
            audioReady = audio.bufferedFrames >= targetAudioFrames || decodeEnded_;
        }
        bufferReady_ = videoReady && audioReady;
        if (!bufferReady_) {
            return;
        }
        if (playRequested_) {
            playbackState_.readyState = MediaReadyState::Prebuffering;
        } else {
            playbackState_.readyState = MediaReadyState::Ready;
        }
    }

    bool activatePlaybackIfReady() {
        const AudioSnapshot beforeStart = audioSnapshot();
        bool shouldStart = false;
        {
            std::lock_guard lock(stateMutex_);
            if (!playRequested_ || playbackActive_ || !bufferReady_) {
                return false;
            }
            playbackBaseSeconds_ = pausedTimelineSeconds_;
            audioPlayedOrigin_ = beforeStart.playedFrames;
            wallClockOrigin_ = std::chrono::steady_clock::now();
            playbackActive_ = true;
            underrunActive_ = false;
            playbackState_.readyState = MediaReadyState::Playing;
            shouldStart = beforeStart.enabled;
        }

        if (shouldStart) {
            bool started = false;
            std::string error;
            {
                std::lock_guard audioLock(audioMutex_);
                if (audioOutput_) {
                    started = audioOutput_->start();
                    error = audioOutput_->lastError();
                }
            }
            if (!started) {
                fail("audio output start failed: " + error);
                return true;
            }
        }
        return true;
    }

    double playbackClockSeconds() const {
        const AudioSnapshot audio = audioSnapshot();
        std::lock_guard lock(stateMutex_);
        return playbackClockSecondsLocked(audio);
    }

    double playbackClockSecondsLocked(const AudioSnapshot& audio) const {
        if (!playbackActive_) {
            return pausedTimelineSeconds_;
        }
        if (audio.enabled && audio.format.sampleRate > 0) {
            const uint64_t elapsedFrames =
                audio.playedFrames >= audioPlayedOrigin_
                    ? audio.playedFrames - audioPlayedOrigin_
                    : 0;
            return playbackBaseSeconds_ +
                   static_cast<double>(elapsedFrames) / audio.format.sampleRate;
        }
        const double elapsedSeconds = std::chrono::duration<double>(
                                          std::chrono::steady_clock::now() -
                                          wallClockOrigin_)
                                          .count();
        return playbackBaseSeconds_ + elapsedSeconds;
    }

    double displayClockSecondsLocked(double absoluteClockSeconds) const {
        const double durationSeconds = playbackState_.durationSeconds;
        if (!sourceOptions_.loop || durationSeconds <= 0.0) {
            return std::clamp(absoluteClockSeconds, 0.0, durationSeconds > 0.0
                                                              ? durationSeconds
                                                              : absoluteClockSeconds);
        }
        const double loopSeconds = std::fmod(absoluteClockSeconds, durationSeconds);
        return loopSeconds >= 0.0 ? loopSeconds : loopSeconds + durationSeconds;
    }

    AudioSnapshot audioSnapshot() const {
        std::lock_guard audioLock(audioMutex_);
        if (!audioOutput_) {
            return {};
        }
        return AudioSnapshot{
            true,
            audioOutput_->format(),
            audioOutput_->playedFrames(),
            audioOutput_->bufferedFrames(),
            audioOutput_->capacityFrames(),
        };
    }

    void updateBufferStateLocked(const AudioSnapshot& audio) {
        playbackState_.bufferedVideoFrames = videoFrames_.size();
        playbackState_.bufferedAudioSeconds =
            audio.enabled && audio.format.sampleRate > 0
                ? static_cast<double>(audio.bufferedFrames) / audio.format.sampleRate
                : 0.0;
    }

    void fail(std::string error) {
        {
            std::lock_guard lock(stateMutex_);
            playbackActive_ = false;
            playRequested_ = false;
            playbackState_.readyState = MediaReadyState::Failed;
            playbackState_.error = std::move(error);
        }
        {
            std::lock_guard audioLock(audioMutex_);
            if (audioOutput_) {
                audioOutput_->pause();
            }
        }
        requestRedraw();
    }

    void requestRedraw() const {
        if (requestRedraw_) {
            requestRedraw_();
        }
    }

    std::function<void()> requestRedraw_;
    AudioOutputFactory audioOutputFactory_;

    mutable std::mutex stateMutex_;
    std::condition_variable wakeDecoder_;
    MediaSourceOptions sourceOptions_;
    MediaPlaybackState playbackState_;
    std::deque<detail::DecodedVideoFrame> videoFrames_;
    sk_sp<SkImage> currentFrame_;
    LoadRequest requestedLoad_ = LoadRequest::None;
    std::optional<double> pendingSeekSeconds_;
    double frameDurationSeconds_ = 1.0 / 30.0;
    double pausedTimelineSeconds_ = 0.0;
    double playbackBaseSeconds_ = 0.0;
    double lastDecodedEndSeconds_ = 0.0;
    double lastDisplayedEndSeconds_ = 0.0;
    uint64_t audioPlayedOrigin_ = 0;
    std::chrono::steady_clock::time_point wallClockOrigin_{};
    bool opened_ = false;
    bool explicitPrepareRequested_ = false;
    bool playRequested_ = false;
    bool playbackActive_ = false;
    bool bufferReady_ = false;
    bool decodeEnded_ = false;
    bool underrunActive_ = false;

    mutable std::mutex audioMutex_;
    std::unique_ptr<AudioOutput> audioOutput_;

    std::mutex workerMutex_;
    std::jthread worker_;
    std::atomic_bool cancelRequested_{false};
    std::atomic_uint64_t commandSerial_{0};
};

}  // namespace

MediaPlayerFactory makeMediaPlayerFactory(AudioOutputFactory audioOutputFactory) {
    return [audioOutputFactory = std::move(audioOutputFactory)](
               MediaPlayerCreateOptions options) {
        return std::make_unique<FfmpegMediaPlayer>(
            std::move(options), audioOutputFactory);
    };
}

}  // namespace skui::ffmpeg
