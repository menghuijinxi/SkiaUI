#include "skui_ffmpeg.h"
#include "skui_runtime.h"

#include "include/core/SkColor.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

struct FakeAudioState {
    void advance(double seconds) {
        std::lock_guard lock(mutex);
        if (!running || format.sampleRate <= 0) {
            return;
        }
        const size_t requestedFrames = static_cast<size_t>(
            std::llround(seconds * format.sampleRate));
        const size_t consumedFrames = std::min(requestedFrames, bufferedFrames);
        bufferedFrames -= consumedFrames;
        playedFrames += consumedFrames;
    }

    mutable std::mutex mutex;
    skui::AudioOutputFormat format;
    size_t capacityFrames = 0;
    size_t bufferedFrames = 0;
    uint64_t playedFrames = 0;
    size_t pauseCalls = 0;
    size_t suspendCalls = 0;
    size_t finishCalls = 0;
    size_t silentWriteFrames = 0;
    bool running = false;
    bool muted = false;
    std::string error;
    std::condition_variable controlChanged;
    bool blockSuspend = false;
    bool suspendEntered = false;
    bool releaseSuspend = false;
};

class FakeAudioOutput final : public skui::AudioOutput {
public:
    explicit FakeAudioOutput(std::shared_ptr<FakeAudioState> state)
        : state_(std::move(state)) {}

    bool configure(const skui::AudioOutputFormat& format,
                   double bufferSeconds) override {
        std::lock_guard lock(state_->mutex);
        state_->format = format;
        state_->capacityFrames = static_cast<size_t>(
            std::ceil(bufferSeconds * format.sampleRate));
        state_->bufferedFrames = 0;
        state_->playedFrames = 0;
        state_->silentWriteFrames = 0;
        return true;
    }

    size_t write(std::span<const float> samples) override {
        std::lock_guard lock(state_->mutex);
        const size_t inputFrames =
            samples.size() / static_cast<size_t>(state_->format.channelCount);
        const size_t writtenFrames = std::min(
            inputFrames, state_->capacityFrames - state_->bufferedFrames);
        const size_t writtenSamples =
            writtenFrames * static_cast<size_t>(state_->format.channelCount);
        const bool isSilent = std::all_of(
            samples.begin(),
            samples.begin() + static_cast<ptrdiff_t>(writtenSamples),
            [](float sample) {
                return sample == 0.0f;
            });
        state_->bufferedFrames += writtenFrames;
        if (isSilent) {
            state_->silentWriteFrames += writtenFrames;
        }
        return writtenFrames;
    }

    bool start() override {
        std::lock_guard lock(state_->mutex);
        state_->running = true;
        return true;
    }

    void pause() override {
        std::lock_guard lock(state_->mutex);
        state_->running = false;
        ++state_->pauseCalls;
    }

    void suspend() override {
        std::unique_lock lock(state_->mutex);
        state_->running = false;
        ++state_->suspendCalls;
        state_->suspendEntered = true;
        state_->controlChanged.notify_all();
        state_->controlChanged.wait(lock, [this] {
            return !state_->blockSuspend || state_->releaseSuspend;
        });
    }

    void finish() override {
        std::lock_guard lock(state_->mutex);
        state_->running = false;
        ++state_->finishCalls;
    }

    void flush() override {
        std::lock_guard lock(state_->mutex);
        state_->bufferedFrames = 0;
        state_->playedFrames = 0;
    }

    void stop() override {
        std::lock_guard lock(state_->mutex);
        state_->running = false;
        state_->bufferedFrames = 0;
    }

    void setMuted(bool muted) override {
        std::lock_guard lock(state_->mutex);
        state_->muted = muted;
    }

    [[nodiscard]] skui::AudioOutputFormat format() const override {
        std::lock_guard lock(state_->mutex);
        return state_->format;
    }

    [[nodiscard]] uint64_t playedFrames() const override {
        std::lock_guard lock(state_->mutex);
        return state_->playedFrames;
    }

    [[nodiscard]] size_t bufferedFrames() const override {
        std::lock_guard lock(state_->mutex);
        return state_->bufferedFrames;
    }

    [[nodiscard]] size_t capacityFrames() const override {
        std::lock_guard lock(state_->mutex);
        return state_->capacityFrames;
    }

