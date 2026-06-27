#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace perf {

class Trace {
public:
    using Clock = std::chrono::steady_clock;

    static bool enabled() {
        return state().file != nullptr;
    }

    static Clock::time_point now() {
        return Clock::now();
    }

    static double elapsedMs(Clock::time_point start, Clock::time_point stop) {
        return std::chrono::duration<double, std::milli>(stop - start).count();
    }

    static double elapsedMs(Clock::time_point start) {
        return elapsedMs(start, Clock::now());
    }

    static void write(const char* component,
                      const char* event,
                      int width,
                      int height,
                      double durationMs,
                      const std::string& detail = {}) {
        State& s = state();
        if (!s.file) {
            return;
        }

        const double timeMs = elapsedMs(s.start, Clock::now());
        std::lock_guard<std::mutex> lock(s.mutex);
        std::fprintf(s.file,
                     "%.3f,%lu,%lu,%s,%s,%s,%d,%d,%.3f,\"",
                     timeMs,
                     static_cast<unsigned long>(GetCurrentProcessId()),
                     static_cast<unsigned long>(GetCurrentThreadId()),
                     s.processName,
                     component ? component : "",
                     event ? event : "",
                     width,
                     height,
                     durationMs);
        for (char ch : detail) {
            if (ch == '"') {
                std::fputc('"', s.file);
            }
            std::fputc(ch, s.file);
        }
        std::fprintf(s.file, "\"\n");
        if (s.flushEveryWrite) {
            std::fflush(s.file);
        }
    }

private:
    struct State {
        FILE* file = nullptr;
        Clock::time_point start = Clock::now();
        std::mutex mutex;
        char processName[MAX_PATH] = "process";
        bool flushEveryWrite = false;

        State() {
            char modulePath[MAX_PATH]{};
            const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
            if (length > 0) {
                const char* slash = std::strrchr(modulePath, '\\');
                std::snprintf(processName, sizeof(processName), "%s", slash ? slash + 1 : modulePath);
            }

            char* path = nullptr;
            size_t pathLength = 0;
            if (_dupenv_s(&path, &pathLength, "SKIATEST_PERF_CSV") != 0 || !path || path[0] == '\0') {
                std::free(path);
                return;
            }

            if (fopen_s(&file, path, "ab") == 0 && file) {
                if (_ftelli64(file) == 0) {
                    std::fprintf(file,
                                 "time_ms,pid,tid,process,component,event,width,height,duration_ms,detail\n");
                    std::fflush(file);
                }
            }
            std::free(path);

            char* flush = nullptr;
            size_t flushLength = 0;
            if (_dupenv_s(&flush, &flushLength, "SKIATEST_PERF_FLUSH") == 0 && flush) {
                flushEveryWrite = flush[0] == '1' || flush[0] == 't' || flush[0] == 'T' ||
                                  flush[0] == 'y' || flush[0] == 'Y';
            }
            std::free(flush);
        }

        ~State() {
            if (file) {
                std::fclose(file);
                file = nullptr;
            }
        }
    };

    static State& state() {
        static State s;
        return s;
    }
};

}  // namespace perf
