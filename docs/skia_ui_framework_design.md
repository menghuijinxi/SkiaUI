# Skia 轻量 UI 框架设计文档

## 目标

做一套可以移植到其他项目里的轻量 UI 框架，底层用 Skia 绘制，布局用 Yoga。

使用者的理想接入方式应该是：

1. 创建一个 UI 运行时对象。
2. 填写参数。
3. 配置渲染后端。
4. 加载一个 `.html` 文件。
5. 把窗口事件转发给 UI。
6. 每帧调用渲染函数。

测试程序只能是测试宿主，不允许把框架逻辑都写进 `main.cpp`。`main.cpp` 只负责解析测试参数、创建窗口、创建 UI 对象。

## 不做什么

- 第一阶段不做浏览器。
- 第一阶段不做 JavaScript。
- 第一阶段不做完整 HTML 标准。
- 第一阶段不做完整 CSS 标准。
- 第一阶段不做复杂多行富文本编辑器。
- 框架核心不能依赖 Win32。
- 框架核心不能依赖 DX12。
- 框架核心不能绑定现在的 `SkiaLayerDesk` 业务界面。

## 总体架构

```text
使用者项目 / 测试窗口
  -> 平台事件适配层
  -> UiRuntime
       -> 文档加载
       -> DOM 树
       -> CSS 样式计算
       -> Yoga 布局
       -> 控件状态
       -> 渲染后端抽象
       -> Skia 绘制器
```

核心层负责 DOM、样式、布局、焦点、悬停、按下状态、控件值。

渲染层必须抽象出来。默认实现可以用现在的 DX12 + Skia 后端，但使用者必须可以接入自己的后端，就像 ImGui 可以接入不同图形 API 一样。

## 对外 API 形态

普通项目接入时，期望是这种用法：

```cpp
skui::RuntimeOptions options;
options.assetRoot = "assets/ui";
options.scale = 1.0f; // 用户 UI 缩放倍率，会和 resize 传入的 dpiScale 相乘。
options.textScale = 1.0f; // 文字辅助倍率，会合入内容布局、绘制和输入倍率。
options.theme = skui::Theme::Dark();

skui::Runtime ui(options);
ui.setRenderer(std::make_unique<MyRendererBackend>());
ui.loadDocument("assets/ui/layers.html");

// 窗口回调里：
ui.resize(width, height, dpiScale);
ui.handleEvent(event);

// 渲染循环里：
ui.render();
```

如果使用内置 Win32 + DX12 测试宿主，期望是这种用法：

```cpp
skui::win32::WindowOptions window;
window.title = L"SkiaUiDesk";
window.width = 1280;
window.height = 800;
window.useSystemDpiScale = true;
window.useSystemTextScale = true;
window.runtime.scale = 1.0f;

skui::win32::Dx12WindowApp app(window);
app.ui().loadDocument("assets/ui/layers.html");
return app.run();
```

## 建议目录结构

```text
src/skui/
  public/
    skui_runtime.h
    skui_events.h
    skui_renderer_backend.h
    skui_theme.h
  core/
    runtime.cpp
    document.cpp
    element.cpp
    style_engine.cpp
    style_parser.cpp
    layout_yoga.cpp
    control_state.cpp
    hit_test.cpp
  render/
    skia_painter.cpp
    skia_text_cache.cpp
  platform/
    win32_event_adapter.cpp
    win32_window_app.cpp
  backends/
    dx12_skia_backend.cpp

src/skui_demo/
  main.cpp
  demo_window.cpp

assets/skui_demo/
  layers.html
  layers.css
```

原则：

- `src/skui/public/` 是使用者能包含的头文件。
- `src/skui/core/` 不允许直接依赖 Win32、DX12。
- `src/skui/platform/` 放窗口事件适配。
- `src/skui/backends/` 放默认渲染后端。
- `src/skui_demo/` 只是测试程序。

## 核心模块

### `UiRuntime`

`UiRuntime` 是使用者主要接触的类。

职责：

- 加载 HTML。
- 加载 CSS。
- 保存 DOM 树。
- 保存控件状态。
- 接收平台无关事件。
- 判断是否需要重新布局。
- 判断是否需要重新绘制。
- 调用渲染后端开始一帧。
- 调用 Skia 绘制器绘制 DOM。
- 调用渲染后端提交一帧。

### 渲染后端接口

渲染后端是框架和使用者项目之间最重要的边界。

第一版接口草案：