    [[nodiscard]] std::string lastError() const override {
        std::lock_guard lock(state_->mutex);
        return state_->error;
    }

private:
    std::shared_ptr<FakeAudioState> state_;
};

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool waitUntilBuffered(skui::MediaPlayer& player,
                       std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)player.tick(0.0);
        const skui::MediaPlaybackState state = player.state();
        if (state.readyState == skui::MediaReadyState::Ready ||
            state.readyState == skui::MediaReadyState::Playing) {
            return true;
        }
        if (state.readyState == skui::MediaReadyState::Failed) {
            std::cerr << "FFmpeg player failed: " << state.error << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cerr << "timed out waiting for FFmpeg predecode\n";
    return false;
}

void writeLittleEndian16(std::ofstream& output, uint16_t value) {
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8u) & 0xFFu),
    };
    output.write(bytes.data(), bytes.size());
}

void writeLittleEndian32(std::ofstream& output, uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8u) & 0xFFu),
        static_cast<char>((value >> 16u) & 0xFFu),
        static_cast<char>((value >> 24u) & 0xFFu),
    };
    output.write(bytes.data(), bytes.size());
}

class TemporaryWavFile {
public:
    ~TemporaryWavFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    bool create() {
        constexpr uint32_t kSampleRate = 48000;
        constexpr uint16_t kChannelCount = 1;
        constexpr uint16_t kBitsPerSample = 16;
        constexpr uint32_t kSampleCount = kSampleRate / 2;
        constexpr uint32_t kDataSize =
            kSampleCount * kChannelCount * kBitsPerSample / 8;

        std::error_code error;
        std::filesystem::create_directories("tmp", error);
        if (error) {
            return false;
        }
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::path("tmp") /
                ("skui-audio-only-" + std::to_string(suffix) + ".wav");
        std::ofstream output(path_, std::ios::binary);
        if (!output) {
            return false;
        }

        output.write("RIFF", 4);
        writeLittleEndian32(output, 36u + kDataSize);
        output.write("WAVEfmt ", 8);
        writeLittleEndian32(output, 16);
        writeLittleEndian16(output, 1);
        writeLittleEndian16(output, kChannelCount);
        writeLittleEndian32(output, kSampleRate);
        writeLittleEndian32(
            output,
            kSampleRate * kChannelCount * kBitsPerSample / 8);
        writeLittleEndian16(output, kChannelCount * kBitsPerSample / 8);
        writeLittleEndian16(output, kBitsPerSample);
        output.write("data", 4);
        writeLittleEndian32(output, kDataSize);
        const std::array<char, 4096> silence{};
        uint32_t remainingBytes = kDataSize;
        while (remainingBytes > 0) {
            const size_t writeSize = std::min<size_t>(
                remainingBytes, silence.size());
            output.write(silence.data(), writeSize);
            remainingBytes -= static_cast<uint32_t>(writeSize);
        }
        return output.good();
    }

    bool createTone44100Hz() {
        constexpr uint32_t kSampleRate = 44100;
        constexpr uint16_t kChannelCount = 1;
        constexpr uint16_t kBitsPerSample = 16;
        constexpr uint32_t kSampleCount = kSampleRate / 2;
        constexpr uint32_t kDataSize =
            kSampleCount * kChannelCount * kBitsPerSample / 8;
        constexpr double kToneFrequencyHz = 997.0;
        constexpr double kToneAmplitude = 0.35;

        std::error_code error;
        std::filesystem::create_directories("tmp", error);
        if (error) {
            return false;
        }
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::path("tmp") /
                ("skui-resample-" + std::to_string(suffix) + ".wav");
        std::ofstream output(path_, std::ios::binary);
        if (!output) {
            return false;
        }

        output.write("RIFF", 4);
        writeLittleEndian32(output, 36u + kDataSize);
        output.write("WAVEfmt ", 8);
        writeLittleEndian32(output, 16);
        writeLittleEndian16(output, 1);
        writeLittleEndian16(output, kChannelCount);
        writeLittleEndian32(output, kSampleRate);
        writeLittleEndian32(
            output,
            kSampleRate * kChannelCount * kBitsPerSample / 8);
        writeLittleEndian16(output, kChannelCount * kBitsPerSample / 8);
        writeLittleEndian16(output, kBitsPerSample);
        output.write("data", 4);
        writeLittleEndian32(output, kDataSize);
        for (uint32_t frame = 0; frame < kSampleCount; ++frame) {
            const double seconds = static_cast<double>(frame) / kSampleRate;
            const double sample = kToneAmplitude * std::sin(
                2.0 * std::numbers::pi_v<double> * kToneFrequencyHz * seconds);
            const int16_t quantized = static_cast<int16_t>(std::llround(
                sample * std::numeric_limits<int16_t>::max()));
            writeLittleEndian16(output, static_cast<uint16_t>(quantized));
        }
        return output.good();
    }

