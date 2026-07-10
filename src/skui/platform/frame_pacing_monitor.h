#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

namespace skui::win32 {

class FramePacingMonitor {
public:
    ~FramePacingMonitor();

    void initialize(HWND hwnd);
    void noteWheelInput();
    void notePointerDragInput();
    void noteRedrawRequest(bool posted);
    void noteRedrawDispatch();
    void noteFramePresented(int width,
                            int height,
                            double frameWorkMs,
                            const char* backend);
    void finalize(int width, int height);

private:
    using Clock = std::chrono::steady_clock;

    static int64_t clockTicks(Clock::time_point time);
    static Clock::time_point timeFromTicks(int64_t ticks);
    static double elapsedMs(Clock::time_point start,
                            Clock::time_point stop);

    double queryRefreshPeriodMs(HWND hwnd) const;
    int estimateMissedFrames(double intervalMs) const;
    void noteScrollInput(const char* event);
    void writeEvent(const char* event,
                    int width,
                    int height,
                    double durationMs,
                    const std::string& detail = {});
    void writeSummary(int width,
                      int height,
                      const char* event);
    void resetSummary(Clock::time_point now);

    bool enabled_ = false;
    std::FILE* file_ = nullptr;
    bool flushEveryWrite_ = false;
    Clock::time_point traceStart_;
    double refreshPeriodMs_ = 1000.0 / 60.0;
    Clock::time_point previousPresent_;
    bool hasPreviousPresent_ = false;
    bool continuousFrameExpected_ = false;
    Clock::time_point lastScrollInput_;
    bool scrollSessionActive_ = false;
    Clock::time_point firstPendingScrollInput_;
    Clock::time_point latestPendingScrollInput_;
    uint64_t pendingScrollEvents_ = 0;
    double lastRedrawQueueMs_ = 0.0;
    std::atomic<int64_t> redrawPostedTicks_{0};
    std::atomic<uint64_t> redrawPostCount_{0};
    std::atomic<uint64_t> redrawCoalescedCount_{0};
    Clock::time_point summaryStart_;
    uint64_t summaryFrames_ = 0;
    uint64_t summaryInputFrames_ = 0;
    uint64_t summaryMissedFrames_ = 0;
    uint64_t summaryLateFrames_ = 0;
    double summaryTotalIntervalMs_ = 0.0;
    double summaryWorstIntervalMs_ = 0.0;
    double summaryWorstFrameWorkMs_ = 0.0;
    double summaryWorstInputLatencyMs_ = 0.0;
    int lastWidth_ = 0;
    int lastHeight_ = 0;
};

}  // namespace skui::win32
