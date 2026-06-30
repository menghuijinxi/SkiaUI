# SkUI CSS / DOM 支持说明

这份文档记录当前 SkUI 运行时已经实现的 HTML、DOM、CSS 和交互能力。SkUI 不是浏览器内核，也不承诺完整 HTML/CSS 标准；它是面向 Skia 原生 UI 的轻量子集。

## HTML 解析

- 使用 Lexbor 解析 HTML。
- 只读取 `<body>` 下的元素作为 UI 树。
- `<style>` 会被解析成样式规则；`<head>`、`<meta>`、`<title>`、`<script>` 不会生成可绘制节点。
- 未知标签会作为普通容器节点参与布局、绘制、事件命中。
- 文本节点会压缩连续空白；纯空白文本会被忽略。
- `<textarea>` 内部文本会作为初始 `value`。
- `<svg>` 会保留原始 SVG markup，并交给 Skia SVG DOM 绘制。

## 常用标签

| 标签 | 当前行为 |
| --- | --- |
| `div` / `span` / `label` / `text` | 普通容器或文本节点 |
| `button` | 普通可命中节点，可通过 `data-action` 发出事件 |
| `input` | 单行输入框，支持焦点、光标、选区、剪贴板、IME、Ctrl+Z |
| `textarea` | 多行输入框，复用输入框行为，支持换行、选区、剪贴板、IME、Ctrl+Z |
| `selectable` | 可框选复制文本标签；普通文本默认不可选中 |
| `progress` | 进度条，`value` / `max` 控制填充比例 |
| `img` | 当前用于加载 SVG 文件资源 |
| `svg` | 内联 SVG，由 Skia SVG DOM 绘制 |

`select` / `option` 目前没有浏览器式下拉控件行为。需要下拉框时，建议用普通节点自行组合：按钮节点负责展开，菜单节点用 `display:none` 或类名切换显示。

## 通用属性

| 属性 | 说明 |
| --- | --- |
| `id` | 运行时更新和事件识别的主要定位方式 |
| `class` | 支持多 class，参与 CSS 匹配 |
| `style` | 内联 CSS 声明，优先级高于 `<style>` 规则 |
| `data-action` | 命中事件回调中的业务动作名 |
| `value` | 输入框值；进度条当前值 |
| `max` | 进度条最大值 |
| `placeholder` | 输入框占位文本 |
| `src` | `img` 的资源路径 |
| `data-virtual-width` / `data-virtual-height` | 虚拟滚动内容尺寸，不需要真实子元素撑开 |

布尔属性如 `disabled`、`checked`、`selected` 当前主要用于 CSS 伪类匹配，不等同于完整浏览器控件状态。

## CSS 选择器

已支持：

- 标签选择器：`div`
- 通配选择器：`*`
- class：`.item`
- id：`#panel`
- 组合在同一元素上：`button.primary.active`
- 后代选择器：`.panel .row`
- 子选择器：`.panel > .row`
- 选择器列表：`.a, .b`
- 属性选择器：`[data-state]`、`[data-state=selected]`
- 伪类：`:hover`、`:active`、`:focus`、`:disabled`、`:checked`、`:selected`
- 结构伪类：`:first-child`、`:last-child`、`:nth-child(n)`、`:nth-child(odd)`、`:nth-child(even)`

优先级规则接近浏览器：id 高于 class / 属性 / 伪类，高于 tag；同优先级按源码顺序覆盖；内联 `style` 最后覆盖。

暂不支持：

- 兄弟选择器 `+` / `~`
- `:not()`、`:has()`、`:is()`
- CSS 变量
- 动画和 transition
- 外部 CSS 文件自动加载

## 媒体查询

支持基础 `@media`：

- `(min-width: Npx)`
- `(max-width: Npx)`
- `(min-height: Npx)`
- `(max-height: Npx)`
- 多条件可用 `and` 连接

窗口尺寸变化后会重新计算样式和布局。示例：