```cpp
class RendererBackend {
public:
    virtual ~RendererBackend() = default;

    virtual bool initialize(const RendererInitInfo& info) = 0;
    virtual void resize(int width, int height, float dpiScale) = 0;
    virtual SkCanvas* beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void present() = 0;
};
```

默认 DX12 后端可以复用现在 `D3DPresenter` 里已经修过的逻辑，但要放到 `src/skui/backends/`。

默认 DX12 后端需要保留这些经验：

- Skia 包装 swapchain backbuffer 时必须正确保留 COM 引用。
- transition command list 不能每帧创建临时对象后立刻释放。
- resize 前要释放 Ganesh 可清理资源，避免 `ResizeBuffers` 因旧 backbuffer 仍被持有而失败。
- 框架核心不应该知道这些 DX12 细节。

### 平台事件

窗口事件要抽象，不能让核心直接处理 `WM_*`。

第一版事件类型：

```cpp
enum class EventType {
    PointerMove,
    PointerDown,
    PointerUp,
    MouseWheel,
    KeyDown,
    KeyUp,
    TextInput,
    ImeStart,
    ImeUpdate,
    ImeEnd,
    FocusGained,
    FocusLost,
    Resize,
};
```

Win32 适配层负责把这些消息转成通用事件：

- `WM_MOUSEMOVE`
- `WM_LBUTTONDOWN`
- `WM_LBUTTONUP`
- `WM_MOUSEWHEEL`
- `WM_KEYDOWN`
- `WM_KEYUP`
- `WM_CHAR`
- `WM_IME_COMPOSITION`
- `WM_SIZE`
- `WM_SETFOCUS`
- `WM_KILLFOCUS`

未来如果接 SDL、GLFW、Qt、Android，只需要新增事件适配层。

## DOM 设计

框架内部先用自己的轻量 DOM。

结构草案：

```cpp
struct Element {
    ElementId id;
    std::string tag;
    std::string idAttr;
    std::vector<std::string> classes;
    std::unordered_map<std::string, std::string> attrs;
    std::string text;
    std::vector<std::unique_ptr<Element>> children;

    ComputedStyle style;
    LayoutBox layout;
};
```

第一阶段支持这些标签：

- `window`
- `panel`
- `div`
- `label`
- `button`
- `input`
- `select`
- `option`
- `checkbox`
- `line`
- `spacer`

后面可以做控件注册表，让使用者注册自己的标签：

```cpp
ui.registerControl("color-picker", std::make_unique<ColorPickerFactory>());
```

## HTML / CSS 解析方案

第一阶段建议先做自研子集解析器，不一上来接完整解析库。

原因：

- 我们只需要 UI 用的 HTML 子集。
- CSS 也只需要常用属性。
- 自研子集更容易控制行为和性能。
- 未来要换 Lexbor，也可以放在解析接口后面，不影响框架核心。

第一阶段 HTML 支持：

- 标签。
- 属性。
- 文本节点。
- 自闭合标签。
- 简单嵌套。

第一阶段 CSS 支持：

- `tag`
- `.class`
- `#id`
- `tag.class`
- `.a.b`
- `#id.class`
- 后代选择器：`.a .b`
- 子选择器：`.a > .b`
- 属性选择器：`[attr]`、`[attr=value]`
- `:hover`
- `:active`
- `:focus`
- `:checked`
- `:disabled`
- `:selected`
- `:first-child`
- `:last-child`
- `:nth-child(n)`、`:nth-child(odd)`、`:nth-child(even)`
- 浏览器式基础 specificity：id 高于 class/属性/伪类，高于 tag；同优先级按源码顺序覆盖；inline style 最后覆盖。
- 运行时 `addClassById` / `removeClassById` / `setStyleById` / `setAttributeById` / `removeAttributeById`。

第一阶段不支持：

- 复杂组合选择器。
- 兄弟选择器。
- 属性选择器。
- 媒体查询。
- CSS 变量。
- 动画。

动态 CSS 后续待补充：

- 运行时增删 `<style>` 或外部 stylesheet 规则。
- 兄弟选择器：`+`、`~`。
- `:not()`、`:has()`、`:is()` 等更复杂伪类。
- `transition` / `animation`。当前可以先参考 RmlUi、浏览器引擎的分层思路，但不应直接移植完整浏览器动画系统；适合后续在 Runtime 内做一个只覆盖 `opacity`、`transform` 和简单 easing 的轻量动画子集。
- 样式变化的精细 dirty 标记，区分只重绘、需要 layout、需要重新测量文本。

## Lexbor 说明

Lexbor 不是 C++ 库，是 C99 库。

