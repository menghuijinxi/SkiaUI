# SkUI 项目集成指南

这份文档说明如何把 `Skui` 集成到其他 CMake/C++ 项目里。SkUI 核心只依赖 Skia、Skia SVG 模块、Yoga 和 Lexbor；Win32 事件适配和 Win32 + DX12 宿主都是可选层。

## 模块边界

当前代码分成三个可复用 target：

| Target | 作用 | 适用场景 |
| --- | --- | --- |
| `Skui` | HTML/CSS 解析、DOM、样式、Yoga 布局、事件、Skia 绘制 | 已经有窗口和 Skia canvas 的项目 |
| `SkuiWin32` | Win32 消息、鼠标键盘、IME、剪贴板、光标适配 | Windows 上已有 OpenGL、Vulkan、DX12 或 CPU raster 宿主，只想复用平台事件转换 |
| `SkuiWin32Dx12` | Win32 窗口、DX12 swapchain、D3D presenter | Windows 原生项目快速拉起一个 SkUI DX12 窗口 |

`SkuiWin32Dx12` 只是当前仓库自带的一个宿主示例，不代表 SkUI 只能运行在 DX12 上。SkUI 核心 target 不创建窗口、不管理 swapchain，也不直接依赖 Win32/DX12；它只要求宿主在合适的时机提供一个有效的 `SkCanvas`，并把平台输入事件转成 `skui::Event`。Windows 平台的 OpenGL、Vulkan 或自研渲染后端可以链接 `SkuiWin32` 复用通用 Win32 事件适配，再自己管理 GPU 后端。

Demo target：

- `SkiaUiDesk`：SkUI 自制 CSS + DOM 功能 demo。
- `SkiaRelayDeskDemo`：RelayDesk 界面 demo。
- `SkiaDynamicDomDemo`：动态 DOM 增删、替换、显示/隐藏和滚动布局 demo。
- `SkuiInteractionTests`：运行时行为回归测试。

## 依赖

项目需要 C++23，以及这些 vcpkg 包。依赖必须由目标项目自己拉取和配置，不要直接复用本仓库 `build` 目录里已经编译好的 `Skui.lib`、`SkuiWin32.lib`、`SkuiWin32Dx12.lib` 或 Skia/Yoga/Lexbor 等第三方 `.lib`。

原因是 SkUI 的第三方依赖和目标项目的渲染后端、资源格式、运行平台强相关。目标项目可能需要 Direct3D、OpenGL、Vulkan、CPU raster、不同图片 codec 或不同 CRT/编译参数；如果直接链接本项目预编译产物，这些选择会被本项目的构建方式固定住，后续也容易出现 ABI、运行库、Debug/Release、DPI/后端特性不一致的问题。

推荐做法：

- SkUI 以源码 target 的形式进入目标项目。
- Skia、Yoga、Lexbor 由目标项目的包管理方案拉取，例如 vcpkg manifest、submodule、Conan 或目标项目已有的依赖系统。
- SkUI 的 CMake 只使用 `find_package` / `target_link_libraries` 绑定目标项目解析出来的依赖 target。
- 目标项目根据自己的后端选择 Skia feature；例如 Win32/DX12 宿主需要 Direct3D，已有 OpenGL 后端的项目可以只启用 OpenGL，纯 CPU 离屏渲染项目不必被迫启用 DX12。

vcpkg manifest 示例：

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

这个 manifest 只是 Win32/DX12 demo 的参考起点，不是 SkUI 对使用者的固定要求。如果只接入 `Skui`，不需要 Win32/DX12 窗口和渲染相关系统库；本地位图解码走 Skia codec，使用 vcpkg 时需要给 `skia` 启用目标项目真正需要的图片格式 feature，例如 `png`、`jpeg`、`webp`。接入 `SkuiWin32` 时需要 Win32 输入相关系统库，例如 `user32`、`imm32`；接入 `SkuiWin32Dx12` 时还需要 `gdi32`、`dwmapi`、`shcore`、`d3d12`、`dxgi`、`dxguid`、`d3dcompiler`、`dbghelp`、`shell32`、`usp10`、`windowscodecs`。

不要这样接入：

