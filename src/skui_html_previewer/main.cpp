#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_win32_app.h"

#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkPngEncoder.h"

#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr SkColor kPreviewClearColor = SK_ColorWHITE;
constexpr UINT kOpenInitialDocumentMessage = WM_APP + 0x542;
constexpr UINT_PTR kOpenDocumentCommand = 41001;
constexpr UINT_PTR kReloadDocumentCommand = 41002;
constexpr UINT_PTR kSaveScreenshotCommand = 41003;
constexpr UINT_PTR kExitCommand = 41004;
constexpr std::string_view kEmptyDocument = R"html(
<!doctype html>
<html>
<head>
  <style>
    html, body {
      width: 100%;
      height: 100%;
      margin: 0;
      background: #ffffff;
    }
  </style>
</head>
<body></body>
</html>
)html";

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::string utf8FromWide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(
        CP_UTF8,
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

    std::string value(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        value.data(),
        bytes,
        nullptr,
        nullptr);
    return value;
}

std::wstring wideFromUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int characters = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (characters <= 0) {
        return {};
    }

    std::wstring value(static_cast<size_t>(characters), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        value.data(),
        characters);
    return value;
}

bool saveRuntimeScreenshot(skui::Runtime& runtime,
                           const std::filesystem::path& outputPath,
                           std::string& error) {
    const int width = std::max(1, runtime.width());
    const int height = std::max(1, runtime.height());
    const size_t rowBytes = static_cast<size_t>(width) * sizeof(uint32_t);
    std::vector<uint32_t> pixels(
        static_cast<size_t>(width) * static_cast<size_t>(height));
    if (!runtime.renderToBgraPixels(
            pixels.data(),
            width,
            height,
            rowBytes,
            runtime.dpiScale())) {
        error = runtime.lastError();
        return false;
    }

    const SkImageInfo imageInfo = SkImageInfo::Make(
        width,
        height,
        kBGRA_8888_SkColorType,
        kPremul_SkAlphaType);
    const SkPixmap pixmap(imageInfo, pixels.data(), rowBytes);
    SkPngEncoder::Options encoderOptions;
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, encoderOptions);
    if (!png) {
        error = "Skia PNG encoding failed";
        return false;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "failed to open screenshot output";
        return false;
    }
    output.write(
        static_cast<const char*>(png->data()),
        static_cast<std::streamsize>(png->size()));
    if (!output) {
        error = "failed to write screenshot output";
        return false;
    }
    return true;
}

class PreviewController {
public:
    explicit PreviewController(std::optional<std::filesystem::path> initialPath)
        : initialPath_(std::move(initialPath)) {}

    void prepareRuntime(skui::Runtime& runtime) const {
        runtime.loadDocumentFromString(kEmptyDocument);
    }

    bool handleWindowMessage(HWND hwnd,
                             UINT message,
                             WPARAM wParam,
                             LPARAM lParam,
                             skui::Runtime& runtime) {
        switch (message) {
        case WM_CREATE:
            installMenu(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            PostMessageW(hwnd, kOpenInitialDocumentMessage, 0, 0);
            return true;
        case kOpenInitialDocumentMessage:
            if (initialPath_) {
                loadDocument(hwnd, runtime, *initialPath_);
                initialPath_.reset();
            } else {
                openDocument(hwnd, runtime);
            }
            return true;
        case WM_COMMAND:
            return handleCommand(hwnd, LOWORD(wParam), runtime);
        case WM_KEYDOWN:
            if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                openDocument(hwnd, runtime);
                return true;
            }
            if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                saveScreenshot(hwnd, runtime);
                return true;
            }
            if (wParam == VK_F5) {
                reloadDocument(hwnd, runtime);
                return true;
            }
            return false;
        case WM_DROPFILES:
            loadDroppedDocument(
                hwnd,
                runtime,
                reinterpret_cast<HDROP>(wParam));
            return true;
        default:
            return false;
        }
    }