它可以接入 C++ 项目，因为 C++ 可以直接调用 C API，但我们需要自己包一层 RAII，避免资源释放混乱。

Lexbor 可以做的事情：

- 解析 HTML。
- 构建 DOM。
- 解析一部分 CSS 相关内容。
- 做 selector 匹配。
- 提供 encoding、URL 等辅助模块。

Lexbor 不会做的事情：

- 不会帮我们布局。
- 不会帮我们绘制。
- 不会帮我们做 Skia 集成。
- 不会帮我们做按钮、输入框、下拉框这些控件状态。
- 不会帮我们接窗口事件。

结论：

Lexbor 适合以后作为“解析层可选后端”，不适合直接当 UI 框架。

建议接口：

```cpp
class HtmlParser {
public:
    virtual ~HtmlParser() = default;
    virtual std::unique_ptr<Document> parseHtml(std::string_view html) = 0;
};

class StyleParser {
public:
    virtual ~StyleParser() = default;
    virtual StyleSheet parseCss(std::string_view css) = 0;
};
```

这样未来可以有两个实现：

- `SimpleHtmlParser`
- `LexborHtmlParser`

## CSS 属性范围

### 映射到 Yoga 的属性

这些属性负责布局：

- `display`
- `position`
- `left`
- `top`
- `right`
- `bottom`
- `width`
- `height`
- `min-width`
- `min-height`
- `max-width`
- `max-height`
- `flex-direction`
- `flex-grow`
- `flex-shrink`
- `flex-basis`
- `align-items`
- `align-self`
- `justify-content`
- `gap`
- `row-gap`
- `column-gap`
- `padding`
- `margin`
- `border-width`

### 映射到 Skia 的属性

这些属性负责绘制：

- `background`
- `color`
- `border-color`
- `border-radius`
- `font-size`
- `font-weight`
- `opacity`
- `text-align`
- `line-height`

### 控件相关属性

- `cursor`
- `pointer-events`
- `disabled`

## Yoga 布局

Yoga 是第一版布局引擎。

处理流程：

1. DOM 树变成 Yoga node 树。
2. CSS 布局属性写进 Yoga node。
3. 文本、按钮、输入框设置 measure 函数。
4. 调用 `YGNodeCalculateLayout`。
5. 把 Yoga 结果复制回每个 Element 的 `LayoutBox`。
6. Skia 绘制时只读 `LayoutBox`。

Yoga 要隔离在 `LayoutEngine` 后面：

```cpp
class LayoutEngine {
public:
    virtual ~LayoutEngine() = default;
    virtual void layout(Document& document, float width, float height) = 0;
};
```

这样未来如果某些控件不用 Yoga，或者要加绝对布局优化，不会影响 UI 核心。

## 控件设计

第一批控件：

- 按钮。
- 文本标签。
- 面板 / 外边框。
- 线 / 分割线。
- 单行输入框。
- 下拉框。
- 勾选框。
- 空白占位。

不建议第一阶段每个控件都搞复杂继承树。

建议使用标签行为表：

```cpp
ControlBehavior* resolveBehavior(Element& element);
behavior->handleEvent(context, element, event);
behavior->paint(context, element, painter);
```

好处：

- 简单。
- 容易注册自定义控件。
- DOM 和控件状态能分开。
- 不会一开始就写出很重的控件体系。

## 输入框风险

输入框是第一阶段最复杂的控件。

按钮、线、外边框、勾选框都比较简单。

输入框至少需要处理：

- 焦点。
- 光标。
- 基础选区。
- Backspace。
- Delete。
- 左右方向键。
- Home / End。
- 鼠标点选。
- Shift + 鼠标点选扩展选区。
- 文本输入。

中文输入法需要单独设计：

- `ImeStart`
- `ImeUpdate`
- `ImeEnd`

建议第一阶段只做单行输入框。多行文本框后面再做。

输入框后续待补充：

- 双击中文词汇时按中文分词选中完整词。当前只保证 ASCII 单词、空白和单个非 ASCII 字符的基础行为，完整中文分词需要单独词法模块或接入分词库。

## 绘制流程

Skia 绘制器只读 DOM、样式和布局结果。

绘制顺序：

1. 背景。
2. 边框。
3. 控件内容。
4. 子元素。
5. overlay 层。

overlay 层用于：

- 下拉菜单。
- tooltip。
- 输入框光标。
- 输入法组合文字。

绘制器不允许直接调用 Win32 或 DX12。

## 缓存策略

第一阶段可以做这些缓存：