```cmake
# 错误：把本仓库某次构建出来的产物硬塞给目标项目。
target_link_directories(MyApp PRIVATE D:/CMakeProject/SkiaTest/build/ninja-vcpkg)
target_link_libraries(MyApp PRIVATE Skui.lib skia.lib yoga.lib lexbor.lib)
```

正确方向是让目标项目在自己的构建里生成这些 target：

```cmake
# 正确：依赖由目标项目 toolchain / package manager 解析。
find_package(unofficial-skia CONFIG REQUIRED)
find_package(yoga CONFIG REQUIRED)
find_package(lexbor CONFIG REQUIRED)

add_library(Skui STATIC ...)
target_link_libraries(Skui PUBLIC unofficial::skia::skia unofficial::skia::modules::svg yoga::yogacore)
target_link_libraries(Skui PRIVATE lexbor::lexbor_static)
```

## 方案一：直接复制源码接入

适合当前阶段，也是最推荐给其他项目的接入方式。把这些目录复制到目标项目，由目标项目自己的 CMake 和依赖系统编译 SkUI：

```text
src/skui/public/
src/skui/core/
src/skui/render/
```

`src/skui/public/` 里常用的接入头文件：

| 头文件 | 用途 |
| --- | --- |
| `skui_runtime.h` | Runtime、事件、运行时更新接口 |
| `skui_runtime_helpers.h` | 批量更新保护、逻辑尺寸、style/px/action 辅助函数 |
| `skui_dropdown.h` | 普通 DOM 组合下拉框的状态控制 |
| `skui_virtual_window.h` | 列表、聊天记录等单维窗口化渲染状态 |
| `skui_virtual_table.h` | 表格窗口化渲染、表格面板布局、工具栏换行高度计算 |
| `skui_win32_event_adapter.h` | Win32 消息、IME、剪贴板、光标到 SkUI 事件的适配；只用 `SkuiWin32` 或 `SkuiWin32Dx12` 时需要 |
| `skui_win32_app.h` | Win32/DX12 宿主入口；只用 `SkuiWin32Dx12` 时需要 |

如果需要复用通用 Win32 事件适配，再复制：

```text
src/skui/platform/win32_event_adapter.cpp
```

如果需要内置 Win32/DX12 宿主，再复制：

```text
src/skui/platform/win32_dx12_app.cpp
src/d3d_presenter.h
src/d3d_presenter.cpp
src/perf_trace.h
```

目标项目 CMake。这里的 `find_package` 必须解析到目标项目自己的依赖安装结果，不应该指向本仓库的构建目录：

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

如果 Windows 项目已经有自己的窗口和 OpenGL/Vulkan/DX12 后端，只想复用 Win32 事件适配，可以先增加 `SkuiWin32`：

```cmake
add_library(SkuiWin32 STATIC
    src/skui/platform/win32_event_adapter.cpp
)

target_include_directories(SkuiWin32 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/skui/public"
)

target_link_libraries(SkuiWin32 PUBLIC Skui)
target_link_libraries(SkuiWin32 PRIVATE user32 imm32)
target_compile_definitions(SkuiWin32 PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
```

如果同时接入 `SkuiWin32Dx12`，还需要把 Win32 窗口宿主和 DX12 presenter 加进目标。DX12 host public 依赖 `SkuiWin32`，不要再次把 `win32_event_adapter.cpp` 编入 DX12 target：

```cmake
add_library(SkuiWin32Dx12 STATIC
    src/skui/platform/win32_dx12_app.cpp
    src/d3d_presenter.cpp
)

target_link_libraries(SkuiWin32Dx12 PUBLIC SkuiWin32)

target_link_libraries(SkuiWin32Dx12 PRIVATE
    user32
    gdi32
    dwmapi
    shcore
    d3d12
    dxgi
    dxguid
    d3dcompiler
    dbghelp
    ole32
    imm32
    shell32
)
```

## 方案二：作为子目录接入

如果目标项目能直接引用本仓库，可以把本仓库作为 submodule 或源码目录，然后在顶层 `CMakeLists.txt` 使用：

```cmake
add_subdirectory(external/SkiaTest)
target_link_libraries(MyApp PRIVATE Skui)
```

