#include "frame_pacing_monitor.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace skui::win32 {
namespace {

constexpr double kDefaultRefreshPeriodMs = 1000.0 / 60.0;
constexpr double kDroppedFrameThreshold = 1.5;
constexpr double kLateFrameThreshold = 1.25;
constexpr double kSummaryIntervalMs = 1000.0;
constexpr double kScrollSessionTailMs = 600.0;
constexpr double kScrollInputBurstGapMs = 100.0;

}  // namespace

FramePacingMonitor::~FramePacingMonitor() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void FramePacingMonitor::initialize(HWND hwnd) {
    char* path = nullptr;
    size_t pathLength = 0;
    if (_dupenv_s(&path,
                  &pathLength,
                  "SKIATEST_FRAME_PACING_CSV") != 0 ||
        !path ||
        path[0] == '\0') {
        std::free(path);
        return;
    }

    if (fopen_s(&file_, path, "ab") != 0 || !file_) {
        std::free(path);
        return;
    }
    std::free(path);

    if (_ftelli64(file_) == 0) {
        std::fprintf(file_,
                     "time_ms,pid,tid,component,event,width,height,"
                     "duration_ms,detail\n");
        std::fflush(file_);
    }

    char* flush = nullptr;
    size_t flushLength = 0;
    if (_dupenv_s(&flush,
                  &flushLength,
                  "SKIATEST_FRAME_PACING_FLUSH") == 0 &&
        flush) {
        flushEveryWrite_ =
            flush[0] == '1' ||
            flush[0] == 't' ||
            flush[0] == 'T' ||
            flush[0] == 'y' ||
            flush[0] == 'Y';
    }
    std::free(flush);

    enabled_ = true;
    const auto now = Clock::now();
    traceStart_ = now;
    refreshPeriodMs_ = queryRefreshPeriodMs(hwnd);
    resetSummary(now);

    std::ostringstream detail;
    detail << std::fixed << std::setprecision(3)
           << "refresh_ms=" << refreshPeriodMs_
           << ";refresh_hz=" << 1000.0 / refreshPeriodMs_;
    writeEvent("monitor_started",
               0,
               0,
               refreshPeriodMs_,
               detail.str());
}

void FramePacingMonitor::noteWheelInput() {
    noteScrollInput("wheel_input");
}

void FramePacingMonitor::notePointerDragInput() {
    noteScrollInput("pointer_drag_input");
}

void FramePacingMonitor::noteScrollInput(const char* event) {
    if (!enabled_) {
        return;
    }

    const auto now = Clock::now();
    if (scrollSessionActive_ &&
        elapsedMs(lastScrollInput_, now) >
            kScrollSessionTailMs) {
        writeSummary(lastWidth_,
                     lastHeight_,
                     "scroll_summary");
        resetSummary(now);
        scrollSessionActive_ = false;
        hasPreviousPresent_ = false;
        continuousFrameExpected_ = false;
        pendingScrollEvents_ = 0;
    }
    if (!scrollSessionActive_) {
        scrollSessionActive_ = true;
        hasPreviousPresent_ = false;
        continuousFrameExpected_ = false;
        pendingScrollEvents_ = 0;
        resetSummary(now);
    }
    if (pendingScrollEvents_ > 0 &&
        elapsedMs(latestPendingScrollInput_, now) >
            kScrollInputBurstGapMs) {
        pendingScrollEvents_ = 0;
    }
    lastScrollInput_ = now;
    if (pendingScrollEvents_ == 0) {
        firstPendingScrollInput_ = now;
    }
    latestPendingScrollInput_ = now;
    ++pendingScrollEvents_;
    writeEvent(event, 0, 0, 0.0);
}

