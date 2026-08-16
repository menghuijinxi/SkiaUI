#include "skui_ffmpeg.h"
#include "skui_runtime.h"

#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr unsigned kChannelTolerance = 2;

std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    const int size = WideCharToMultiByte(CP_UTF8,
                                         0,
                                         wide.data(),
                                         static_cast<int>(wide.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0) {
        return path.string();
    }
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        static_cast<int>(wide.size()),
                        utf8.data(),
                        size,
                        nullptr,
                        nullptr);
    return utf8;
#else
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
#endif
}

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool isReady(const std::optional<skui::MediaPlaybackState>& state) {
    return state && (state->readyState == skui::MediaReadyState::Ready ||
                     state->readyState == skui::MediaReadyState::Playing ||
                     state->readyState == skui::MediaReadyState::Paused);
}

bool waitForFrames(skui::Runtime& runtime) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)runtime.tick(0.0f);
        const auto background = runtime.videoStateById("background");
        const auto overlay = runtime.videoStateById("overlay");
        if ((background && background->readyState == skui::MediaReadyState::Failed) ||
            (overlay && overlay->readyState == skui::MediaReadyState::Failed)) {
            std::cerr << "background error: "
                      << (background ? background->error : "missing state") << '\n'
                      << "overlay error: "
                      << (overlay ? overlay->error : "missing state") << '\n';
            return false;
        }
        if (isReady(background) && isReady(overlay)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "timed out waiting for both video frames\n";
    return false;
}

bool renderPixels(skui::Runtime& runtime, std::vector<uint32_t>& pixels) {
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    if (runtime.renderToBgraPixels(pixels.data(),
                                  kWidth,
                                  kHeight,
                                  static_cast<size_t>(kWidth) * sizeof(uint32_t),
                                  1.0f)) {
        return true;
    }
    std::cerr << "render failed: " << runtime.lastError() << '\n';
    return false;
}

unsigned channel(uint32_t color, unsigned shift) {
    return (color >> shift) & 0xFFu;
}

bool channelsNear(uint32_t actual, uint32_t expected) {
    for (const unsigned shift : {0u, 8u, 16u}) {
        const unsigned a = channel(actual, shift);
        const unsigned e = channel(expected, shift);
        if (a > e + kChannelTolerance || e > a + kChannelTolerance) {
            return false;
        }
    }
    return true;
}

uint32_t lighten(uint32_t destination, uint32_t source) {
    return 0xFF000000u |
           (std::max(channel(destination, 16u), channel(source, 16u)) << 16u) |
           (std::max(channel(destination, 8u), channel(source, 8u)) << 8u) |
           std::max(channel(destination, 0u), channel(source, 0u));
}

bool validateComposite(const std::vector<uint32_t>& destination,
                       const std::vector<uint32_t>& source,
                       const std::vector<uint32_t>& composite) {
    size_t compared = 0;
    size_t matched = 0;
    size_t blackSourceSamples = 0;
    size_t preservedBlackSamples = 0;
    size_t visibleOverlaySamples = 0;
    for (int y = 2; y < kHeight - 2; ++y) {
        for (int x = 2; x < kWidth - 2; ++x) {
            const size_t index = static_cast<size_t>(y) * kWidth + x;
            const uint32_t expected = lighten(destination[index], source[index]);
            ++compared;
            if (channelsNear(composite[index], expected)) {
                ++matched;
            }

            const unsigned sourceMaximum = std::max({
                channel(source[index], 0u),
                channel(source[index], 8u),
                channel(source[index], 16u),
            });
            const unsigned destinationMaximum = std::max({
                channel(destination[index], 0u),
                channel(destination[index], 8u),
                channel(destination[index], 16u),
            });
            if (sourceMaximum <= 12u && destinationMaximum >= 24u) {
                ++blackSourceSamples;
                if (channelsNear(composite[index], destination[index])) {
                    ++preservedBlackSamples;
                }
            }
            if (channel(source[index], 0u) > channel(destination[index], 0u) + 16u ||
                channel(source[index], 8u) > channel(destination[index], 8u) + 16u ||
                channel(source[index], 16u) > channel(destination[index], 16u) + 16u) {
                ++visibleOverlaySamples;
            }
        }
    }

    const double matchRatio = compared == 0
                                  ? 0.0
                                  : static_cast<double>(matched) / compared;
    std::cout << "matching pixels: " << matched << '/' << compared
              << " (" << matchRatio * 100.0 << "%)\n"
              << "black source samples: " << preservedBlackSamples << '/'
              << blackSourceSamples << '\n'
              << "visible overlay samples: " << visibleOverlaySamples << '\n';
    return expect(matchRatio >= 0.995,
                  "composite pixels must follow max(darkBackground, overlay)") &&
           expect(blackSourceSamples >= 100,
                  "fixture must contain black overlay pixels over a visible background") &&
           expect(preservedBlackSamples == blackSourceSamples,
                  "black overlay pixels must preserve the darkened background") &&
           expect(visibleOverlaySamples >= 100,
                  "fixture must contain visible overlay content");
}

