#include "skui_runtime.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
    }
    return condition;
}

sk_sp<SkImage> makeRedFrame() {
    constexpr int kSize = 2;
    const SkImageInfo info = SkImageInfo::Make(
        kSize,
        kSize,
        kBGRA_8888_SkColorType,
        kPremul_SkAlphaType,
        SkColorSpace::MakeSRGB());
    const uint32_t pixels[kSize * kSize] = {
        0xFFFF0000u,
        0xFFFF0000u,
        0xFFFF0000u,
        0xFFFF0000u,
    };
    return SkImages::RasterFromPixmapCopy(
        SkPixmap(info, pixels, kSize * sizeof(uint32_t)));
}

struct FakePlayerState {
    skui::MediaSourceOptions source;
    skui::MediaPlaybackState playback;
    sk_sp<SkImage> frame;
    int setSourceCalls = 0;
    int metadataCalls = 0;
    int prepareCalls = 0;
    int playCalls = 0;
    int pauseCalls = 0;
    int tickCalls = 0;
    int closeCalls = 0;
};

class FakePlayer final : public skui::MediaPlayer {
public:
    FakePlayer(std::shared_ptr<FakePlayerState> state,
               skui::MediaPlayerCreateOptions options)
        : state_(std::move(state)), options_(std::move(options)) {}

    bool setSource(const skui::MediaSourceOptions& options) override {
        state_->source = options;
        state_->playback = {};
        state_->frame.reset();
        ++state_->setSourceCalls;
        return !options.source.empty();
    }

    void setLoop(bool loop) override {
        state_->source.loop = loop;
    }

    void setMuted(bool muted) override {
        state_->source.muted = muted;
    }

    bool loadMetadata() override {
        ++state_->metadataCalls;
        state_->playback.readyState = skui::MediaReadyState::Ready;
        state_->playback.videoWidth = 2;
        state_->playback.videoHeight = 2;
        return true;
    }

    bool prepare() override {
        ++state_->prepareCalls;
        state_->playback.readyState = skui::MediaReadyState::Ready;
        state_->playback.videoWidth = 2;
        state_->playback.videoHeight = 2;
        state_->playback.bufferedVideoFrames = state_->source.predecodeFrames;
        state_->frame = makeRedFrame();
        if (options_.requestRedraw) {
            options_.requestRedraw();
        }
        return true;
    }

    bool play() override {
        ++state_->playCalls;
        if (!state_->frame) {
            prepare();
        }
        state_->playback.readyState = skui::MediaReadyState::Playing;
        return true;
    }

    void pause() override {
        ++state_->pauseCalls;
        state_->playback.readyState = skui::MediaReadyState::Paused;
    }

    bool seek(double seconds) override {
        state_->playback.currentSeconds = seconds;
        return seconds >= 0.0;
    }

    void close() override {
        ++state_->closeCalls;
        state_->playback.readyState = skui::MediaReadyState::Idle;
    }

    bool tick(double deltaSeconds) override {
        ++state_->tickCalls;
        if (state_->playback.readyState != skui::MediaReadyState::Playing) {
            return false;
        }
        state_->playback.currentSeconds += deltaSeconds;
        return true;
    }

    bool needsTicks() const override {
        return state_->playback.readyState == skui::MediaReadyState::Playing;
    }

    sk_sp<SkImage> currentFrame() const override {
        return state_->frame;
    }

    skui::MediaPlaybackState state() const override {
        return state_->playback;
    }

private:
    std::shared_ptr<FakePlayerState> state_;
    skui::MediaPlayerCreateOptions options_;
};

struct FakePlayerFactory {
    std::vector<std::shared_ptr<FakePlayerState>> players;

    skui::MediaPlayerFactory callback() {
        return [this](skui::MediaPlayerCreateOptions options) {
            auto state = std::make_shared<FakePlayerState>();
            players.push_back(state);
            return std::make_unique<FakePlayer>(state, std::move(options));
        };
    }
};

bool renderCenter(skui::Runtime& runtime, uint32_t& pixel) {
    constexpr int kSize = 16;
    std::vector<uint32_t> pixels(kSize * kSize);
    if (!runtime.renderToBgraPixels(
            pixels.data(),
            kSize,
            kSize,
            kSize * sizeof(uint32_t),
            1.0f)) {
        return false;
    }
    pixel = pixels[8 * kSize + 8];
    return true;
}

bool testExplicitPreloadPreparesBeforePlay() {
    FakePlayerFactory factory;
    int redraws = 0;
    skui::RuntimeOptions options;
    options.clearColor = SK_ColorBLACK;
    options.mediaPlayerFactory = factory.callback();
    options.requestRedraw = [&] {
        ++redraws;
    };
    skui::Runtime runtime(std::move(options));
    const bool loaded = runtime.loadDocumentFromString(R"html(
<html><body>
  <video id="clip" src="clip.webm" preload="auto"
         data-predecode-frames="4" width="16" height="16"></video>
</body></html>)html");

    uint32_t pixel = 0;
    bool ok = expect(loaded, "preload document loads") &&
              expect(factory.players.size() == 1, "video creates one player");
    if (!ok) {
        return false;
    }
    const std::shared_ptr<FakePlayerState>& player = factory.players.front();
    ok = expect(player->setSourceCalls == 1, "source configured once") && ok;
    ok = expect(player->prepareCalls == 1, "preload auto prepares") && ok;
    ok = expect(player->playCalls == 0, "preload does not start playback") && ok;
    ok = expect(player->source.predecodeFrames == 4, "element frame count is used") && ok;
    ok = expect(redraws == 1, "first frame requests redraw") && ok;
    ok = expect(renderCenter(runtime, pixel), "prepared frame renders") && ok;
    ok = expect(pixel == 0xFFFF0000u, "video frame is drawn through Skia") && ok;
    return ok;
}