```css
@media (max-width: 900px) {
  .right-panel {
    display: none;
  }
}
```

## CSS 属性

### 布局

| 属性 | 值 |
| --- | --- |
| `display` | `flex`、`none` |
| `position` | `relative`、`absolute` |
| `left` / `top` / `right` / `bottom` | `px`、`%`、`auto` |
| `width` / `height` | `px`、`%`、`auto` |
| `min-width` / `min-height` | `px`、`%` |
| `max-width` / `max-height` | `px`、`%` |
| `margin` / 单边 `margin-*` | 1-4 值，`px` / `%` / `auto` |
| `padding` / 单边 `padding-*` | 1-4 值，`px` / `%` |
| `flex-direction` | `row`、`column` |
| `flex-wrap` | `nowrap`、`wrap` |
| `flex-grow` / `flex-shrink` | 数字 |
| `align-items` / `align-self` | `stretch`、`center`、`flex-start`、`flex-end` |
| `justify-content` | `flex-start`、`center`、`flex-end`、`space-between` |

布局由 Yoga 计算。当前没有实现 `gap`、`row-gap`、`column-gap`，需要用 margin 表达间距。

### 绘制

| 属性 | 说明 |
| --- | --- |
| `color` | 文本色；`progress` 的填充色；SVG 的 `currentColor` |
| `background-color` | 背景色 |
| `background` | 支持颜色、`linear-gradient(...)`、`linear-gradient(to right, ...)`、`linear-gradient(to bottom, ...)`、`linear-gradient-x(...)`、`linear-gradient-y(...)`、`radial-gradient(...)` |
| `border` | 简单 shorthand：宽度、`solid` / `none`、颜色 |
| `border-color` | 边框色 |
| `border-width` | 数字或 `px` |
| `border-style` | `solid`、`none` |
| `border-radius` | 1-4 个数字或 `px`，按 CSS shorthand 顺序展开四角 |
| `border-top-left-radius` / `border-top-right-radius` / `border-bottom-right-radius` / `border-bottom-left-radius` | 数字或 `px` |
| `font-size` | 数字或 `px` |
| `font-weight` | `bold`、`600`、`700` 为粗体；其他为常规 |

颜色支持：

- `transparent`
- `white`
- `black`
- `#RRGGBB`
- `rgb(r,g,b)`
- `rgba(r,g,b,a)`，`a` 可用 `0-1` 或 `0-255`

`border-radius` 展开规则与浏览器 CSS shorthand 一致：一个值应用到四角，两个值为左上/右下与右上/左下，三个值为左上、右上/左下、右下，四个值为左上、右上、右下、左下。当前只支持圆形半径，不支持 `border-radius: 8px / 4px` 这类椭圆半径语法。分角圆角会作用于背景、边框、`progress` 填充和 `overflow` 裁剪。

### 滚动和裁剪

| 属性 | 值 |
| --- | --- |
| `overflow` | `visible`、`hidden`、`auto`、`scroll` |
| `overflow-x` / `overflow-y` | 同上 |
| `scrollbar-gutter` | 支持包含 `stable` 的值 |

行为：

- 滚动容器会裁剪子元素。
- 鼠标滚轮默认纵向滚动；`Shift + 滚轮` 横向滚动。
- 支持拖动滚动条 thumb，点击滚动条轨道。
- `scrollbar-gutter: stable` 会给滚动条保留独立占位，避免滚动条盖在内容上。
- 虚拟滚动通过 `data-virtual-width` / `data-virtual-height` 设置内容尺寸，实际 DOM 可以只保留可见池化节点。
- 设置虚拟尺寸后，滚动范围以 `data-virtual-width` / `data-virtual-height` 为准，不会被池化节点临时移动到很远位置撑大。
- 滚动发生时会向 `data-action` 节点发出 `Scroll` 事件，事件中包含 `scrollX` / `scrollY`。