这个方式仍然必须使用目标项目自己的 toolchain、vcpkg manifest 或其他依赖解析方式；它只是复用源码目录，不是复用本仓库编译产物。不要把 `external/SkiaTest/build` 里的库文件提交给目标项目使用。

这个方式会同时带入本仓库 demo target。后续如果要做成更干净的库，建议增加 `SKUI_BUILD_DEMOS` / `SKUI_BUILD_TESTS` 选项，把 demo 从库构建里隔离出去。对正式项目来说，当前更建议使用“方案一”的源码 target 接入方式，只复制 `Skui` / `SkuiWin32` / `SkuiWin32Dx12` 需要的源码和 public 头文件。

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
    options.useSystemDpiScale = true;
    options.runtime.scale = 1.0f;
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

`WindowOptions::useSystemDpiScale` 控制 Win32/DX12 宿主是否把系统 DPI 纳入 SkUI 的缩放计算，默认是 `true`。如果目标系统已有自己的 UI 缩放方案，例如宿主已经在 4K 下按 1.5 倍管理窗口和输入坐标，可以把它设为 `false`，让 SkUI 按 1.0 的平台 DPI 运行。

`RuntimeOptions::scale` 是 SkUI 自身的用户缩放倍率，默认 `1.0f`。最终生效倍率为“Win32 系统 DPI 倍率（如果启用）乘以 `RuntimeOptions::scale`”。因此：

- 跟随 Windows 150% 缩放：`useSystemDpiScale = true`，`runtime.scale = 1.0f`。
- 忽略 Windows DPI，使用宿主自己的 150% 缩放：`useSystemDpiScale = false`，由宿主管理窗口尺寸和输入坐标，必要时设置 `runtime.scale = 1.5f`。
- 需要运行时切换 UI 缩放时，调用 `Runtime::setScale(scale)`；`Runtime::effectiveScale()` 可以读取最终用于布局、绘制和事件坐标换算的倍率。

## 方案四：只接入运行时和自己的渲染循环

如果目标项目已经有窗口、消息循环和 Skia `SkCanvas`，只需要 `Skui`。这也是接入新后端的标准路径：新后端负责窗口、GPU context、swapchain、帧调度和平台事件；SkUI 负责 DOM、CSS、布局、事件分发和在 `SkCanvas` 上绘制。

后端边界建议这样划分：

| 职责 | 归属 |
| --- | --- |
| HTML/CSS 解析、DOM、Yoga 布局、命中测试、控件状态 | `Skui` |
| Skia 绘制命令输出 | `Skui`，通过 `Runtime::render(SkCanvas*)` |
| 窗口创建、swapchain、GPU surface、present | 目标项目后端 |
| 鼠标、键盘、输入法、剪贴板、光标 | 目标项目平台适配层；Windows 项目可以复用 `SkuiWin32` |
| 图片加载完成后的重绘调度 | 目标项目通过 `RuntimeOptions::requestRedraw` 接入 |

接入流程：

1. 在目标项目里按源码 target 编译 `Skui`。如果是 Windows 自定义后端，可以额外链接 `SkuiWin32`；不要为了复用 Win32 事件而依赖 `SkuiWin32Dx12`。
2. 目标项目按自己的后端创建 Skia surface，例如 Direct3D、OpenGL、Vulkan、Metal、Raster surface 或已有引擎暴露的 `SkCanvas`。
3. 创建 `skui::Runtime`，设置 `assetRoot`、剪贴板回调、重绘回调和业务事件回调。
4. 窗口尺寸或 DPI 变化时调用 `Runtime::resize(width, height, dpiScale)`。
5. 平台输入事件转成 `skui::Event` 后调用 `Runtime::handleEvent(event)`。
6. 每帧拿到目标后端当前帧的 `SkCanvas`，调用 `Runtime::render(canvas)`，再由目标后端 present。
7. 图片异步加载、状态更新或输入事件触发重绘时，由宿主根据 `requestRedraw` 或自己的 dirty flag 安排下一帧。

