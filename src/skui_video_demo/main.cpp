#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_win32_app.h"
#include "skui_ffmpeg.h"
#include "skui_win32_audio.h"

#include "include/core/SkColor.h"
#include "skui/core/skui_internal.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr SkColor kClearColor = SkColorSetRGB(9, 11, 15);
constexpr int kInitialFrameRate = 60;
constexpr std::chrono::milliseconds kTelemetryInterval(250);

const std::filesystem::path kIntroVideoPath =
    LR"(E:\Project\Init_Ue_Project_UE5_5\Content\Movies\区位价值\开始.mp4)";
const std::filesystem::path kLoopVideoPath =
    LR"(E:\Project\Init_Ue_Project_UE5_5\Content\Movies\区位价值\循环.mp4)";
const std::filesystem::path kLogoVideoPath =
    LR"(E:\Project\Init_Ue_Project_UE5_5\Content\Movies\LOGO演绎.mp4)";

enum class SequencePhase {
    Idle,
    StartingIntro,
    IntroPlaying,
    IntroPaused,
    LoopPlaying,
    LoopPaused,
    Failed
};

struct DemoState {
    skui::win32::Dx12WindowApp* app = nullptr;
    SequencePhase sequencePhase = SequencePhase::Idle;
    int targetFrameRate = kInitialFrameRate;
    uint64_t sampleFrameCount = 0;
    double measuredFrameRate = 0.0;
    std::chrono::steady_clock::time_point sampleStarted =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastTelemetry =
        std::chrono::steady_clock::time_point::min();
};

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::string defaultDocumentPath() {
    const std::filesystem::path working =
        std::filesystem::current_path() / "assets" / "skui_video_demo" /
        "video_demo.html";
    if (std::filesystem::exists(working)) {
        return skui::pathToUtf8(working);
    }

    wchar_t modulePath[32768]{};
    if (!GetModuleFileNameW(nullptr,
                            modulePath,
                            static_cast<DWORD>(std::size(modulePath)))) {
        return "assets/skui_video_demo/video_demo.html";
    }
    const std::filesystem::path local =
        std::filesystem::path(modulePath).parent_path() / "assets" /
        "skui_video_demo" / "video_demo.html";
    return skui::pathToUtf8(local);
}

std::string_view readyStateName(skui::MediaReadyState state) {
    switch (state) {
    case skui::MediaReadyState::Idle:
        return "空闲";
    case skui::MediaReadyState::Opening:
        return "打开中";
    case skui::MediaReadyState::Prebuffering:
        return "预解码中";
    case skui::MediaReadyState::Ready:
        return "已预解码";
    case skui::MediaReadyState::Playing:
        return "播放中";
    case skui::MediaReadyState::Paused:
        return "已暂停";
    case skui::MediaReadyState::Rebuffering:
        return "重新缓冲";
    case skui::MediaReadyState::Ended:
        return "已结束";
    case skui::MediaReadyState::Failed:
        return "失败";
    }
    return "未知";
}

std::string compactState(const std::optional<skui::MediaPlaybackState>& state) {
    if (!state) {
        return "未创建";
    }
    if (state->readyState == skui::MediaReadyState::Failed) {
        return "失败：" + state->error;
    }

    std::ostringstream text;
    text << readyStateName(state->readyState) << " · " << std::fixed
         << std::setprecision(2) << state->currentSeconds << "/"
         << state->durationSeconds << "s · 缓冲 "
         << state->bufferedVideoFrames << " 帧 · 丢帧 "
         << state->droppedVideoFrames;
    return text.str();
}

void setSequenceVideo(skui::Runtime& runtime, bool showIntro) {
    runtime.beginUpdate();
    runtime.setVisibleById("intro-video", showIntro);
    runtime.setVisibleById("loop-video", !showIntro);
    runtime.setTextById("sequence-badge", showIntro ? "INTRO" : "LOOP");
    runtime.endUpdate();
}

void selectFrameRate(skui::Runtime& runtime,
                     DemoState& state,
                     int framesPerSecond) {
    static constexpr std::array<int, 4> kFrameRates{10, 30, 60, 120};
    state.targetFrameRate = framesPerSecond;
    state.sampleFrameCount = 0;
    state.measuredFrameRate = 0.0;
    state.sampleStarted = std::chrono::steady_clock::now();
    if (state.app) {
        state.app->setFrameRateLimit(framesPerSecond);
    }

    runtime.beginUpdate();
    for (const int candidate : kFrameRates) {
        const std::string id = "fps-" + std::to_string(candidate);
        if (candidate == framesPerSecond) {
            runtime.addClassById(id, "selected");
        } else {
            runtime.removeClassById(id, "selected");
        }
    }
    runtime.endUpdate();
}

