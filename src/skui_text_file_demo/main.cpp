#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_win32_app.h"

#include "include/core/SkColor.h"
#include "skui/core/skui_internal.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr SkColor kClearColor = SkColorSetRGB(7, 12, 18);

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::string defaultDocumentPath() {
    const std::filesystem::path working =
        std::filesystem::current_path() / "assets" / "skui_text_file_demo" / "text_file.html";
    if (std::filesystem::exists(working)) {
        return skui::pathToUtf8(working);
    }

    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modulePath, MAX_PATH)) {
        return "assets/skui_text_file_demo/text_file.html";
    }
    const std::filesystem::path exe = modulePath;
    const std::filesystem::path local =
        exe.parent_path() / "assets" / "skui_text_file_demo" / "text_file.html";
    if (std::filesystem::exists(local)) {
        return skui::pathToUtf8(local);
    }
    return skui::pathToUtf8(working);
}

std::string utf8FromWide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8,
                                          0,
                                          text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (bytes <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        out.data(),
                        bytes,
                        nullptr,
                        nullptr);
    return out;
}

std::string utf8FromCodePage(std::string_view text, UINT codePage) {
    if (text.empty()) {
        return {};
    }
    const int wideChars = MultiByteToWideChar(codePage,
                                              0,
                                              text.data(),
                                              static_cast<int>(text.size()),
                                              nullptr,
                                              0);
    if (wideChars <= 0) {
        return std::string(text);
    }

    std::wstring wide(static_cast<size_t>(wideChars), L'\0');
    MultiByteToWideChar(codePage,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        wide.data(),
                        wideChars);
    return utf8FromWide(wide);
}

std::string utf8FromUtf16LeBytes(std::string_view bytes) {
    std::wstring wide;
    wide.reserve(bytes.size() / 2);
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto lo = static_cast<uint16_t>(static_cast<unsigned char>(bytes[i]));
        const auto hi = static_cast<uint16_t>(static_cast<unsigned char>(bytes[i + 1]));
        wide.push_back(static_cast<wchar_t>(lo | (hi << 8)));
    }
    return utf8FromWide(wide);
}

bool isValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        if (ch < 0x80) {
            length = 1;
        } else if ((ch & 0xe0) == 0xc0) {
            length = 2;
        } else if ((ch & 0xf0) == 0xe0) {
            length = 3;
        } else if ((ch & 0xf8) == 0xf0) {
            length = 4;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (size_t j = 1; j < length; ++j) {
            const unsigned char next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

std::string bytesToDisplayText(std::string bytes) {
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf) {
        bytes.erase(0, 3);
        return bytes;
    }

    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        return utf8FromUtf16LeBytes(std::string_view(bytes).substr(2));
    }

    if (isValidUtf8(bytes)) {
        return bytes;
    }
    return utf8FromCodePage(bytes, CP_ACP);
}

std::optional<std::filesystem::path> chooseTextFile() {
    wchar_t filePath[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = filePath;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filePath));
    dialog.lpstrFilter =
        L"Text files\0*.txt;*.log;*.md;*.json;*.xml;*.csv;*.html;*.cpp;*.h;*.hpp;*.c;*.cc\0"
        L"All files\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    return std::filesystem::path(filePath);
}

std::string readBinaryFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Failed to open file.";
        return {};
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        error = "Failed to read file size.";
        return {};
    }
    file.seekg(0, std::ios::beg);

    std::string bytes(static_cast<size_t>(size), '\0');
    if (!bytes.empty()) {
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!file && !file.eof()) {
        error = "Failed to read file content.";
        return {};
    }
    return bytes;
}

size_t lineCount(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
}

std::string formatBytes(size_t bytes) {
    std::ostringstream out;
    if (bytes >= 1024 * 1024) {
        out << (bytes / (1024 * 1024)) << "."
            << ((bytes % (1024 * 1024)) * 10 / (1024 * 1024)) << " MB";
    } else if (bytes >= 1024) {
        out << (bytes / 1024) << "." << ((bytes % 1024) * 10 / 1024) << " KB";
    } else {
        out << bytes << " B";
    }
    return out.str();
}

void loadTextFile(skui::Runtime& runtime, const std::filesystem::path& path) {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    std::string bytes = readBinaryFile(path, error);
    if (!error.empty()) {
        runtime.setTextById("status-main", error);
        return;
    }

    std::string text = bytesToDisplayText(std::move(bytes));
    const size_t bytesLoaded = text.size();
    const size_t lines = lineCount(text);
    runtime.beginUpdate();
    runtime.setTextById("file-name", utf8FromWide(path.wstring()));
    runtime.setValueById("text-viewer", text);

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::ostringstream status;
    status << "Loaded " << formatBytes(bytesLoaded) << ", " << lines
           << " lines, " << elapsedMs << " ms.";
    runtime.setTextById("status-main", status.str());
    runtime.endUpdate();
}

struct BenchmarkRow {
    std::string name;
    int frames = 0;
    double totalMs = 0.0;
};

void writeBenchmarkError(const std::filesystem::path& outputPath, std::string_view message) {
    if (!outputPath.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
    }
    std::ofstream file(outputPath, std::ios::binary);
    if (file) {
        file << "error\n" << message << "\n";
    }
}