自定义后端传给 `Runtime::resize(width, height, dpiScale)` 的 `dpiScale` 应只表示平台 DPI 倍率。如果宿主不希望 SkUI 跟随系统 DPI，就传 `1.0f`；如果还需要用户自定义 UI 缩放，设置 `RuntimeOptions::scale` 或运行时调用 `Runtime::setScale(scale)`。SkUI 内部会用 `dpiScale * scale` 统一处理逻辑视口、Skia 绘制缩放和输入命中坐标。

最小宿主循环示例：

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

### 新后端适配清单

新增 OpenGL、Vulkan、Metal、SDL、Qt、游戏引擎或已有 CAD/GIS 渲染后端时，不需要改 SkUI 核心，通常只需要新增一个宿主 target，例如 `SkuiSdlGl`、`SkuiQtRaster` 或项目自己的 `MyAppSkuiHost`。

新后端需要实现这些连接点：

- **SkCanvas 来源**：每帧提供目标 surface 对应的 `SkCanvas`，并保证 `Runtime::render` 调用期间 canvas 有效。
- **尺寸同步**：窗口逻辑宽高和 DPI scale 变化后调用 `Runtime::resize`。
- **事件转换**：把平台鼠标、滚轮、键盘、文本输入、IME 组合文本、焦点变化转成 `skui::Event`。
- **剪贴板**：设置 `RuntimeOptions::readClipboardText` 和 `RuntimeOptions::writeClipboardText`，文本统一转成 UTF-8。
- **光标**：每次输入事件或 hover 变化后读取 `Runtime::cursor()`，映射成平台光标。
- **重绘调度**：设置 `RuntimeOptions::requestRedraw`，图片加载、动画或运行时更新完成后能唤醒后端绘制一帧。
- **资源路径**：设置 `RuntimeOptions::assetRoot`，并在目标项目构建后复制 HTML、CSS、SVG、图片等资源。

不同后端只差在“如何得到 SkCanvas”和“如何接入平台事件”。只要 Skia 能在目标后端上创建 surface，SkUI 就可以复用同一套 DOM/CSS/布局和控件逻辑。

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

事件坐标应使用绘制目标的像素坐标，并和 `Runtime::resize(width, height, dpiScale)` 里的 `width`、`height` 保持同一坐标系。Runtime 会用 `dpiScale * RuntimeOptions::scale` 反算 SkUI 逻辑坐标；如果宿主已经提前把输入转换成逻辑坐标，就应传 `dpiScale = 1.0f` 并避免重复设置 UI 缩放。

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

注意：不要依赖完整 JavaScript 或浏览器 DOM API。SkUI 的动态能力来自 C++ 回调和运行时更新接口。简单状态更新优先使用 `applyUpdates` 批量同步样式、文本和属性；需要运行时增删 UI 节点时，使用 `appendHtmlById`、`prependHtmlById`、`replaceHtmlById`、`removeElementById`。隐藏节点有两种语义：`setVisibleById(id, false)` 使用 `display:none`，隐藏后不占布局；`setStyleById(id, "visibility:hidden;")` 隐藏绘制和命中但保留布局占位。

### 下拉框

SkUI 目前没有浏览器原生 `select` 行为。下拉框建议用普通 DOM 节点写样式，用 `skui::DropdownState` 管理状态。

HTML 只需要给按钮、选中文本、箭头、菜单、遮罩和选项提供稳定 id：

```html
<div id="layer-dropdown" data-action="toggle-layer-menu">
  <span id="layer-selected">地块边界.shp</span>
  <span id="layer-arrow">v</span>
</div>
<div id="layer-menu" class="dropdown-menu page-hidden">
  <div id="layer-option-0" data-action="select-layer:0">地块边界.shp</div>
  <div id="layer-option-1" data-action="select-layer:1">道路中心线.shp</div>
</div>
<div id="layer-backdrop" class="page-hidden" data-action="close-layer-menu"></div>
```

C++ 侧复用同一个状态对象：

