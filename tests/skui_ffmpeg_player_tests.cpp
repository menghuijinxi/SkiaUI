#include "skui_ffmpeg.h"

#include "include/core/SkColor.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

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
    bool running = false;
    bool muted = false;
    std::string error;
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
        return true;
    }

    size_t write(std::span<const float> samples) override {
        std::lock_guard lock(state_->mutex);
        const size_t inputFrames =
            samples.size() / static_cast<size_t>(state_->format.channelCount);
        const size_t writtenFrames = std::min(
            inputFrames, state_->capacityFrames - state_->bufferedFrames);
        state_->bufferedFrames += writtenFrames;
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

    if (!check(player->currentFrame() != nullptr,
               "explicit predecode should expose the first frame") ||
        !check(prepared.bufferedVideoFrames >= 3,
               "explicit predecode should fill the requested frame count") ||
        !check(prepared.videoWidth > 0 && prepared.videoHeight > 0,
               "decoded metadata should include video dimensions") ||
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

    player->pause();
    audioState->advance(0.2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)player->tick(0.2);
    if (!check(std::abs(player->state().currentSeconds - advancedSeconds) < 0.01,
               "paused playback should hold its media time")) {
        return false;
    }

    const double seekTarget = prepared.durationSeconds > 1.0
                                  ? 0.5
                                  : prepared.durationSeconds * 0.5;
    if (!check(player->seek(seekTarget), "seek should be accepted") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10)) ||
        !check(player->currentFrame() != nullptr,
               "seek should decode a replacement frame before resuming")) {
        return false;
    }
    player->close();
    return true;
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
                     "[--expect-vp9-alpha]\n";
        return 2;
    }
    const bool expectVp9Alpha =
        argc == 3 && std::string(argv[2]) == "--expect-vp9-alpha";
    if (!testPredecodeAndAudioClock(argv[1], expectVp9Alpha)) {
        return 1;
    }
    if (expectVp9Alpha && !testLoopKeepsAFrameAcrossBoundaries(argv[1])) {
        return 1;
    }
    std::cout << "SkUI FFmpeg player tests passed\n";
    return 0;
}