private:
    std::optional<std::filesystem::path> initialPath_;
    std::optional<std::filesystem::path> currentPath_;

    static void installMenu(HWND hwnd) {
        HMENU menu = CreateMenu();
        HMENU fileMenu = CreatePopupMenu();
        if (!menu || !fileMenu) {
            if (fileMenu) {
                DestroyMenu(fileMenu);
            }
            if (menu) {
                DestroyMenu(menu);
            }
            return;
        }

        AppendMenuW(
            fileMenu,
            MF_STRING,
            kOpenDocumentCommand,
            L"打开 HTML...\tCtrl+O");
        AppendMenuW(
            fileMenu,
            MF_STRING,
            kReloadDocumentCommand,
            L"重新加载\tF5");
        AppendMenuW(
            fileMenu,
            MF_STRING,
            kSaveScreenshotCommand,
            L"保存渲染截图...\tCtrl+S");
        AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(fileMenu, MF_STRING, kExitCommand, L"退出");
        AppendMenuW(
            menu,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(fileMenu),
            L"文件");
        if (!SetMenu(hwnd, menu)) {
            DestroyMenu(menu);
        }
    }

    bool handleCommand(HWND hwnd,
                       UINT command,
                       skui::Runtime& runtime) {
        if (command == kOpenDocumentCommand) {
            openDocument(hwnd, runtime);
            return true;
        }
        if (command == kReloadDocumentCommand) {
            reloadDocument(hwnd, runtime);
            return true;
        }
        if (command == kSaveScreenshotCommand) {
            saveScreenshot(hwnd, runtime);
            return true;
        }
        if (command == kExitCommand) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return true;
        }
        return false;
    }

    void openDocument(HWND hwnd, skui::Runtime& runtime) {
        std::array<wchar_t, 32768> pathBuffer{};
        std::wstring initialDirectory;
        if (currentPath_) {
            initialDirectory = currentPath_->parent_path().native();
        }

        static constexpr wchar_t kHtmlFilter[] =
            L"HTML 文件 (*.html;*.htm)\0*.html;*.htm\0"
            L"所有文件 (*.*)\0*.*\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd;
        dialog.lpstrFilter = kHtmlFilter;
        dialog.lpstrFile = pathBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
        dialog.lpstrInitialDir = initialDirectory.empty()
                                     ? nullptr
                                     : initialDirectory.c_str();
        dialog.lpstrDefExt = L"html";
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                       OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
            loadDocument(
                hwnd,
                runtime,
                std::filesystem::path(pathBuffer.data()));
        }
    }

    void reloadDocument(HWND hwnd, skui::Runtime& runtime) {
        if (!currentPath_) {
            openDocument(hwnd, runtime);
            return;
        }
        loadDocument(hwnd, runtime, *currentPath_);
    }

    void saveScreenshot(HWND hwnd, skui::Runtime& runtime) const {
        std::array<wchar_t, 32768> pathBuffer{};
        const std::wstring defaultName = currentPath_
            ? currentPath_->stem().native() + L".png"
            : L"skui-preview.png";
        std::copy_n(
            defaultName.data(),
            std::min(defaultName.size(), pathBuffer.size() - 1),
            pathBuffer.data());

        std::wstring initialDirectory;
        if (currentPath_) {
            initialDirectory = currentPath_->parent_path().native();
        }
        static constexpr wchar_t kPngFilter[] =
            L"PNG 图片 (*.png)\0*.png\0"
            L"所有文件 (*.*)\0*.*\0";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd;
        dialog.lpstrFilter = kPngFilter;
        dialog.lpstrFile = pathBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
        dialog.lpstrInitialDir = initialDirectory.empty()
                                     ? nullptr
                                     : initialDirectory.c_str();
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT |
                       OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) {
            return;
        }

        std::string error;
        const std::filesystem::path outputPath(pathBuffer.data());
        if (!saveRuntimeScreenshot(runtime, outputPath, error)) {
            const std::wstring message =
                L"无法保存渲染截图：\n" + outputPath.native() +
                L"\n\n" + wideFromUtf8(error);
            MessageBoxW(
                hwnd,
                message.c_str(),
                L"Skia HTML 预览器",
                MB_OK | MB_ICONERROR);
        }
    }

    void loadDroppedDocument(HWND hwnd,
                             skui::Runtime& runtime,
                             HDROP drop) {
        std::array<wchar_t, 32768> pathBuffer{};
        const UINT length = DragQueryFileW(
            drop,
            0,
            pathBuffer.data(),
            static_cast<UINT>(pathBuffer.size()));
        DragFinish(drop);
        if (length == 0 || length >= pathBuffer.size()) {
            return;
        }
        loadDocument(
            hwnd,
            runtime,
            std::filesystem::path(pathBuffer.data()));
    }

    bool loadDocument(HWND hwnd,
                      skui::Runtime& runtime,
                      const std::filesystem::path& requestedPath) {
        std::error_code error;
        std::filesystem::path path =
            std::filesystem::absolute(requestedPath, error);
        if (error) {
            path = requestedPath;
        }
        path = path.lexically_normal();

        if (!runtime.loadDocument(utf8FromWide(path.native()))) {
            const std::wstring message =
                L"无法加载 HTML：\n" + path.native() + L"\n\n" +
                wideFromUtf8(runtime.lastError());
            MessageBoxW(
                hwnd,
                message.c_str(),
                L"Skia HTML 预览器",
                MB_OK | MB_ICONERROR);
            return false;
        }

        currentPath_ = path;
        std::wstring title = L"Skia HTML 预览器 - ";
        title += path.filename().native();
        SetWindowTextW(hwnd, title.c_str());
        return true;
    }
};