bool testOnDemandPlayDoesNotPreload() {
    FakePlayerFactory factory;
    skui::RuntimeOptions options;
    options.mediaPlayerFactory = factory.callback();
    skui::Runtime runtime(std::move(options));
    const bool loaded = runtime.loadDocumentFromString(R"html(
<html><body>
  <video id="clip" src="clip.webm" width="16" height="16"></video>
</body></html>)html");

    bool ok = expect(loaded, "on-demand document loads") &&
              expect(factory.players.size() == 1, "on-demand video creates player");
    if (!ok) {
        return false;
    }
    const std::shared_ptr<FakePlayerState>& player = factory.players.front();
    ok = expect(player->prepareCalls == 0, "missing preload does not prepare") && ok;
    ok = expect(runtime.playVideoById("clip"), "explicit play is accepted") && ok;
    ok = expect(player->playCalls == 1, "explicit play reaches player") && ok;
    ok = expect(runtime.tick(0.1f), "playing video keeps tick active") && ok;
    const auto state = runtime.videoStateById("clip");
    ok = expect(state.has_value(), "video state is available") && ok;
    ok = expect(state && std::abs(state->currentSeconds - 0.1) < 0.0001,
                "tick forwards real delta") && ok;
    ok = expect(runtime.pauseVideoById("clip"), "pause is accepted") && ok;
    ok = expect(!runtime.tick(0.1f), "paused video no longer requests ticks") && ok;
    return ok;
}

bool testMetadataAutoplayAndRemovalLifecycle() {
    FakePlayerFactory factory;
    skui::RuntimeOptions options;
    options.mediaPlayerFactory = factory.callback();
    skui::Runtime runtime(std::move(options));
    bool ok = expect(runtime.loadDocumentFromString(R"html(
<html><body>
  <video id="metadata" src="metadata.webm" preload="metadata"></video>
  <video id="autoplay" src="auto.webm" autoplay></video>
</body></html>)html"),
                     "metadata/autoplay document loads");
    ok = expect(factory.players.size() == 2, "both media elements create players") && ok;
    if (!ok) {
        return false;
    }
    ok = expect(factory.players[0]->metadataCalls == 1,
                "metadata preload does not decode frames") && ok;
    ok = expect(factory.players[0]->prepareCalls == 0,
                "metadata preload stays metadata-only") && ok;
    ok = expect(factory.players[1]->playCalls == 1,
                "autoplay starts on-demand playback") && ok;
    ok = expect(runtime.removeElementById("autoplay"), "video node can be removed") && ok;
    ok = expect(factory.players[1]->closeCalls == 1,
                "removing node closes its player immediately") && ok;
    return ok;
}

bool testMetadataPreloadCanEscalateToExplicitPredecode() {
    FakePlayerFactory factory;
    skui::RuntimeOptions options;
    options.mediaPlayerFactory = factory.callback();
    skui::Runtime runtime(std::move(options));
    bool ok = expect(runtime.loadDocumentFromString(R"html(
<html><body>
  <video id="clip" src="clip.webm" preload="metadata"></video>
</body></html>)html"),
                     "metadata escalation document loads");
    if (!ok || factory.players.empty()) {
        return false;
    }
    const std::shared_ptr<FakePlayerState>& player = factory.players.front();
    ok = expect(player->metadataCalls == 1,
                "metadata mode opens metadata once") && ok;
    ok = expect(runtime.setAttributeById("clip", "preload", "auto"),
                "preload attribute can change to auto") && ok;
    ok = expect(player->prepareCalls == 1,
                "auto mode explicitly starts frame predecode after metadata") && ok;
    return ok;
}

bool testVideoMetadataProvidesIntrinsicLayoutSize() {
    FakePlayerFactory factory;
    skui::RuntimeOptions options;
    options.clearColor = SK_ColorBLACK;
    options.mediaPlayerFactory = factory.callback();
    skui::Runtime runtime(std::move(options));
    bool ok = expect(runtime.loadDocumentFromString(R"html(
<html><body>
  <video id="clip" src="clip.webm" preload="auto"></video>
</body></html>)html"),
                     "intrinsic-size video document loads");
    std::vector<uint32_t> pixels(4 * 6, 0);
    ok = expect(runtime.renderToBgraPixels(
                    pixels.data(), 4, 6, 4 * sizeof(uint32_t), 1.0f),
                "intrinsic-size video renders") && ok;
    ok = expect(pixels[1 * 4 + 1] == 0xFFFF0000u,
                "metadata dimensions give video a 2x2 intrinsic box") && ok;
    ok = expect(pixels[5 * 4 + 3] == 0xFF000000u,
                "video metadata preserves intrinsic aspect ratio") && ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = testExplicitPreloadPreparesBeforePlay() && ok;
    ok = testOnDemandPlayDoesNotPreload() && ok;
    ok = testMetadataAutoplayAndRemovalLifecycle() && ok;
    ok = testMetadataPreloadCanEscalateToExplicitPredecode() && ok;
    ok = testVideoMetadataProvidesIntrinsicLayoutSize() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "SkUI media runtime tests passed\n";
    return 0;
}