    [[nodiscard]] std::string path() const {
        return path_.string();
    }

private:
    std::filesystem::path path_;
};

bool testAudioOnlyPlayback(const std::string& mediaPath) {
    const auto audioState = std::make_shared<FakeAudioState>();
    skui::AudioOutputFactory audioFactory = [audioState] {
        return std::make_unique<FakeAudioOutput>(audioState);
    };
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory(std::move(audioFactory));
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   mediaPath,
                   3,
                   true,
                   false,
                   false,
               }),
               "audio-only source should be accepted") ||
        !check(player->prepare(), "audio-only source should prebuffer") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10))) {
        return false;
    }

    const skui::MediaPlaybackState prepared = player->state();
    if (!check(!prepared.hasVideo,
               "audio-only media should not report a video stream") ||
        !check(prepared.hasAudio,
               "audio-only media should report an audio stream") ||
        !check(player->currentFrame() == nullptr,
               "audio-only playback should not expose a video frame") ||
        !check(prepared.bufferedAudioSeconds >= 0.05,
               "audio-only prepare should fill the PCM buffer") ||
        !check(!prepared.decoderName.empty(),
               "audio-only metadata should expose the audio decoder")) {
        return false;
    }

    if (!check(player->play(), "audio-only source should play")) {
        return false;
    }
    (void)player->tick(0.0);
    audioState->advance(0.1);
    (void)player->tick(1.0 / 10.0);
    if (!check(player->state().currentSeconds >= 0.09,
               "audio device consumption should advance audio-only playback")) {
        return false;
    }

    if (prepared.durationSeconds > 0.0 && prepared.durationSeconds <= 1.0) {
        audioState->advance(prepared.durationSeconds * 0.7);
        (void)player->tick(1.0 / 10.0);
        const double beforeWrap = player->state().currentSeconds;
        audioState->advance(prepared.durationSeconds * 0.4);
        (void)player->tick(1.0 / 10.0);
        const skui::MediaPlaybackState afterWrap = player->state();
        if (!check(afterWrap.readyState == skui::MediaReadyState::Playing,
                   "audio-only loop should stay in Playing") ||
            !check(afterWrap.currentSeconds < beforeWrap,
                   "audio-only loop should wrap its visible media time")) {
            return false;
        }
    }

    player->pause();
    player->close();
    return true;
}

bool testGeneratedAudioOnlyWavPlayback() {
    TemporaryWavFile fixture;
    return check(fixture.create(),
                 "audio-only WAV fixture should be created") &&
           testAudioOnlyPlayback(fixture.path());
}

bool testSeekDoesNotBlockOnAudioControl() {
    TemporaryWavFile fixture;
    if (!check(fixture.create(),
               "seek audio-control fixture should be created")) {
        return false;
    }

    const auto audioState = std::make_shared<FakeAudioState>();
    skui::AudioOutputFactory audioFactory = [audioState] {
        return std::make_unique<FakeAudioOutput>(audioState);
    };
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory(std::move(audioFactory));
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   fixture.path(),
                   1,
                   false,
                   false,
                   false,
               }),
               "seek audio-control source should be accepted") ||
        !check(player->prepare(), "seek audio-control source should prepare") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10))) {
        return false;
    }

    {
        std::lock_guard lock(audioState->mutex);
        audioState->blockSuspend = true;
        audioState->suspendEntered = false;
        audioState->releaseSuspend = false;
    }
    std::future<bool> seekResult = std::async(
        std::launch::async,
        [&player] {
            return player->seek(0.2);
        });
    const bool returnedWhileWorkerBlocked =
        seekResult.wait_for(std::chrono::milliseconds(200)) ==
        std::future_status::ready;

    bool suspendEntered = false;
    {
        std::unique_lock lock(audioState->mutex);
        suspendEntered = audioState->controlChanged.wait_for(
            lock,
            std::chrono::seconds(2),
            [&audioState] {
                return audioState->suspendEntered;
            });
        audioState->releaseSuspend = true;
        audioState->blockSuspend = false;
        audioState->controlChanged.notify_all();
    }
    const bool accepted = seekResult.get();
    const bool readyAfterRelease = waitUntilBuffered(
        *player, std::chrono::seconds(10));
    player->close();
    return check(returnedWhileWorkerBlocked,
                 "seek should not wait for the audio control worker") &&
           check(suspendEntered,
                 "seek worker should perform the deferred audio reset") &&
           check(accepted, "deferred audio seek should be accepted") &&
           check(readyAfterRelease,
                 "deferred audio seek should finish after audio reset");
}