struct PreviewArguments {
    std::optional<std::filesystem::path> documentPath;
    std::optional<std::filesystem::path> screenshotPath;
    int width = 1280;
    int height = 800;
};

std::optional<int> parsePositiveInteger(std::wstring_view text) {
    std::wstring value(text);
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    if (end != value.c_str() + value.size() || parsed <= 0 ||
        parsed > 16384) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

PreviewArguments commandLineArguments() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(
        GetCommandLineW(),
        &argumentCount);
    if (!arguments) {
        return {};
    }

    PreviewArguments result;
    for (int index = 1; index < argumentCount; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--screenshot" && index + 1 < argumentCount) {
            result.screenshotPath =
                std::filesystem::path(arguments[++index]);
        } else if (argument == L"--width" && index + 1 < argumentCount) {
            if (std::optional<int> width =
                    parsePositiveInteger(arguments[++index])) {
                result.width = *width;
            }
        } else if (argument == L"--height" && index + 1 < argumentCount) {
            if (std::optional<int> height =
                    parsePositiveInteger(arguments[++index])) {
                result.height = *height;
            }
        } else if (!argument.starts_with(L'-') && !result.documentPath) {
            result.documentPath = std::filesystem::path(argument);
        }
    }
    LocalFree(arguments);
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    const PreviewArguments arguments = commandLineArguments();
    if (arguments.screenshotPath) {
        if (!arguments.documentPath) {
            return 2;
        }
        skui::RuntimeOptions runtimeOptions;
        runtimeOptions.clearColor = kPreviewClearColor;
        skui::Runtime runtime(std::move(runtimeOptions));
        runtime.resize(arguments.width, arguments.height, 1.0f);
        if (!runtime.loadDocument(
                utf8FromWide(arguments.documentPath->native()))) {
            return 3;
        }
        std::string error;
        return saveRuntimeScreenshot(
                   runtime,
                   *arguments.screenshotPath,
                   error)
            ? 0
            : 4;
    }

    PreviewController controller(arguments.documentPath);

    skui::win32::WindowOptions options;
    options.title = L"Skia HTML 预览器";
    options.logicalWidth = arguments.width;
    options.logicalHeight = arguments.height;
    options.clearColor = colorRefFromSkColor(kPreviewClearColor);
    options.runtime.clearColor = kPreviewClearColor;
    options.onRuntimeReady = [&controller](skui::Runtime& runtime) {
        controller.prepareRuntime(runtime);
    };
    options.onWindowMessage = [&controller](HWND hwnd,
                                             UINT message,
                                             WPARAM wParam,
                                             LPARAM lParam,
                                             skui::Runtime& runtime) {
        return controller.handleWindowMessage(
            hwnd,
            message,
            wParam,
            lParam,
            runtime);
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
