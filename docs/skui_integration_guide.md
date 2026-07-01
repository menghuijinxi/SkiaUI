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

如果只接入 `Skui`，不需要 Win32/DX12 窗口和渲染相关系统库；本地位图解码走 Skia codec，使用 vcpkg 时需要给 `skia` 启用常用格式 feature，例如 `png`、`jpeg`、`webp`。接入 `SkuiWin32Dx12` 时还需要链接 `user32`、`gdi32`、`dwmapi`、`shcore`、`d3d12`、`dxgi`、`dxguid`、`d3dcompiler`、`dbghelp`、`imm32`、`shell32`、`usp10`。

## 方案一：直接复制源码接入

适合当前阶段。把这些目录复制到目标项目：

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
| `skui_win32_app.h` | Win32/DX12 宿主入口；只用 `SkuiWin32Dx12` 时需要 |

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

如果同时接入 `SkuiWin32Dx12`，还需要把 Win32 窗口宿主、Win32 事件适配器和 DX12 presenter 加进目标：

```cmake
add_library(SkuiWin32Dx12 STATIC
    src/skui/platform/win32_event_adapter.cpp
    src/skui/platform/win32_dx12_app.cpp
    src/d3d_presenter.cpp
)

target_link_libraries(SkuiWin32Dx12 PUBLIC Skui)

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

`img` 的位图资源首帧会先排队，由 renderer 后台 worker 读取并交给 Skia codec 解码；当前覆盖 PNG、JPEG、WebP 和 BMP。自定义宿主如果不用 `SkuiWin32Dx12`，需要设置 `RuntimeOptions::requestRedraw`，用于在图片完成加载后安排重绘；Win32 宿主已经接入这个回调。SVG 文件仍按文本读取并交给 Skia SVG DOM 渲染。

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
- 普通 DOM 组合下拉框优先复用 `skui::DropdownState`，不要在每个页面重复写展开/关闭/选中同步逻辑。
- 大量数据界面使用虚拟滚动，不创建全量 DOM 节点。
- 大表格优先复用 `skui::VirtualTableAdapter`，由页面只提供列定义、池化 DOM id 规则和数据源。
- 使用 `applyUpdates` 批量更新，避免一帧内多次重复布局。