bool testResampledAudioDoesNotInsertSilentFrames() {
    TemporaryWavFile fixture;
    if (!check(fixture.createTone44100Hz(),
               "44.1 kHz audio fixture should be created")) {
        return false;
    }

    const auto audioState = std::make_shared<FakeAudioState>();
    skui::AudioOutputFactory audioFactory = [audioState] {
        return std::make_unique<FakeAudioOutput>(audioState);
    };
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory(std::move(audioFactory));
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   fixture.path(),
                   3,
                   false,
                   false,
                   false,
               }),
               "resample fixture source should be accepted") ||
        !check(player->prepare(), "resample fixture should prebuffer")) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(audioState->mutex);
            if (audioState->bufferedFrames >= 23000) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::lock_guard lock(audioState->mutex);
    constexpr size_t kExpectedOutputFrames = 24000;
    constexpr size_t kFrameTolerance = 512;
    return check(audioState->format.sampleRate == 48000,
                 "resample fixture should use the 48 kHz output format") &&
           check(audioState->bufferedFrames >=
                     kExpectedOutputFrames - kFrameTolerance,
                 "resample fixture should preserve its expected duration") &&
           check(audioState->bufferedFrames <=
                     kExpectedOutputFrames + kFrameTolerance,
                 "resample fixture should not stretch its output duration") &&
           check(audioState->silentWriteFrames == 0,
                 "resampling should not inject silent PCM frames");
}