bool writeComparison(const std::filesystem::path& path,
                     const std::vector<uint32_t>& destination,
                     const std::vector<uint32_t>& source,
                     const std::vector<uint32_t>& composite) {
    if (path.empty()) {
        return true;
    }
    constexpr int kOutputWidth = kWidth * 3;
    std::vector<uint32_t> pixels(static_cast<size_t>(kOutputWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        const size_t inputOffset = static_cast<size_t>(y) * kWidth;
        const size_t outputOffset = static_cast<size_t>(y) * kOutputWidth;
        std::copy_n(destination.begin() + inputOffset,
                    kWidth,
                    pixels.begin() + outputOffset);
        std::copy_n(source.begin() + inputOffset,
                    kWidth,
                    pixels.begin() + outputOffset + kWidth);
        std::copy_n(composite.begin() + inputOffset,
                    kWidth,
                    pixels.begin() + outputOffset + kWidth * 2);
    }

    const SkImageInfo info = SkImageInfo::Make(kOutputWidth,
                                               kHeight,
                                               kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType,
                                               SkColorSpace::MakeSRGB());
    const SkPixmap pixmap(info,
                          pixels.data(),
                          static_cast<size_t>(kOutputWidth) * sizeof(uint32_t));
    SkPngEncoder::Options options;
    const sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, options);
    if (!png) {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    file.write(static_cast<const char*>(png->data()),
               static_cast<std::streamsize>(png->size()));
    return file.good();
}

bool waitForPlayerReady(skui::MediaPlayer& player,
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
            std::cerr << "replay fixture failed: " << state.error << '\n';
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cerr << "timed out waiting for replay fixture frames\n";
    return false;
}

bool testReplayStartsWithFreshFrames(const std::filesystem::path& mediaPath) {
    const skui::MediaPlayerFactory factory =
        skui::ffmpeg::makeMediaPlayerFactory();
    std::unique_ptr<skui::MediaPlayer> player = factory({});
    bool ok = expect(player->setSource(skui::MediaSourceOptions{
                         pathToUtf8(mediaPath),
                         1,
                         false,
                         true,
                     }),
                     "replay fixture source is accepted");
    ok = expect(player->prepare(), "replay fixture begins predecode") && ok;
    ok = waitForPlayerReady(*player, std::chrono::seconds(10)) && ok;
    if (!ok) {
        return false;
    }

    const double duration = player->state().durationSeconds;
    const double activeSeekSeconds = std::clamp(duration * 0.5, 1.0, 2.0);
    for (int attempt = 0; attempt < 12; ++attempt) {
        ok = expect(player->seek(activeSeekSeconds),
                    "active replay seek is accepted") && ok;
        ok = waitForPlayerReady(*player, std::chrono::seconds(10)) && ok;
        const uint64_t displayedBeforePlay = player->state().displayedVideoFrames;
        ok = expect(player->play(), "active replay segment begins playback") && ok;
        const auto displayDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < displayDeadline &&
               player->state().displayedVideoFrames == displayedBeforePlay) {
            (void)player->tick(0.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok = expect(player->state().displayedVideoFrames > displayedBeforePlay,
                    "active replay segment presents a frame") && ok;

        // The consumed frame wakes the decoder. Reset while its next batch may still be in flight.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        player->pause();
        ok = expect(player->seek(0.0), "closed replay segment resets to zero") && ok;
    }
    if (!ok || !waitForPlayerReady(*player, std::chrono::seconds(10))) {
        return false;
    }

    const uint64_t displayedBeforeReplay = player->state().displayedVideoFrames;
    ok = expect(player->play(), "reset replay segment begins playback") && ok;
    const auto replayDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    while (std::chrono::steady_clock::now() < replayDeadline) {
        (void)player->tick(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const skui::MediaPlaybackState replayed = player->state();
    ok = expect(replayed.displayedVideoFrames >= displayedBeforeReplay + 2,
                "replay must present fresh frames immediately after reset") && ok;
    ok = expect(replayed.currentSeconds >= 0.2 && replayed.currentSeconds < 0.8,
                "replay clock must restart near zero at normal speed") && ok;
    player->close();
    return ok;
}

int run(const std::filesystem::path& backgroundPath,
        const std::filesystem::path& overlayPath,
        const std::filesystem::path& outputPath) {
    if (!expect(std::filesystem::is_regular_file(backgroundPath),
                "background video must exist") ||
        !expect(std::filesystem::is_regular_file(overlayPath),
                "overlay video must exist")) {
        return 1;
    }

    skui::RuntimeOptions options;
    options.clearColor = SK_ColorBLACK;
    options.mediaPlayerFactory = skui::ffmpeg::makeMediaPlayerFactory();
    skui::Runtime runtime(std::move(options));
    runtime.resize(kWidth, kHeight, 1.0f);
    bool ok = expect(runtime.loadDocumentFromString(R"html(
<html><head><style>
html, body, .stage, video, .dimmer {
  position: absolute;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
  margin: 0;
}
html, body, .stage { background-color: #000000; }
.dimmer { background-color: #000000; opacity: 0.6; }
.overlay { mix-blend-mode: lighten; }
</style></head><body><div class="stage">
  <video id="background" preload="none" muted></video>
  <div id="dimmer" class="dimmer"></div>
  <video id="overlay" class="overlay" preload="none" muted></video>
</div></body></html>)html"),
                     "composite test document loads");
    ok = expect(runtime.setAttributeById(
                    "background", "src", pathToUtf8(backgroundPath)),
                "background source is assigned") && ok;
    ok = expect(runtime.setAttributeById(
                    "overlay", "src", pathToUtf8(overlayPath)),
                "overlay source is assigned") && ok;
    ok = expect(runtime.prepareVideoById("background") &&
                    runtime.prepareVideoById("overlay"),
                "both videos accept explicit predecode") && ok;
    ok = waitForFrames(runtime) && ok;
    if (!ok) {
        return 1;
    }

    std::vector<uint32_t> destination;
    std::vector<uint32_t> source;
    std::vector<uint32_t> composite;
    ok = expect(runtime.setVisibleById("overlay", false),
                "overlay hides for destination render") && ok;
    ok = renderPixels(runtime, destination) && ok;
    ok = expect(runtime.setVisibleById("background", false) &&
                    runtime.setVisibleById("dimmer", false) &&
                    runtime.setVisibleById("overlay", true),
                "overlay isolates for source render") && ok;
    ok = renderPixels(runtime, source) && ok;
    ok = expect(runtime.setVisibleById("background", true) &&
                    runtime.setVisibleById("dimmer", true),
                "all composite layers restore") && ok;
    ok = renderPixels(runtime, composite) && ok;
    ok = validateComposite(destination, source, composite) && ok;
    ok = expect(writeComparison(outputPath, destination, source, composite),
                "comparison PNG is written") && ok;
    ok = testReplayStartsWithFreshFrames(overlayPath) && ok;
    return ok ? 0 : 1;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    if (argc < 3 || argc > 4) {
        std::wcerr << L"usage: SkuiVideoCompositeTests <background> <overlay> [output.png]\n";
        return 2;
    }
    return run(argv[1], argv[2], argc == 4 ? std::filesystem::path(argv[3])
                                           : std::filesystem::path{});
}
#else
int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: SkuiVideoCompositeTests <background> <overlay> [output.png]\n";
        return 2;
    }
    return run(argv[1], argv[2], argc == 4 ? std::filesystem::path(argv[3])
                                           : std::filesystem::path{});
}
#endif
