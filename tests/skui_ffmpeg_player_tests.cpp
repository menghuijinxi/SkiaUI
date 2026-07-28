#include "skui_ffmpeg.h"

#include "include/core/SkColor.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
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
    size_t pauseCalls = 0;
    size_t suspendCalls = 0;
    size_t finishCalls = 0;
    size_t silentWriteFrames = 0;
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
        std::lock_guard lock(state_->mutex);
        state_->running = false;
        ++state_->suspendCalls;
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
    return check(audioState->bufferedFrames >= 23000,
                 "resample fixture should queue its PCM") &&
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
    if (prepared.hasAudio) {
        std::lock_guard lock(audioState->mutex);
        if (!check(audioState->pauseCalls == 1,
                   "user pause should use the pause audio notification") ||
            !check(audioState->suspendCalls == 0,
                   "user pause should not use the transient suspend notification") ||
            !check(audioState->finishCalls == 0,
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
                                  ? 0.5
                                  : prepared.durationSeconds * 0.5;
    if (!check(player->seek(seekTarget), "seek should be accepted") ||
        !waitUntilBuffered(*player, std::chrono::seconds(10)) ||
        !check(player->currentFrame() != nullptr,
               "seek should decode a replacement frame before resuming")) {
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
    if (expectVp9Alpha && !testLoopKeepsAFrameAcrossBoundaries(argv[1])) {
        return 1;
    }
    if (!testGeneratedAudioOnlyWavPlayback()) {
        return 1;
    }
    if (!testResampledAudioDoesNotInsertSilentFrames()) {
        return 1;
    }
    std::cout << "SkUI FFmpeg player tests passed\n";
    return 0;
}