```cpp
skui::DropdownState layerDropdown(skui::DropdownConfig{
    .selectedTextId = "layer-selected",
    .arrowId = "layer-arrow",
    .menuId = "layer-menu",
    .backdropId = "layer-backdrop",
    .optionIdPrefix = "layer-option-",
    .hiddenClass = "page-hidden",
    .selectedClass = "selected",
    .openArrow = "^",
    .closedArrow = "v",
    .optionCount = layers.size(),
});

ui.setElementEventCallback([&](const skui::ElementEvent& event) {
    if (event.type != skui::ElementEventType::Click) {
        return;
    }
    if (event.action == "toggle-layer-menu") {
        layerDropdown.toggle(ui);
    } else if (event.action == "close-layer-menu") {
        layerDropdown.setOpen(ui, false);
    } else if (event.action.starts_with("select-layer:")) {
        const int index = parseIndex(event.action);
        layerDropdown.select(ui, index, layers[index].name);
    }
});
```

## 大量数据：虚拟滚动 / 窗口化渲染

SkUI 支持浏览器常见的窗口化思路：滚动范围按完整数据尺寸计算，DOM 里只保留可见范围附近的一小批节点。这样可以展示几十万行数据，但实际每帧只刷新几十行。

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

### 单维列表和聊天记录

列表、聊天记录这类每项高度固定的场景，优先用 `skui::VirtualWindowState` 计算可见窗口。它会根据滚动位置和视口高度返回：

- `firstItem`：第一条可见数据的下标。
- `scrollOffset`：当前滚动偏移，用来把池化节点移动到正确位置。
- `cachedItems`：本次需要显示的池化节点数量。
- `renderNeeded`：首项、偏移或缓存数量没有变化时为 `false`，可以跳过 DOM 更新。

业务层仍负责把 `firstItem + poolIndex` 对应的数据写入池化节点。所有节点更新建议放进同一个 `RuntimeUpdates` 批次：样式负责移动和显隐，文本负责替换可见内容，属性负责更新 `data-action` 或虚拟尺寸。

### 表格

表格优先用 `skui::VirtualTableAdapter`，不要在页面 `main.cpp` 里重新写一套池化表格刷新器。调用方只需要提供列定义、池化 DOM id 规则和数据源。

池化 DOM 的 id 规则由 `VirtualTableGeometry` 决定：

| 节点 | id 规则 |
| --- | --- |
| 表头拖拽列 | 调用方传入 `VirtualTableRenderConfig::handleHeaderId` |
| 普通表头 | `headerPrefix + column.id` |
| 行背景 | `rowPrefix + poolIndex + "-bg"`，`poolIndex` 从 1 开始 |
| 行拖拽列 | `rowPrefix + poolIndex + "-handle"` |
| 单元格 | `cellPrefix + poolIndex + "-" + column.id` |

HTML 示例：

```html
<div id="attr-table"
     class="attribute-table"
     data-action="attr-table-scroll"
     data-virtual-width="1200"
     data-virtual-height="3600000">
  <div id="attr-head-handle" class="table-header handle"></div>
  <div id="attr-head-id" class="table-header">ID</div>
  <div id="attr-head-name" class="table-header">名称</div>

  <div id="attr-row-1-bg" class="table-row-bg"></div>
  <div id="attr-row-1-handle" class="table-row-handle"></div>
  <div id="attr-cell-1-id" class="table-cell"></div>
  <div id="attr-cell-1-name" class="table-cell"></div>
</div>
```

C++ 示例：

```cpp
std::vector<skui::VirtualTableColumn> columns = {
    {"id", 96},
    {"name", 150},
    {"landuse", 180},
    {"height", 130},
};

skui::VirtualTableAdapter table(
    skui::VirtualTableGeometry("attr-row-", "attr-cell-", "attr-head-", columns, 40, 40, 36),
    skui::VirtualWindowConfig{
        .itemCount = static_cast<int>(records.size()),
        .itemExtent = 36,
        .leadingExtent = 40,
        .poolSize = 80,
        .minCachedItems = 12,
        .overscanItems = 4,
    },
    skui::VirtualTableRenderConfig{
        .viewportId = "attr-table",
        .handleHeaderId = "attr-head-handle",
        .rowBaseClass = "table-row-bg",
        .rowSelectedClass = "selected",
        .rowActionPrefix = "select-row:",
        .rowHandleActionPrefix = "select-row:",
        .cellBaseClass = "table-cell",
        .cellSelectedClass = "selected-cell",
        .cellColumnClassPrefix = "col-",
    });

skui::VirtualTableDataSource source;
source.itemCount = static_cast<int>(records.size());
source.row = [&](const skui::VirtualTableRowContext& row) {
    return skui::VirtualTableRowData{
        .selected = row.rowIndex == selectedRow,
    };
};
source.cell = [&](const skui::VirtualTableCellContext& cell) {
    return skui::VirtualTableCellData{
        .text = valueFor(records[cell.rowIndex], cell.columnId),
        .action = "select-cell:" + std::to_string(cell.rowIndex) + ":" +
            std::string(cell.columnId),
        .selected = isSelectedCell(cell.rowIndex, cell.columnId),
    };
};

table.refresh(ui, currentScrollY, tableViewportHeight, source);
```

