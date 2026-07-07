#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_win32_app.h"

#include "include/core/SkColor.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr SkColor kClearColor = SkColorSetRGB(7, 12, 18);
constexpr int kTileWidth = 332;
constexpr int kTileHeight = 236;
constexpr int kImageWidth = 304;
constexpr int kImageHeight = 171;
constexpr int kGap = 18;
constexpr int kColumns = 3;

struct ImageItem {
    std::filesystem::path path;
    std::filesystem::file_time_type modified{};
};

COLORREF colorRefFromSkColor(SkColor color) {
    return RGB(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color));
}

std::filesystem::path defaultImageFolder() {
    return std::filesystem::path(L"C:\\Users\\wap80\\Pictures\\Screenshots");
}

std::string htmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string pathText(const std::filesystem::path& path) {
    return path.string();
}

std::string widePathText(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return pathText(path);
    }

    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string lowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool isSupportedImage(const std::filesystem::path& path) {
    const std::string ext = lowerAscii(path.extension().string());
    return ext == ".png" ||
           ext == ".jpg" ||
           ext == ".jpeg" ||
           ext == ".webp" ||
           ext == ".bmp" ||
           ext == ".gif" ||
           ext == ".ico";
}

std::vector<ImageItem> loadImages(const std::filesystem::path& folder) {
    std::vector<ImageItem> images;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        return images;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec || !isSupportedImage(entry.path())) {
            ec.clear();
            continue;
        }
        images.push_back({entry.path(), entry.last_write_time(ec)});
        ec.clear();
    }

    std::sort(images.begin(), images.end(), [](const ImageItem& a, const ImageItem& b) {
        if (a.modified == b.modified) {
            return a.path.filename().string() < b.path.filename().string();
        }
        return a.modified > b.modified;
    });
    return images;
}

std::string buildImageDocument(const std::filesystem::path& folder) {
    const std::vector<ImageItem> images = loadImages(folder);
    const size_t rows = images.empty() ? 1 : (images.size() + kColumns - 1) / kColumns;
    const int virtualHeight = std::max(520, kGap + static_cast<int>(rows) * (kTileHeight + kGap));

    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><style>";
    html << ".screen{flex-grow:1;position:relative;background:linear-gradient(to bottom,#07111c,#0b1724);"
            "color:rgba(238,246,252,0.94);font-size:16px;}";
    html << ".toolbar{position:absolute;left:24px;top:20px;right:24px;height:90px;"
            "background-color:rgba(14,28,43,0.86);border-color:rgba(128,164,192,0.45);"
            "border-width:1px;border-style:solid;border-radius:8px;}";
    html << ".title{position:absolute;left:22px;top:12px;width:420px;height:30px;font-size:24px;font-weight:bold;}";
    html << ".subtitle{position:absolute;left:22px;top:50px;right:22px;height:24px;"
            "color:rgba(186,204,220,0.86);font-size:15px;}";
    html << ".status{position:absolute;left:24px;top:122px;right:24px;height:30px;"
            "color:rgba(190,209,225,0.88);font-size:15px;}";
    html << ".viewer{position:absolute;left:24px;top:160px;right:24px;bottom:24px;overflow:auto;"
            "overflow-x:hidden;overflow-y:auto;scrollbar-gutter:stable;background-color:rgba(4,11,18,0.72);"
            "border-color:rgba(128,164,192,0.48);border-width:1px;border-style:solid;border-radius:8px;}";
    html << ".tile{position:absolute;background-color:rgba(16,31,45,0.88);border-color:rgba(113,148,174,0.46);"
            "border-width:1px;border-style:solid;border-radius:8px;}";
    html << ".thumb{position:absolute;left:14px;top:14px;width:" << kImageWidth << "px;height:" << kImageHeight
         << "px;border-radius:5px;background-color:rgba(2,8,14,0.9);}";
    html << ".name{position:absolute;left:14px;top:194px;right:14px;height:22px;font-size:14px;"
            "color:rgba(238,246,252,0.94);}";
    html << ".empty{position:absolute;left:28px;top:28px;width:900px;height:28px;color:rgba(238,246,252,0.9);}";
    html << "</style></head><body><div class=\"screen\">";
    html << "<div class=\"toolbar\"><div class=\"title\">Image Scroller VRAM Demo</div>";
    html << "<div class=\"subtitle\">" << htmlEscape(widePathText(folder)) << "</div></div>";
    html << "<div class=\"status\">" << images.size()
         << " images. Use the mouse wheel to scroll; only visible tiles should enter the draw path.</div>";
    html << "<div class=\"viewer\" data-virtual-height=\"" << virtualHeight << "\">";

    if (images.empty()) {
        html << "<div class=\"empty\">No supported images found in this folder.</div>";
    }

    for (size_t i = 0; i < images.size(); ++i) {
        const int column = static_cast<int>(i % kColumns);
        const int row = static_cast<int>(i / kColumns);
        const int left = kGap + column * (kTileWidth + kGap);
        const int top = kGap + row * (kTileHeight + kGap);
        const std::string fileName = widePathText(images[i].path.filename());
        html << "<div class=\"tile\" style=\"left:" << left << "px;top:" << top << "px;width:" << kTileWidth
             << "px;height:" << kTileHeight << "px;\">";
        html << "<img class=\"thumb\" src=\"" << htmlEscape(fileName) << "\">";
        html << "<div class=\"name\">" << htmlEscape(fileName) << "</div></div>";
    }

    html << "</div></div></body></html>";
    return html.str();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path folder = defaultImageFolder();
    if (argv && argc >= 2) {
        folder = std::filesystem::path(argv[1]);
    }
    if (argv) {
        LocalFree(argv);
    }

    skui::win32::WindowOptions options;
    options.title = L"SkiaImageScrollerDemo";
    options.logicalWidth = 1120;
    options.logicalHeight = 860;
    options.clearColor = colorRefFromSkColor(kClearColor);
    options.runtime.clearColor = kClearColor;
    options.onRuntimeResize = [folder, loaded = false](skui::Runtime& runtime) mutable {
        if (loaded) {
            return;
        }
        loaded = runtime.loadDocumentFromString(buildImageDocument(folder), widePathText(folder));
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
