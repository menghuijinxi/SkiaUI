# SkUI 项目集成指南

这份文档说明如何把 `Skui` 集成到其他 CMake/C++ 项目里。SkUI 核心只依赖 Skia、Skia SVG 模块、Yoga 和 Lexbor；Win32 + DX12 宿主是可选层。

## 模块边界

当前代码分成两个可复用 target：

| Target | 作用 | 适用场景 |
| --- | --- | --- |
| `Skui` | HTML/CSS 解析、DOM、样式、Yoga 布局、事件、Skia 绘制 | 已经有窗口和 Skia canvas 的项目 |
| `SkuiWin32Dx12` | Win32 窗口、DX12 swapchain、鼠标键盘 IME、剪贴板、光标适配 | Windows 原生项目快速拉起一个 SkUI 窗口 |

Demo target：

- `SkiaUiDesk`：SkUI 自制 CSS + DOM 功能 demo。
- `SkiaRelayDeskDemo`：RelayDesk 界面 demo。
- `SkuiInteractionTests`：运行时行为回归测试。

## 依赖

项目需要 C++23，以及这些 vcpkg 包：

```json
{
  "dependencies": [
    {
      "name": "skia",
      "default-features": false,
      "features": ["direct3d", "gl"]
    },
    "lexbor",
    "yoga"
  ]
}
```

如果只接入 `Skui`，不需要 Win32/DX12 相关系统库。接入 `SkuiWin32Dx12` 时需要 Windows SDK，并链接 `user32`、`gdi32`、`dwmapi`、`shcore`、`d3d12`、`dxgi`、`dxguid`、`d3dcompiler`、`dbghelp`、`ole32`、`imm32`、`shell32`、`usp10`、`windowscodecs`。

## 方案一：直接复制源码接入

适合当前阶段。把这些目录复制到目标项目：

```text
src/skui/public/
src/skui/core/
src/skui/render/
```

如果需要内置 Win32/DX12 宿主，再复制：

```text
src/skui/platform/
src/d3d_presenter.h
src/d3d_presenter.cpp
src/perf_trace.h
```

目标项目 CMake：

```cmake
find_package(unofficial-skia CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)
find_package(lexbor CONFIG REQUIRED)

add_library(Skui STATIC
    src/skui/core/skui_layout.cpp
    src/skui/core/skui_parser.cpp
    src/skui/core/skui_runtime.cpp
    src/skui/core/skui_utils.cpp
    src/skui/render/skui_skia_renderer.cpp
)

target_include_directories(Skui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
)

target_include_directories(Skui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/core"
)

target_link_libraries(Skui PUBLIC
    unofficial::skia::skia
    unofficial::skia::modules::svg
    yoga::yogacore
)

target_link_libraries(Skui PRIVATE
    lexbor::lexbor_static
)

target_compile_definitions(Skui PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(Skui PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
endif()
```

## 方案二：作为子目录接入

如果目标项目能直接引用本仓库，可以把本仓库作为 submodule 或源码目录，然后在顶层 `CMakeLists.txt` 使用：

```cmake
add_subdirectory(external/SkiaTest)
target_link_libraries(MyApp PRIVATE Skui)
```

这个方式会同时带入本仓库 demo target。后续如果要做成更干净的库，建议增加 `SKUI_BUILD_DEMOS` / `SKUI_BUILD_TESTS` 选项，把 demo 从库构建里隔离出去。

## 方案三：使用 Win32/DX12 宿主

目标项目如果只是想快速打开一个 SkUI 窗口，可以链接 `SkuiWin32Dx12`：

```cpp
#include "skui_win32_app.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    skui::win32::WindowOptions options;
    options.title = L"MySkuiApp";
    options.logicalWidth = 1280;
    options.logicalHeight = 800;
    options.documentPath = "assets/ui/main.html";
    options.runtime.assetRoot = "assets/ui";
    options.onRuntimeReady = [](skui::Runtime& ui) {
        ui.setElementEventCallback([&ui](const skui::ElementEvent& event) {
            if (event.type == skui::ElementEventType::Click && event.action == "save") {
                ui.addClassById("save-button", "done");
            }
        });
    };

    skui::win32::Dx12WindowApp app(std::move(options));
    return app.run(instance, showCmd);
}
```

## 方案四：只接入运行时和自己的渲染循环

如果目标项目已经有窗口、消息循环和 Skia `SkCanvas`，只需要 `Skui`：

```cpp
#include "skui_runtime.h"

skui::RuntimeOptions options;
options.assetRoot = "assets/ui";
options.clearColor = SK_ColorBLACK;
options.readClipboardText = [] { return std::string{}; };
options.writeClipboardText = [](std::string_view text) {};

skui::Runtime ui(options);
ui.setElementEventCallback([&](const skui::ElementEvent& event) {
    if (event.type == skui::ElementEventType::Scroll &&
        event.action == "large-table-scroll") {
        // 根据 event.scrollX / event.scrollY 更新池化行和列。
    }
});

ui.loadDocument("assets/ui/main.html");

// 窗口尺寸变化：
ui.resize(width, height, dpiScale);

// 鼠标、键盘、IME 转成 skui::Event：
skui::Event e;
e.type = skui::EventType::MouseMove;
e.x = mouseX;
e.y = mouseY;
ui.handleEvent(e);

// 每帧绘制：
ui.render(canvas);
```

`Runtime::renderToBgraPixels` 可用于离屏截图、自动化测试和视觉回归。

## 平台事件适配

自定义宿主需要把平台事件转成 `skui::Event`：