bool testPredecodeAndAudioClock(const std::string& mediaPath,
                                bool expectVp9Alpha) {
    const auto audioState = std::make_shared<FakeAudioState>();
    skui::AudioOutputFactory audioFactory = [audioState] {
        return std::make_unique<FakeAudioOutput>(audioState);
    };
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory(std::move(audioFactory));
    std::unique_ptr<skui::MediaPlayer> player = factory({});

    if (!check(player->setSource(skui::MediaSourceOptions{
                   mediaPath,
                   3,
                   false,
                   false,
               }),
               "setSource should accept a valid path") ||
        !check(player->prepare(), "prepare should start explicit predecode") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10))) {
        return false;
    }

    const skui::MediaPlaybackState prepared = player->state();
    const sk_sp<SkImage> preparedFrame = player->currentFrame();
    if (expectVp9Alpha) {
        SkPixmap pixels;
        const sk_sp<SkImage> frame = player->currentFrame();
        if (!check(prepared.decoderName == "libvpx-vp9",
                   "VP9 WebM must use the named libvpx-vp9 decoder") ||
            !check(prepared.hasAlpha,
                   "transparent VP9 WebM should report an alpha channel") ||
            !check(frame && frame->peekPixels(&pixels),
                   "transparent VP9 frame should expose raster pixels") ||
            !check(SkColorGetA(pixels.getColor(1, 1)) < 128,
                   "transparent VP9 frame should preserve partial alpha") ||
            !check(SkColorGetA(pixels.getColor(14, 1)) > 240,
                   "opaque VP9 pixels should preserve full alpha")) {
            return false;
        }
    }

    if (!check(preparedFrame != nullptr,
               "explicit predecode should expose the first frame") ||
        !check(prepared.bufferedVideoFrames >= 3,
               "explicit predecode should fill the requested frame count") ||
        !check(prepared.videoWidth > 0 && prepared.videoHeight > 0,
               "decoded metadata should include video dimensions") ||
        !check(preparedFrame && preparedFrame->width() == prepared.videoWidth &&
                   preparedFrame->height() == prepared.videoHeight,
               "decoded frame dimensions should match video metadata") ||
        !check(!prepared.decoderName.empty(),
               "decoded metadata should expose the selected decoder")) {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const size_t stableBufferedFrames = player->state().bufferedVideoFrames;
    if (!check(stableBufferedFrames == prepared.bufferedVideoFrames,
               "predecode should stop when its bounded buffer is full")) {
        return false;
    }

    if (!check(player->play(), "play should accept a prepared source")) {
        return false;
    }
    (void)player->tick(0.0);
    const double initialSeconds = player->state().currentSeconds;
    if (prepared.hasAudio) {
        for (int index = 0; index < 5; ++index) {
            (void)player->tick(1.0 / 120.0);
        }
        if (!check(std::abs(player->state().currentSeconds - initialSeconds) < 0.001,
                   "render ticks must not advance an audio-backed media clock")) {
            return false;
        }
        audioState->advance(0.12);
        (void)player->tick(1.0 / 10.0);
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        (void)player->tick(1.0 / 10.0);
    }
    const double advancedSeconds = player->state().currentSeconds;
    if (!check(advancedSeconds >= initialSeconds + 0.1,
               "media clock should advance with audio device consumption")) {
        return false;
    }

    size_t suspendCallsBeforeUserPause = 0;
    size_t finishCallsBeforeUserPause = 0;
    if (prepared.hasAudio) {
        std::lock_guard lock(audioState->mutex);
        suspendCallsBeforeUserPause = audioState->suspendCalls;
        finishCallsBeforeUserPause = audioState->finishCalls;
    }
    player->pause();
    const uint64_t displayedBeforePausedSeek =
        player->state().displayedVideoFrames;
    if (prepared.hasAudio) {
        std::lock_guard lock(audioState->mutex);
        if (!check(audioState->pauseCalls == 1,
                   "user pause should use the pause audio notification") ||
            !check(audioState->suspendCalls == suspendCallsBeforeUserPause,
                   "user pause should not use the transient suspend notification") ||
            !check(audioState->finishCalls == finishCallsBeforeUserPause,
                   "user pause should not use the finish audio notification")) {
            return false;
        }
    }
    audioState->advance(0.2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)player->tick(0.2);
    if (!check(std::abs(player->state().currentSeconds - advancedSeconds) < 0.01,
               "paused playback should hold its media time")) {
        return false;
    }

    const double seekTarget = prepared.durationSeconds > 1.0
                                  ? prepared.durationSeconds * 0.75
                                  : prepared.durationSeconds * 0.5;
    const sk_sp<SkImage> frameBeforeSeek = player->currentFrame();
    const auto pausedSeekStarted = std::chrono::steady_clock::now();
    if (!check(frameBeforeSeek != nullptr,
               "playback should expose a frame before seeking") ||
        !check(player->seek(seekTarget), "seek should be accepted") ||
        !check(player->currentFrame() != nullptr,
               "seek should retain a visible frame while decoding") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10))) {
        return false;
    }
    const auto pausedSeekElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pausedSeekStarted);
    const sk_sp<SkImage> frameAfterSeek = player->currentFrame();
    if (!check(frameAfterSeek != nullptr,
               "seek should decode a replacement preview frame before resuming") ||
        !check(frameAfterSeek.get() != frameBeforeSeek.get(),
               "seek should replace the retained frame with the target frame") ||
        !check(pausedSeekElapsed < std::chrono::milliseconds(750),
               "paused seek should publish its first preview frame promptly") ||
        !check(player->state().displayedVideoFrames ==
                   displayedBeforePausedSeek + 1,
                "paused seek should display only one preview frame")) {
        return false;
    }
    const SkImage* pausedSeekFrame = frameAfterSeek.get();
    const uint64_t pausedSeekDisplayCount =
        player->state().displayedVideoFrames;
    bool pausedSeekChangedAgain = false;
    const auto pausedSeekStableDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < pausedSeekStableDeadline) {
        (void)player->tick(0.0);
        if (player->currentFrame().get() != pausedSeekFrame ||
            player->state().displayedVideoFrames != pausedSeekDisplayCount) {
            pausedSeekChangedAgain = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!check(!pausedSeekChangedAgain,
               "paused seek should keep the target frame visible")) {
        return false;
    }
    const std::array<double, 4> pausedSeekTargets{
        seekTarget * 0.25,
        seekTarget * 0.75,
        seekTarget * 0.4,
        seekTarget * 0.9,
    };
    const uint64_t displayedBeforeRapidPausedSeek =
        player->state().displayedVideoFrames;
    bool pausedPreviewPublishedWhileSeeking = false;
    double finalPausedSeekTarget = 0.0;
    for (size_t index = 0; index < 50; ++index) {
        finalPausedSeekTarget =
            pausedSeekTargets[index % pausedSeekTargets.size()];
        if (!check(player->seek(finalPausedSeekTarget),
                   "paused rapid seek target should be accepted")) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)player->tick(0.0);
        pausedPreviewPublishedWhileSeeking =
            pausedPreviewPublishedWhileSeeking ||
            player->state().displayedVideoFrames >
                displayedBeforeRapidPausedSeek;
    }
    finalPausedSeekTarget = pausedSeekTargets.back();
    if (!check(player->seek(finalPausedSeekTarget),
               "final paused rapid seek target should be accepted")) {
        return false;
    }
    bool rapidPausedSeekReady = false;
    const auto rapidPausedSeekDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < rapidPausedSeekDeadline) {
        (void)player->tick(0.0);
        const skui::MediaPlaybackState state = player->state();
        if (state.readyState == skui::MediaReadyState::Ready &&
            state.displayedVideoFrames > displayedBeforeRapidPausedSeek) {
            rapidPausedSeekReady = true;
            break;
        }
        if (state.readyState == skui::MediaReadyState::Failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const skui::MediaPlaybackState rapidPausedSeekState = player->state();
    if (!check(pausedPreviewPublishedWhileSeeking,
               "paused continuous seek should publish preview frames") ||
        !check(rapidPausedSeekReady,
               "paused rapid seek should finish without waiting for predecode") ||
        !check(std::abs(rapidPausedSeekState.currentSeconds -
                        finalPausedSeekTarget) < 0.01,
               "paused rapid seek should keep the latest target timeline")) {
        return false;
    }
    if (prepared.hasAudio) {
        std::lock_guard lock(audioState->mutex);
        if (!check(audioState->suspendCalls > 0,
                   "seek should transiently suspend audio output")) {
            return false;
        }
    }
    player->close();
    return true;
}

bool testPausedSeekKeepsAudioTimeline(const std::string& mediaPath) {
    const auto audioState = std::make_shared<FakeAudioState>();
    skui::AudioOutputFactory audioFactory = [audioState] {
        return std::make_unique<FakeAudioOutput>(audioState);
    };
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory(std::move(audioFactory));
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   mediaPath,
                   3,
                   false,
                   false,
                   true,
               }),
               "audio timeline source should be accepted") ||
        !check(player->prepare(), "audio timeline source should prepare") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10))) {
        return false;
    }

    const skui::MediaPlaybackState prepared = player->state();
    if (!prepared.hasAudio) {
        player->close();
        return true;
    }

    const double seekTarget = std::max(0.1, prepared.durationSeconds * 0.75);
    const uint64_t displayedBeforeSeek = prepared.displayedVideoFrames;
    if (!check(player->seek(seekTarget),
               "audio timeline paused seek should be accepted")) {
        return false;
    }

    bool pausedSeekReady = false;
    const auto pausedSeekDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < pausedSeekDeadline) {
        (void)player->tick(0.0);
        const skui::MediaPlaybackState state = player->state();
        if (state.readyState == skui::MediaReadyState::Ready &&
            state.displayedVideoFrames > displayedBeforeSeek) {
            pausedSeekReady = true;
            break;
        }
        if (state.readyState == skui::MediaReadyState::Failed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!check(pausedSeekReady,
               "paused audio seek should expose a preview before playback")) {
        player->close();
        return false;
    }

    if (!check(player->play(),
               "paused audio seek should resume playback")) {
        player->close();
        return false;
    }
    bool playing = false;
    const auto playbackDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < playbackDeadline) {
        (void)player->tick(0.0);
        if (player->state().readyState == skui::MediaReadyState::Playing) {
            playing = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!check(playing, "paused audio seek should become Playing") ||
        !check(std::abs(player->state().currentSeconds - seekTarget) < 0.02,
               "audio playback should start at the paused seek target")) {
        player->close();
        return false;
    }

    audioState->advance(0.1);
    (void)player->tick(0.1);
    const skui::MediaPlaybackState advanced = player->state();
    player->close();
    return check(std::abs(advanced.currentSeconds - (seekTarget + 0.1)) < 0.03,
                 "audio playback clock should advance from the paused seek target");
}