### 交互

| 属性 | 值 |
| --- | --- |
| `pointer-events` | `auto`、`none` |
| `cursor` | `auto`、`default`、`pointer`、`text`、`ew-resize`、`col-resize`、`e-resize`、`w-resize`、`ns-resize`、`row-resize`、`n-resize`、`s-resize`、`move`、`crosshair`、`not-allowed` |

`cursor` 会继承。Win32 宿主会把 `skui::Cursor` 映射到系统鼠标光标。

## 输入框能力

`input` 和 `textarea` 支持：

- 鼠标点击定位光标
- 鼠标拖拽选区
- 双击选词
- `Ctrl+A` / `Ctrl+C` / `Ctrl+X` / `Ctrl+V` / `Ctrl+Z`
- Backspace / Delete
- 左右方向键
- Shift + 左右方向键扩展选区
- Home / End
- IME composition / commit

`input` 会把换行和 tab 规整为空格。`textarea` 会保留换行，并把 CRLF 规整成 LF。

## 可复制文本

普通文本默认不可框选复制。需要复制时使用：

```html
<selectable class="message-text">能把最新的 Q2 报告发我吗?</selectable>
```

`selectable` 支持拖拽选择、双击选中单词或单个非 ASCII 字符、`Ctrl+A`、`Ctrl+C`。

## 图片和 SVG

- `img[src]` 当前只读取 SVG 文本资源。
- 内联 `<svg>` 和 SVG 文件都交给 Skia `SkSVGDOM` 渲染。
- 支持将 SVG 中的 `currentColor` 替换为当前节点 CSS `color`。
- SVG 会按节点布局盒缩放绘制，并裁剪到节点盒。

## 事件模型

只有命中的节点带 `data-action` 时，业务层才需要关心事件。回调类型见 `src/skui/public/skui_runtime.h`：

- `MouseDown`
- `MouseMove`
- `MouseUp`
- `Click`
- `Input`
- `Scroll`

`ElementEvent` 包含：

- `tag`
- `id`
- `classes`
- `action`
- `text`
- `value`
- `x` / `y`
- `scrollX` / `scrollY`
- `button`

## 运行时更新接口

对外主要通过元素 `id` 更新：

- `addClassById`
- `removeClassById`
- `setStyleById`
- `setStylesById`
- `setTextById`
- `setTextsById`
- `setAttributeById`
- `setAttributesById`
- `applyUpdates`
- `removeAttributeById`
- `hasClassById`

大量列表、表格、聊天记录建议使用批量接口 `applyUpdates`，减少重复样式计算和布局。

运行时更新规则：

- `setStyleById` 和 `RuntimeUpdates::styles` 会替换该节点完整内联 `style` 声明，不会与旧内联样式做增量合并。
- `setTextById` 和 `RuntimeUpdates::texts` 更新节点文本；输入框和进度条的当前值应通过 `value` 属性更新。
- `setAttributeById`、`setAttributesById`、`removeAttributeById` 会同步已知属性到内部状态，包括 `id`、`class`、`style`、`value`、`max`、`placeholder`、`src`、`data-action`、`data-virtual-width`、`data-virtual-height`。
- `class` 属性更新后会重新参与选择器匹配；`style` 属性更新后会重新解析内联样式。
- `applyUpdates` 会按样式、文本、属性的顺序批量应用，并只请求一次重新布局。

## 当前限制

- 没有 JavaScript。
- 没有完整浏览器表单控件。
- 没有完整 CSS 标准、外部 stylesheet、动画。
- 没有通用图片解码；`img` 目前面向 SVG 图标。
- 文本排版是单行或简单多行编辑框，不是富文本排版引擎。
- 中文双击选词目前按单个非 ASCII 字符处理，不做自然语言分词。
- 虚拟滚动需要业务层监听 `Scroll` 并更新池化 DOM；SkUI 只提供滚动范围、裁剪和事件。