void FramePacingMonitor::noteRedrawRequest(bool posted) {
    if (!enabled_) {
        return;
    }

    if (posted) {
        redrawPostedTicks_.store(clockTicks(Clock::now()),
                                 std::memory_order_release);
        redrawPostCount_.fetch_add(1, std::memory_order_relaxed);
    } else {
        redrawCoalescedCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void FramePacingMonitor::noteRedrawDispatch() {
    if (!enabled_) {
        return;
    }

    const int64_t postedTicks =
        redrawPostedTicks_.exchange(0, std::memory_order_acq_rel);
    if (postedTicks == 0) {
        return;
    }

    if (!continuousFrameExpected_ && pendingScrollEvents_ > 0) {
        firstPendingScrollInput_ = latestPendingScrollInput_;
        pendingScrollEvents_ = 1;
    }
    lastRedrawQueueMs_ =
        elapsedMs(timeFromTicks(postedTicks), Clock::now());
    writeEvent("redraw_dispatch", 0, 0, lastRedrawQueueMs_);
}

void FramePacingMonitor::noteFramePresented(int width,
                                            int height,
                                            double frameWorkMs,
                                            const char* backend) {
    if (!enabled_) {
        return;
    }

    const auto now = Clock::now();
    lastWidth_ = width;
    lastHeight_ = height;
    if (!scrollSessionActive_) {
        return;
    }
    if (elapsedMs(lastScrollInput_, now) >
        kScrollSessionTailMs) {
        writeSummary(width, height, "scroll_summary");
        resetSummary(now);
        scrollSessionActive_ = false;
        hasPreviousPresent_ = false;
        continuousFrameExpected_ = false;
        pendingScrollEvents_ = 0;
        return;
    }

    const bool measureInterval =
        hasPreviousPresent_ && continuousFrameExpected_;
    const double intervalMs =
        measureInterval
            ? elapsedMs(previousPresent_, now)
            : 0.0;
    previousPresent_ = now;
    hasPreviousPresent_ = true;

    const int missedFrames =
        intervalMs > 0.0 ? estimateMissedFrames(intervalMs) : 0;
    const bool lateFrame =
        intervalMs > refreshPeriodMs_ * kLateFrameThreshold;
    const double firstInputLatencyMs =
        pendingScrollEvents_ > 0
            ? elapsedMs(firstPendingScrollInput_, now)
            : 0.0;
    const double latestInputLatencyMs =
        pendingScrollEvents_ > 0
            ? elapsedMs(latestPendingScrollInput_, now)
            : 0.0;
    const uint64_t redrawPosts =
        redrawPostCount_.exchange(0, std::memory_order_acq_rel);
    const uint64_t redrawCoalesced =
        redrawCoalescedCount_.exchange(0, std::memory_order_acq_rel);
    const bool nextFrameExpected =
        redrawPostedTicks_.load(std::memory_order_acquire) != 0;

    std::ostringstream detail;
    detail << std::fixed << std::setprecision(3)
           << "refresh_ms=" << refreshPeriodMs_
           << ";missed=" << missedFrames
           << ";late=" << (lateFrame ? 1 : 0)
           << ";frame_work_ms=" << frameWorkMs
           << ";input_first_latency_ms=" << firstInputLatencyMs
           << ";input_latest_latency_ms=" << latestInputLatencyMs
           << ";input_events=" << pendingScrollEvents_
           << ";redraw_queue_ms=" << lastRedrawQueueMs_
           << ";redraw_posts=" << redrawPosts
           << ";redraw_coalesced=" << redrawCoalesced
           << ";continuous_interval=" << (measureInterval ? 1 : 0)
           << ";next_frame_expected=" << (nextFrameExpected ? 1 : 0)
           << ";backend=" << (backend ? backend : "");
    writeEvent("frame_presented",
               width,
               height,
               intervalMs,
               detail.str());

    if (intervalMs > 0.0) {
        ++summaryFrames_;
        summaryTotalIntervalMs_ += intervalMs;
        summaryWorstIntervalMs_ =
            std::max(summaryWorstIntervalMs_, intervalMs);
        summaryMissedFrames_ += static_cast<uint64_t>(missedFrames);
        if (lateFrame) {
            ++summaryLateFrames_;
        }
    }
    if (pendingScrollEvents_ > 0) {
        ++summaryInputFrames_;
    }
    summaryWorstFrameWorkMs_ =
        std::max(summaryWorstFrameWorkMs_, frameWorkMs);
    summaryWorstInputLatencyMs_ =
        std::max(summaryWorstInputLatencyMs_, firstInputLatencyMs);

    pendingScrollEvents_ = 0;
    lastRedrawQueueMs_ = 0.0;
    continuousFrameExpected_ = nextFrameExpected;

    if (elapsedMs(summaryStart_, now) >=
        kSummaryIntervalMs) {
        writeSummary(width, height, "summary");
        resetSummary(now);
    }
}

void FramePacingMonitor::finalize(int width, int height) {
    if (!enabled_ || !scrollSessionActive_) {
        return;
    }
    writeSummary(width, height, "final_summary");
    std::fflush(file_);
}

int64_t FramePacingMonitor::clockTicks(Clock::time_point time) {
    return static_cast<int64_t>(time.time_since_epoch().count());
}

FramePacingMonitor::Clock::time_point
FramePacingMonitor::timeFromTicks(int64_t ticks) {
    return Clock::time_point(Clock::duration(ticks));
}

double FramePacingMonitor::elapsedMs(Clock::time_point start,
                                     Clock::time_point stop) {
    return std::chrono::duration<double, std::milli>(
               stop - start)
        .count();
}

double FramePacingMonitor::queryRefreshPeriodMs(HWND hwnd) const {
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    LARGE_INTEGER frequency{};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) &&
        timing.qpcRefreshPeriod > 0 &&
        QueryPerformanceFrequency(&frequency) &&
        frequency.QuadPart > 0) {
        return static_cast<double>(timing.qpcRefreshPeriod) * 1000.0 /
               static_cast<double>(frequency.QuadPart);
    }

    HDC hdc = GetDC(hwnd);
    const int refreshHz = hdc ? GetDeviceCaps(hdc, VREFRESH) : 0;
    if (hdc) {
        ReleaseDC(hwnd, hdc);
    }
    return refreshHz > 1
        ? 1000.0 / static_cast<double>(refreshHz)
        : kDefaultRefreshPeriodMs;
}