bool testRapidSeekUsesLatestTarget(const std::string& mediaPath) {
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory();
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   mediaPath,
                   1,
                   false,
                   false,
                   true,
               }),
               "rapid-seek source should be accepted") ||
        !check(player->prepare(), "rapid-seek source should prepare") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10))) {
        return false;
    }

    const skui::MediaPlaybackState prepared = player->state();
    const sk_sp<SkImage> frameBeforeSeek = player->currentFrame();
    if (!check(prepared.durationSeconds > 0.0,
               "rapid-seek source should expose a duration") ||
        !check(frameBeforeSeek != nullptr,
               "rapid-seek source should expose an initial frame")) {
        return false;
    }
    if (!check(player->play(), "rapid-seek source should play while dragging")) {
        return false;
    }

    const std::array<double, 5> progressTargets{0.8, 0.15, 0.65, 0.3, 0.72};
    double finalTarget = 0.0;
    bool previewPublishedWhileSeeking = false;
    for (size_t index = 0; index < 50; ++index) {
        const double progress = progressTargets[index % progressTargets.size()];
        finalTarget = prepared.durationSeconds * progress;
        if (!check(player->seek(finalTarget),
                   "rapid seek target should be accepted")) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)player->tick(0.0);
        previewPublishedWhileSeeking = previewPublishedWhileSeeking ||
            player->currentFrame().get() != frameBeforeSeek.get();
    }
    finalTarget = prepared.durationSeconds * progressTargets.back();
    if (!check(player->seek(finalTarget),
               "final rapid seek target should be accepted")) {
        return false;
    }
    player->pause();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool finalFrameReady = false;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)player->tick(0.0);
        const skui::MediaPlaybackState state = player->state();
        if (state.readyState == skui::MediaReadyState::Failed) {
            std::cerr << "rapid seek failed: " << state.error << '\n';
            break;
        }
        if ((state.readyState == skui::MediaReadyState::Ready ||
             state.readyState == skui::MediaReadyState::Paused) &&
            player->currentFrame().get() != frameBeforeSeek.get()) {
            finalFrameReady = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const skui::MediaPlaybackState finalState = player->state();
    player->close();
    return check(previewPublishedWhileSeeking,
                 "continuous seek should publish preview frames while input continues") &&
           check(finalFrameReady,
                 "rapid seek should publish a replacement frame") &&
           check(std::abs(finalState.currentSeconds - finalTarget) < 0.01,
                 "rapid seek should keep the latest target timeline");
}