bool beginSequence(skui::Runtime& runtime, DemoState& state) {
    const std::optional<skui::MediaPlaybackState> intro =
        runtime.videoStateById("intro-video");
    if (!intro || intro->readyState == skui::MediaReadyState::Failed) {
        state.sequencePhase = SequencePhase::Failed;
        return false;
    }

    if (state.sequencePhase == SequencePhase::IntroPaused) {
        state.sequencePhase = SequencePhase::IntroPlaying;
        return runtime.playVideoById("intro-video");
    }
    if (state.sequencePhase == SequencePhase::LoopPaused) {
        state.sequencePhase = SequencePhase::LoopPlaying;
        return runtime.playVideoById("loop-video");
    }

    runtime.pauseVideoById("loop-video");
    const bool restartNeeded =
        state.sequencePhase == SequencePhase::LoopPlaying ||
        intro->readyState == skui::MediaReadyState::Ended ||
        intro->currentSeconds > 0.001;
    if (restartNeeded) {
        if (!runtime.seekVideoById("intro-video", 0.0) ||
            !runtime.playVideoById("intro-video")) {
            state.sequencePhase = SequencePhase::Failed;
            return false;
        }
        // 重播时继续显示上一段的末帧，等开始片段重新具备首帧再切换。
        state.sequencePhase = SequencePhase::StartingIntro;
        return true;
    }

    setSequenceVideo(runtime, true);
    if (!runtime.playVideoById("intro-video")) {
        state.sequencePhase = SequencePhase::Failed;
        return false;
    }
    state.sequencePhase = SequencePhase::IntroPlaying;
    return true;
}

void pauseSequence(skui::Runtime& runtime, DemoState& state) {
    if (state.sequencePhase == SequencePhase::IntroPlaying ||
        state.sequencePhase == SequencePhase::StartingIntro) {
        runtime.pauseVideoById("intro-video");
        state.sequencePhase = SequencePhase::IntroPaused;
    } else if (state.sequencePhase == SequencePhase::LoopPlaying) {
        runtime.pauseVideoById("loop-video");
        state.sequencePhase = SequencePhase::LoopPaused;
    }
}

void advanceSequence(skui::Runtime& runtime, DemoState& state) {
    const std::optional<skui::MediaPlaybackState> intro =
        runtime.videoStateById("intro-video");
    const std::optional<skui::MediaPlaybackState> loop =
        runtime.videoStateById("loop-video");
    if ((intro && intro->readyState == skui::MediaReadyState::Failed) ||
        (loop && loop->readyState == skui::MediaReadyState::Failed)) {
        state.sequencePhase = SequencePhase::Failed;
        return;
    }

    if (state.sequencePhase == SequencePhase::StartingIntro && intro &&
        intro->readyState == skui::MediaReadyState::Playing) {
        setSequenceVideo(runtime, true);
        state.sequencePhase = SequencePhase::IntroPlaying;
    }
    if (state.sequencePhase != SequencePhase::IntroPlaying || !intro ||
        intro->readyState != skui::MediaReadyState::Ended) {
        return;
    }

    // loop-video 已经显式预解码；切换可见节点后，同一渲染帧直接使用其首帧。
    setSequenceVideo(runtime, false);
    if (!runtime.playVideoById("loop-video")) {
        state.sequencePhase = SequencePhase::Failed;
        return;
    }
    state.sequencePhase = SequencePhase::LoopPlaying;
}

void refreshTelemetry(skui::Runtime& runtime,
                      DemoState& state,
                      std::chrono::steady_clock::time_point now) {
    const std::optional<skui::MediaPlaybackState> intro =
        runtime.videoStateById("intro-video");
    const std::optional<skui::MediaPlaybackState> loop =
        runtime.videoStateById("loop-video");
    const std::optional<skui::MediaPlaybackState> logo =
        runtime.videoStateById("logo-video");

    std::ostringstream runtimeText;
    runtimeText << "目标 " << state.targetFrameRate << " FPS · 实测 "
                << std::fixed << std::setprecision(1)
                << state.measuredFrameRate << " FPS";

    std::string sequenceText = "开始：" + compactState(intro) +
                               "    循环：" + compactState(loop);
    std::string logoText = "LOGO：" + compactState(logo);
    std::string audioText = "音频：";
    if (!logo) {
        audioText += "未创建";
    } else if (logo->readyState == skui::MediaReadyState::Failed) {
        audioText += "失败";
    } else if (logo->hasAudio) {
        audioText += "设备主时钟 · 欠载 " +
                     std::to_string(logo->audioUnderruns) + " 次";
    } else {
        audioText += "素材没有音轨";
    }

    runtime.beginUpdate();
    runtime.setTextById("runtime-readout", runtimeText.str());
    runtime.setTextById("sequence-status", sequenceText);
    runtime.setTextById("logo-status", logoText);
    runtime.setTextById("audio-status", audioText);
    const bool sequenceStarted =
        state.sequencePhase != SequencePhase::Idle &&
        state.sequencePhase != SequencePhase::IntroPlaying &&
        state.sequencePhase != SequencePhase::IntroPaused &&
        state.sequencePhase != SequencePhase::StartingIntro;
    runtime.setTextById(
        "sequence-play",
        sequenceStarted ? "重新播放序列" : "播放开始 + 循环");
    runtime.setTextById(
        "logo-play",
        logo && logo->readyState == skui::MediaReadyState::Ended
            ? "重新播放 LOGO"
            : "播放 LOGO");
    runtime.endUpdate();
    state.lastTelemetry = now;
}