bool writeBenchmarkRows(const std::filesystem::path& outputPath,
                        const std::filesystem::path& inputPath,
                        const std::vector<BenchmarkRow>& rows) {
    if (!outputPath.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(outputPath.parent_path(), error);
        if (error) {
            return false;
        }
    }

    std::ofstream file(outputPath, std::ios::binary);
    if (!file) {
        return false;
    }

    file << "input," << skui::pathToUtf8(inputPath) << "\n";
    file << "metric,frames,total_ms,avg_ms\n";
    for (const BenchmarkRow& row : rows) {
        const double averageMs = row.frames > 0 ? row.totalMs / static_cast<double>(row.frames) : 0.0;
        file << row.name << "," << row.frames << "," << row.totalMs << "," << averageMs << "\n";
    }
    return true;
}

int runBenchmark(const std::filesystem::path& inputPath, const std::filesystem::path& outputPath) {
    if (inputPath.empty() || !std::filesystem::exists(inputPath)) {
        writeBenchmarkError(outputPath, "benchmark input file does not exist");
        return 2;
    }

    skui::RuntimeOptions runtimeOptions;
    runtimeOptions.assetRoot = "assets/skui_text_file_demo";
    runtimeOptions.clearColor = kClearColor;

    skui::Runtime runtime(std::move(runtimeOptions));
    if (!runtime.loadDocument(defaultDocumentPath())) {
        writeBenchmarkError(outputPath, runtime.lastError());
        return 3;
    }

    constexpr int kWidth = 1280;
    constexpr int kHeight = 820;
    runtime.resize(kWidth, kHeight, 1.0f);
    loadTextFile(runtime, inputPath);

    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight));
    const auto renderFrame = [&runtime, &pixels]() {
        const auto start = std::chrono::steady_clock::now();
        runtime.renderToBgraPixels(pixels.data(),
                                   kWidth,
                                   kHeight,
                                   static_cast<size_t>(kWidth) * sizeof(uint32_t),
                                   1.0f);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    };
    const auto eventAndRenderFrame = [&runtime, &renderFrame](const skui::Event& event) {
        const auto start = std::chrono::steady_clock::now();
        runtime.handleEvent(event);
        renderFrame();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    };

    std::vector<BenchmarkRow> rows;
    rows.push_back({"initial_render", 1, renderFrame()});

    constexpr int kScrollFrames = 120;
    double scrollMs = 0.0;
    for (int i = 0; i < kScrollFrames; ++i) {
        skui::Event wheel;
        wheel.type = skui::EventType::MouseWheel;
        wheel.x = 640.0f;
        wheel.y = 520.0f;
        wheel.wheelDelta = -120.0f;
        scrollMs += eventAndRenderFrame(wheel);
    }
    rows.push_back({"scroll_event_render", kScrollFrames, scrollMs});

    skui::Event down;
    down.type = skui::EventType::MouseDown;
    down.button = skui::MouseButton::Left;
    down.x = 52.0f;
    down.y = 190.0f;
    eventAndRenderFrame(down);

    constexpr int kSelectionFrames = 120;
    double selectionMs = 0.0;
    for (int i = 0; i < kSelectionFrames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSelectionFrames - 1);
        skui::Event move;
        move.type = skui::EventType::MouseMove;
        move.x = 52.0f;
        move.y = 190.0f + t * 560.0f;
        selectionMs += eventAndRenderFrame(move);
    }
    rows.push_back({"selection_drag_event_render", kSelectionFrames, selectionMs});

    skui::Event up;
    up.type = skui::EventType::MouseUp;
    up.button = skui::MouseButton::Left;
    up.x = 52.0f;
    up.y = 750.0f;
    eventAndRenderFrame(up);

    if (!writeBenchmarkRows(outputPath, inputPath, rows)) {
        return 4;
    }
    return 0;
}

void installInteractions(skui::Runtime& runtime) {
    runtime.setElementEventCallback([&runtime](const skui::ElementEvent& event) {
        if (event.type != skui::ElementEventType::Click || event.action != "open-text-file") {
            return;
        }
        if (std::optional<std::filesystem::path> path = chooseTextFile()) {
            loadTextFile(runtime, *path);
        }
    });
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && std::wcscmp(argv[1], L"--benchmark") == 0) {
        const std::filesystem::path inputPath = argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path();
        const std::filesystem::path outputPath = argc >= 4
            ? std::filesystem::path(argv[3])
            : std::filesystem::current_path() / "tmp" / "skia_text_file_benchmark.csv";
        LocalFree(argv);
        return runBenchmark(inputPath, outputPath);
    }

    std::optional<std::filesystem::path> initialPath;
    if (argv && argc >= 2) {
        initialPath = std::filesystem::path(argv[1]);
    }
    if (argv) {
        LocalFree(argv);
    }

    skui::win32::WindowOptions options;
    options.title = L"SkiaTextFileDemo";
    options.logicalWidth = 1280;
    options.logicalHeight = 820;
    options.clearColor = colorRefFromSkColor(kClearColor);
    options.documentPath = defaultDocumentPath();
    options.runtime.assetRoot = "assets/skui_text_file_demo";
    options.runtime.clearColor = kClearColor;
    options.onRuntimeReady = [initialPath](skui::Runtime& runtime) {
        installInteractions(runtime);
        if (initialPath) {
            loadTextFile(runtime, *initialPath);
        }
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