bool testRapidSeekResumesPlayback(const std::string& mediaPath) {
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory();
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   mediaPath,
                   1,
                   true,
                   false,
                   true,
               }),
               "playing rapid-seek source should be accepted") ||
        !check(player->prepare(), "playing rapid-seek source should prepare") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10)) ||
        !check(player->play(), "playing rapid-seek source should play")) {
        return false;
    }

    const skui::MediaPlaybackState prepared = player->state();
    const std::array<double, 5> progressTargets{0.8, 0.15, 0.65, 0.3, 0.72};
    for (size_t index = 0; index < 50; ++index) {
        const double target =
            prepared.durationSeconds *
            progressTargets[index % progressTargets.size()];
        if (!check(player->seek(target),
                   "playing rapid seek target should be accepted")) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)player->tick(0.0);
    }

    const double finalTarget = prepared.durationSeconds * 0.57;
    const uint64_t displayedBeforeFinalSeek =
        player->state().displayedVideoFrames;
    if (!check(player->seek(finalTarget),
               "final playing rapid seek target should be accepted")) {
        return false;
    }

    bool resumed = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)player->tick(0.0);
        const skui::MediaPlaybackState state = player->state();
        if (state.readyState == skui::MediaReadyState::Failed) {
            std::cerr << "playing rapid seek failed: " << state.error << '\n';
            break;
        }
        if (state.readyState == skui::MediaReadyState::Playing &&
            state.displayedVideoFrames > displayedBeforeFinalSeek &&
            state.currentSeconds >= finalTarget) {
            resumed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    player->close();
    return check(resumed,
                 "rapid seek should resume playback from the latest target");
}