- 文本 `SkTextBlob` 缓存。
- 颜色解析缓存。
- 字体对象缓存。

第二阶段再做：

- 静态子树 `SkPicture` 缓存。
- SVG 图标缓存。
- 图片资源缓存。

缓存必须跟 dirty flag 绑定，避免移动窗口时重复绘制静态内容。

## 命中测试

命中测试从最上层子节点往父节点走。

规则：

- 跳过不可见元素。
- 跳过 `pointer-events: none`。
- 后绘制的元素优先命中。
- 命中变化会更新 hover。
- 鼠标按下会更新 active。
- 鼠标点击或键盘 Tab 会更新 focus。

第一阶段可以先不做复杂裁剪命中。

## 脏标记

需要分开这些 dirty 状态：

- `styleDirty`
- `layoutDirty`
- `paintDirty`
- `textMetricsDirty`

移动原生窗口不应该标记 UI dirty。

改变窗口尺寸应该标记：

- `layoutDirty`
- `paintDirty`

hover / active / focus 变化通常标记：

- `styleDirty`
- `paintDirty`

文本变化通常标记：

- `textMetricsDirty`
- `layoutDirty`
- `paintDirty`

## 测试程序目标

新增一个测试窗口，例如 `SkiaUiDesk`。

它只负责：

- 解析命令行。
- 创建 Win32 窗口。
- 创建 `skui::Runtime`。
- 创建默认 DX12 后端。
- 转发窗口事件。
- 加载指定 HTML。

命令行草案：

```text
SkiaUiDesk.exe --html assets/skui_demo/layers.html --width 1280 --height 800 --backend dx12
```

## 示例 HTML

```html
<window class="app">
  <panel id="layers" class="panel">
    <label class="title">图层 / 图层管理</label>
    <button id="import">导入SHP</button>
    <button id="new-layer">新建图层</button>
    <input id="search" placeholder="搜索图层..." />
    <checkbox id="visible" checked>显示图层</checkbox>
    <select id="type">
      <option value="polygon">Polygon</option>
      <option value="line">LineString</option>
      <option value="point">Point</option>
    </select>
  </panel>
</window>
```

## 示例 CSS

```css
.app {
  display: flex;
  flex-direction: row;
  width: 100%;
  height: 100%;
  background: #071018;
  color: #f4fbff;
}

.panel {
  width: 600px;
  padding: 24px;
  gap: 16px;
  flex-direction: column;
  background: #132f41;
  border-width: 1px;
  border-color: #7fb5d3;
  border-radius: 8px;
}

button {
  height: 48px;
  background: #15c7c5;
  border-radius: 6px;
  color: #ffffff;
  font-size: 18px;
  font-weight: 700;
}

button:hover {
  background: #27ddda;
}
```

## 实现阶段

### 第一阶段：框架骨架

- 新增 `skui` 静态库。
- 新增公开头文件。
- 新增 `Runtime`。
- 新增平台无关事件结构。
- 新增渲染后端接口。
- 新增 `SkiaUiDesk` 测试窗口。
- 暂时用硬编码 DOM 或极简 HTML parser 跑通绘制。

### 第二阶段：HTML / CSS 子集

- 实现第一版 HTML parser。
- 实现第一版 CSS parser。
- 实现简单 selector 匹配。
- 实现样式计算。
- 接 Yoga 做布局。

### 第三阶段：基础控件

- 按钮。
- 勾选框。
- 下拉框。
- 单行输入框。
- hover / active / focus。
- 控件回调。

### 第四阶段：默认 DX12 后端

- 把默认 DX12 + Skia 后端从测试程序里抽出来。
- 后端实现 `RendererBackend`。
- 保留现在已经修过的 DX12 稳定性逻辑。

### 第五阶段：可选 Lexbor 后端

- 调研 vcpkg 是否能直接安装 Lexbor。
- 如果能安装，新增可选 CMake 开关。
- 用 RAII 包装 Lexbor C API。
- 保持 `SimpleHtmlParser` 作为默认轻量实现。

## 待确认问题

- 第一阶段是否需要支持外部 `.css` 文件，还是只支持 HTML 内联 `<style>`。
- 内部字符串是否全部使用 UTF-8。
- 输入框第一阶段是否必须支持中文输入法。
- 渲染后端接口是否直接暴露 `SkCanvas*`，还是封装成更小的绘制命令接口。
- 控件回调用元素 id 绑定，还是做事件订阅系统。
- 默认主题是否先写死，还是从 CSS 全部控制。