| SkUI 事件 | 来源示例 |
| --- | --- |
| `MouseMove` | 鼠标移动 |
| `MouseLeave` | 鼠标离开窗口 |
| `MouseDown` / `MouseUp` | 鼠标按下和抬起 |
| `MouseDoubleClick` | 双击 |
| `MouseWheel` | 滚轮，`wheelDelta` 使用 Win32 风格 120 单位 |
| `KeyDown` / `KeyUp` | 键盘虚拟键 |
| `TextInput` | 文本输入字符，UTF-8 |
| `ImeComposition` | 输入法组合字符串 |
| `ImeEnd` | 输入法结束 |

事件坐标应使用 SkUI 逻辑坐标。Win32 宿主内部会处理 DPI；自定义宿主要保证传入坐标和 `resize(width, height, dpiScale)` 的约定一致。

## 业务交互推荐写法

HTML：

```html
<button id="save-button" data-action="save">保存</button>
<div id="large-table"
     class="table-viewport"
     data-action="large-table-scroll"
     data-virtual-width="12000"
     data-virtual-height="4000000"></div>
```

C++：

```cpp
ui.setElementEventCallback([&ui](const skui::ElementEvent& event) {
    if (event.action == "save" && event.type == skui::ElementEventType::Click) {
        ui.addClassById("save-button", "done");
    }

    if (event.action == "large-table-scroll" &&
        event.type == skui::ElementEventType::Scroll) {
        skui::RuntimeUpdates updates;
        // 填充 visible row pool 的 text/style/attribute 更新。
        ui.applyUpdates(updates);
    }
});
```

注意：不要依赖完整 JavaScript 或浏览器 DOM API。SkUI 的动态能力来自 C++ 回调和运行时更新接口。

## 大量数据：虚拟滚动 / 窗口化渲染

SkUI 支持浏览器常见的窗口化思路，但业务层需要负责池化节点内容更新。

基本做法：

1. 滚动容器设置固定 `width` / `height` 和 `overflow:auto`。
2. 通过 `data-virtual-width` / `data-virtual-height` 声明完整内容尺寸。
3. DOM 中只放可见范围附近的少量 row/cell 节点。
4. 监听 `Scroll` 事件，根据 `scrollX` / `scrollY` 计算首行首列。
5. 使用 `applyUpdates` 批量更新池化节点的 `top`、`left`、文本和 `data-action`。

`data-virtual-width` / `data-virtual-height` 是滚动范围的权威尺寸。设置后，SkUI 不再用池化节点的实际位置扩大内容尺寸，所以可以把少量节点反复移动到当前可见区域，而不会因为某个复用节点的旧坐标把滚动条撑乱。

示例：

```html
<div id="table"
     class="table"
     data-action="table-scroll"
     data-virtual-width="20000"
     data-virtual-height="4000000">
  <div id="row-0" class="row" style="position:absolute;left:0;top:0"></div>
  <div id="row-1" class="row" style="position:absolute;left:0;top:40px"></div>
</div>
```

```css
.table {
  width: 800px;
  height: 360px;
  overflow: auto;
  scrollbar-gutter: stable;
  position: relative;
}
```

这个模式适合聊天记录、大表格、大列表。不要为几十万行真实创建几十万个 DOM 节点。

池化节点更新建议放进同一个 `RuntimeUpdates` 批次：样式负责移动和显隐，文本负责替换可见内容，属性负责更新 `data-action` 或虚拟尺寸。`setStyleById` 和批量样式更新会替换节点完整内联 `style`，因此业务侧应提交该节点当前需要的完整内联声明。

## 资源路径

`RuntimeOptions::assetRoot` 用于解析相对资源路径。`img src="icons/a.svg"` 查找顺序：

1. HTML 文件所在目录。
2. `RuntimeOptions::assetRoot`。
3. 原始路径。

Demo 构建后会把 `assets/skui_demo` 和 `assets/skui_relay_demo` 复制到 exe 目录。其他项目也应该在构建后复制自己的 HTML、SVG 等资源。

## 离屏截图和测试

`Runtime::renderToBgraPixels` 可以把 UI 渲染到 BGRA 内存，适合做像素级测试：

```cpp
std::vector<uint32_t> pixels(width * height);
ui.renderToBgraPixels(pixels.data(), width, height, width * sizeof(uint32_t), 1.0f);
```

本仓库的回归测试入口：

```powershell
cmake --build --preset ninja-vcpkg-release --target SkuiInteractionTests
.\build\ninja-vcpkg\SkuiInteractionTests.exe
```

如果网络下载慢，可以在构建前设置代理：

```powershell
$env:http_proxy = "http://127.0.0.1:10090"
$env:https_proxy = "http://127.0.0.1:10090"
```

## 编码约定

- HTML、CSS、C++ 源码建议统一 UTF-8。
- MSVC 编译建议开启 `/utf-8`。
- SkUI 文本值内部使用 UTF-8。
- 输入法、剪贴板等平台文本需要在平台适配层转换为 UTF-8。

## 接入检查清单

- `Skui` 源码和 public 头文件已加入目标项目。
- Skia、Skia SVG、Yoga、Lexbor 都能被 CMake 找到。
- 资源目录会随构建复制到运行目录，或 `assetRoot` 指到正确位置。
- 平台事件已转成 `skui::Event`。
- 剪贴板读写回调已设置；使用 `SkuiWin32Dx12` 时默认已设置。
- 鼠标光标读取 `Runtime::cursor()` 并映射到平台光标；使用 `SkuiWin32Dx12` 时默认已设置。
- 大量数据界面使用虚拟滚动，不创建全量 DOM 节点。
- 使用 `applyUpdates` 批量更新，避免一帧内多次重复布局。