void handleRuntimeTick(skui::Runtime& runtime,
                       DemoState& state,
                       float /*deltaSeconds*/) {
    const auto now = std::chrono::steady_clock::now();
    ++state.sampleFrameCount;
    const double sampleSeconds =
        std::chrono::duration<double>(now - state.sampleStarted).count();
    if (sampleSeconds >= 0.5) {
        state.measuredFrameRate =
            static_cast<double>(state.sampleFrameCount) / sampleSeconds;
        state.sampleFrameCount = 0;
        state.sampleStarted = now;
    }

    advanceSequence(runtime, state);
    if (state.lastTelemetry == std::chrono::steady_clock::time_point::min() ||
        now - state.lastTelemetry >= kTelemetryInterval) {
        refreshTelemetry(runtime, state, now);
    }
}

void installInteractions(skui::Runtime& runtime, DemoState& state) {
    runtime.setElementEventCallback(
        [&runtime, &state](const skui::ElementEvent& event) {
            if (event.type != skui::ElementEventType::Click ||
                event.action.empty()) {
                return;
            }

            constexpr std::string_view kFrameRatePrefix = "fps-";
            if (event.action.starts_with(kFrameRatePrefix)) {
                const int value = std::stoi(
                    event.action.substr(kFrameRatePrefix.size()));
                selectFrameRate(runtime, state, value);
            } else if (event.action == "play-sequence") {
                beginSequence(runtime, state);
            } else if (event.action == "pause-sequence") {
                pauseSequence(runtime, state);
            } else if (event.action == "play-logo") {
                runtime.playVideoById("logo-video");
            } else if (event.action == "pause-logo") {
                runtime.pauseVideoById("logo-video");
            }
        });
}

bool bindMedia(skui::Runtime& runtime) {
    struct MediaBinding {
        std::string id;
        const std::filesystem::path* path = nullptr;
    };
    const std::array<MediaBinding, 3> mediaBindings{
        MediaBinding{"intro-video", &kIntroVideoPath},
        MediaBinding{"loop-video", &kLoopVideoPath},
        MediaBinding{"logo-video", &kLogoVideoPath},
    };

    std::vector<skui::AttributeUpdate> sources;
    sources.reserve(mediaBindings.size());
    for (const MediaBinding& binding : mediaBindings) {
        if (!std::filesystem::exists(*binding.path)) {
            runtime.setTextById(
                "runtime-readout",
                "找不到媒体文件：" + skui::pathToUtf8(*binding.path));
            return false;
        }
        sources.push_back({
            binding.id,
            "src",
            skui::pathToUtf8(*binding.path),
        });
    }

    runtime.beginUpdate();
    runtime.setAttributesById(sources);
    runtime.setVisibleById("intro-video", true);
    runtime.setVisibleById("loop-video", false);
    runtime.endUpdate();

    bool prepared = true;
    for (const MediaBinding& binding : mediaBindings) {
        prepared = runtime.prepareVideoById(binding.id) && prepared;
    }
    return prepared;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    DemoState state;
    skui::win32::WindowOptions options;
    options.title = L"SkiaUI Video Playback Lab";
    options.logicalWidth = 1440;
    options.logicalHeight = 860;
    options.clearColor = colorRefFromSkColor(kClearColor);
    options.runtime.clearColor = kClearColor;
    options.runtime.mediaPlayerFactory = skui::ffmpeg::makeMediaPlayerFactory(
        skui::win32::makeWasapiAudioOutputFactory());
    options.runtime.videoPredecodeFrames = 8;
    options.onRuntimeReady = [&state](skui::Runtime& runtime) {
        installInteractions(runtime, state);
        if (!runtime.loadDocument(defaultDocumentPath())) {
            OutputDebugStringA(runtime.lastError().c_str());
            MessageBoxW(nullptr,
                        L"无法加载 video_demo.html，请检查 assets 目录。",
                        L"SkiaVideoDemo",
                        MB_OK | MB_ICONERROR);
            return;
        }
        (void)bindMedia(runtime);
    };
    options.onRuntimeTick = [&state](skui::Runtime& runtime,
                                     float deltaSeconds) {
        handleRuntimeTick(runtime, state, deltaSeconds);
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    state.app = &app;
    app.setFrameRateLimit(state.targetFrameRate);
    return app.run(instance, showCmd);
}