int FramePacingMonitor::estimateMissedFrames(double intervalMs) const {
    if (intervalMs < refreshPeriodMs_ * kDroppedFrameThreshold) {
        return 0;
    }
    return std::max(1,
                    static_cast<int>(
                        std::lround(intervalMs / refreshPeriodMs_)) -
                        1);
}

void FramePacingMonitor::writeEvent(const char* event,
                                    int width,
                                    int height,
                                    double durationMs,
                                    const std::string& detail) {
    if (!file_) {
        return;
    }

    const double timeMs = elapsedMs(traceStart_, Clock::now());
    std::fprintf(file_,
                 "%.3f,%lu,%lu,frame_pacing,%s,%d,%d,%.3f,\"",
                 timeMs,
                 static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long>(GetCurrentThreadId()),
                 event ? event : "",
                 width,
                 height,
                 durationMs);
    for (char ch : detail) {
        if (ch == '"') {
            std::fputc('"', file_);
        }
        std::fputc(ch, file_);
    }
    std::fprintf(file_, "\"\n");
    if (flushEveryWrite_) {
        std::fflush(file_);
    }
}

void FramePacingMonitor::writeSummary(int width,
                                      int height,
                                      const char* event) {
    if (summaryFrames_ == 0 && summaryInputFrames_ == 0) {
        return;
    }

    const double windowMs = summaryTotalIntervalMs_;
    const double averageIntervalMs =
        summaryTotalIntervalMs_ / static_cast<double>(summaryFrames_);
    const double fps =
        windowMs > 0.0
            ? static_cast<double>(summaryFrames_) * 1000.0 / windowMs
            : 0.0;

    std::ostringstream detail;
    detail << std::fixed << std::setprecision(3)
           << "window_ms=" << windowMs
           << ";fps=" << fps
           << ";frames=" << summaryFrames_
           << ";input_frames=" << summaryInputFrames_
           << ";late_frames=" << summaryLateFrames_
           << ";missed_frames=" << summaryMissedFrames_
           << ";average_interval_ms=" << averageIntervalMs
           << ";worst_interval_ms=" << summaryWorstIntervalMs_
           << ";worst_frame_work_ms=" << summaryWorstFrameWorkMs_
           << ";worst_input_latency_ms="
           << summaryWorstInputLatencyMs_;
    writeEvent(event,
               width,
               height,
               summaryWorstIntervalMs_,
               detail.str());
}

void FramePacingMonitor::resetSummary(Clock::time_point now) {
    summaryStart_ = now;
    summaryFrames_ = 0;
    summaryInputFrames_ = 0;
    summaryMissedFrames_ = 0;
    summaryLateFrames_ = 0;
    summaryTotalIntervalMs_ = 0.0;
    summaryWorstIntervalMs_ = 0.0;
    summaryWorstFrameWorkMs_ = 0.0;
    summaryWorstInputLatencyMs_ = 0.0;
}

}  // namespace skui::win32