`VirtualTableAdapter::refresh` 会自动同步 `data-virtual-width` / `data-virtual-height`，并且只有首行、滚动偏移、缓存数量、数据量或强制刷新变化时才批量更新 DOM。选择状态、排序、数据内容变化后调用 `table.invalidate()`，下一次 `refresh` 会强制重写可见池。

### 表格面板自适应

属性表这类可拖宽面板可以复用 `skui::VirtualTablePanelLayout`：

- 面板宽度会被限制在当前窗口内，不会拖到超过主窗口。
- 工具栏按钮宽度通过 `FlowRowLayoutConfig` 计算自动换行后的高度。
- 表格高度会按当前窗口逻辑高度扣除标题、工具栏、底部信息区后占满剩余空间。
- `renderNeeded()` 为 `false` 时，不需要重复写入面板和表格尺寸样式；只要等待下一帧绘制即可。

这类布局仍然由 Yoga 负责普通节点排版；`VirtualTablePanelLayout` 只负责把需要跨节点同步的业务尺寸计算出来，再通过 `setStylesById` 或 `applyUpdates` 写回对应节点。

## 资源路径

`RuntimeOptions::assetRoot` 用于解析相对资源路径。`img src="icons/a.svg"`、`img src="photos/a.bmp"` 这类本地资源查找顺序：

1. HTML 文件所在目录。
2. `RuntimeOptions::assetRoot`。
3. 原始路径。

`img` 的位图资源首帧会先排队，由 renderer 后台 worker 读取并交给 Skia codec 解码；当前覆盖 PNG、JPEG、WebP 和 BMP。自定义宿主如果不用 `SkuiWin32Dx12`，需要设置 `RuntimeOptions::requestRedraw`，用于在图片完成加载后安排重绘；Win32/DX12 宿主已经接入这个回调。SVG 文件仍按文本读取并交给 Skia SVG DOM 渲染。

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
- Skia、Skia SVG、Yoga、Lexbor 由目标项目自己拉取、配置并能被 CMake 找到。
- 没有链接本仓库 `build` 目录里的预编译 `.lib`，也没有把本仓库的第三方依赖产物当成 SDK 分发。
- Skia feature 与目标项目后端一致，例如 DX12 项目启用 Direct3D，OpenGL 项目启用 OpenGL，离屏 CPU 渲染项目只保留必要 feature。
- 如果目标项目不是 Win32/DX12，不需要链接 `SkuiWin32Dx12`；只要自己提供 `SkCanvas`、事件转换和重绘调度即可。Windows 自定义后端可以只链接 `SkuiWin32`。
- 资源目录会随构建复制到运行目录，或 `assetRoot` 指到正确位置。
- 平台事件已转成 `skui::Event`。
- 剪贴板读写回调已设置；使用 `SkuiWin32` 或 `SkuiWin32Dx12` 时默认已设置。
- 鼠标光标读取 `Runtime::cursor()` 并映射到平台光标；使用 `SkuiWin32` 或 `SkuiWin32Dx12` 时默认已设置。
- 普通 DOM 组合下拉框优先复用 `skui::DropdownState`，不要在每个页面重复写展开/关闭/选中同步逻辑。
- 大量数据界面使用虚拟滚动，不创建全量 DOM 节点。
- 大表格优先复用 `skui::VirtualTableAdapter`，由页面只提供列定义、池化 DOM id 规则和数据源。
- 使用 `applyUpdates` 批量更新，避免一帧内多次重复布局。