bool testRuntimeVideoFillsExplicitBox(const std::string& mediaPath) {
    skui::RuntimeOptions options;
    options.clearColor = SK_ColorGREEN;
    options.videoPredecodeFrames = 1;
    options.mediaPlayerFactory = skui::ffmpeg::makeMediaPlayerFactory();
    skui::Runtime runtime(std::move(options));
    bool ok = check(runtime.loadDocumentFromString(R"html(
<html><head><style>
html, body, .video-test {
  position: relative;
  width: 100%;
  height: 100%;
  margin: 0;
  overflow: hidden;
  background-color: #00ff00;
}
.video-test-video {
  position: absolute;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
}
</style></head><body>
  <div class="video-test">
    <video id="clip" class="video-test-video" preload="auto"></video>
  </div>
</body></html>)html"),
                    "runtime video document should load");
    ok = check(runtime.setAttributeById("clip", "data-predecode-frames", "1"),
               "runtime video should accept its predecode count") && ok;
    ok = check(runtime.setAttributeById("clip", "src", mediaPath),
               "runtime video should accept its dynamic source") && ok;
    ok = check(runtime.prepareVideoById("clip"),
               "runtime video should begin predecode") && ok;
    ok = check(runtime.playVideoById("clip"),
               "runtime video should begin playback") && ok;
    runtime.resize(160, 100, 1.0f);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool frameReady = false;
    const size_t initialDisplayedFrames =
        runtime.videoStateById("clip")
            .value_or(skui::MediaPlaybackState{})
            .displayedVideoFrames;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)runtime.tick(0.01f);
        const std::optional<skui::MediaPlaybackState> state =
            runtime.videoStateById("clip");
        if (state && (state->bufferedVideoFrames > 0 ||
                      state->displayedVideoFrames > initialDisplayedFrames)) {
            frameReady = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ok = check(frameReady, "runtime video should publish a decoded frame") && ok;

    std::vector<uint32_t> pixels(160 * 100, 0);
    ok = check(runtime.renderToBgraPixels(
                   pixels.data(), 160, 100, 160 * sizeof(uint32_t), 1.0f),
               "runtime video should render through Skia") && ok;
    size_t fallbackPixels = 0;
    for (int x = 0; x < 160; ++x) {
        if (pixels[95 * 160 + x] == 0xFF00FF00u) {
            ++fallbackPixels;
        }
    }
    ok = check(fallbackPixels < 8,
               "runtime video should cover the bottom of its explicit box") && ok;
    return ok;
}

bool testLoopKeepsAFrameAcrossBoundaries(const std::string& mediaPath) {
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory();
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    if (!check(player->setSource(skui::MediaSourceOptions{
                   mediaPath,
                   3,
                   true,
                   true,
               }),
               "loop fixture source should be accepted") ||
        !check(player->prepare(), "loop fixture should predecode") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10)) ||
        !check(player->play(), "loop fixture should play")) {
        return false;
    }

    bool wrapped = false;
    double previousSeconds = 0.0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1300);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)player->tick(0.005);
        const skui::MediaPlaybackState state = player->state();
        if (!check(state.readyState != skui::MediaReadyState::Failed,
                   "loop playback should not fail") ||
            !check(state.readyState != skui::MediaReadyState::Ended,
                   "loop playback should not enter Ended") ||
            !check(state.readyState != skui::MediaReadyState::Rebuffering,
                   "loop head cache should avoid boundary rebuffering") ||
            !check(player->currentFrame() != nullptr,
                   "loop playback should retain a visible frame")) {
            return false;
        }
        if (state.currentSeconds + 0.2 < previousSeconds) {
            wrapped = true;
        }
        previousSeconds = state.currentSeconds;
    }
    player->close();
    return check(wrapped, "loop fixture should cross at least one boundary");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: SkuiFfmpegPlayerTests <media-file> "
                     "[--expect-vp9-alpha|--expect-audio-only]\n";
        return 2;
    }
    const bool expectVp9Alpha =
        argc == 3 && std::string(argv[2]) == "--expect-vp9-alpha";
    const bool expectAudioOnly =
        argc == 3 && std::string(argv[2]) == "--expect-audio-only";
    if (expectAudioOnly) {
        if (!testAudioOnlyPlayback(argv[1])) {
            return 1;
        }
        std::cout << "SkUI FFmpeg player tests passed\n";
        return 0;
    }
    if (!testPredecodeAndAudioClock(argv[1], expectVp9Alpha)) {
        return 1;
    }
    if (!testPausedSeekKeepsAudioTimeline(argv[1])) {
        return 1;
    }
    if (!testRapidSeekUsesLatestTarget(argv[1])) {
        return 1;
    }
    if (!testRapidSeekResumesPlayback(argv[1])) {
        return 1;
    }
    if (!testRuntimeVideoFillsExplicitBox(argv[1])) {
        return 1;
    }
    if (expectVp9Alpha && !testLoopKeepsAFrameAcrossBoundaries(argv[1])) {
        return 1;
    }
    if (!testGeneratedAudioOnlyWavPlayback()) {
        return 1;
    }
    if (!testSeekDoesNotBlockOnAudioControl()) {
        return 1;
    }
    if (!testResampledAudioDoesNotInsertSilentFrames()) {
        return 1;
    }
    std::cout << "SkUI FFmpeg player tests passed\n";
    return 0;
}
