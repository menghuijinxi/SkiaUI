# SkUI CSS / DOM 支持说明

这份文档记录当前 SkUI 运行时已经实现的 HTML、DOM、CSS 和交互能力。SkUI 不是浏览器内核，也不承诺完整 HTML/CSS 标准；它是面向 Skia 原生 UI 的轻量子集。

## HTML 解析

- 使用 Lexbor 解析 HTML。
- 只读取 `<body>` 下的元素作为 UI 树。
- `<style>` 和本地 `<link rel="stylesheet" href="...">` 会被解析成样式规则；`<head>`、`<meta>`、`<title>`、`<script>` 不会生成可绘制节点。
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
| `div[contenteditable]` | 浏览器式编辑宿主；普通 `p` / `div` 文本容器可编辑，`contenteditable="false"` 子树作为不可编辑原子节点 |
| `selectable` | 可框选复制文本标签，支持自动折行、`<br>` 显式换行和内联 `<a href>`；普通文本默认不可选中 |
| `progress` | 进度条，`value` / `max` 控制填充比例 |
| `img` | 加载本地图片资源；SVG 走 SVG DOM，位图走异步加载 |
| `video` | FFmpeg 软件播放；支持显式预解码、音频同步、循环和 VP8/VP9 Alpha WebM |
| `svg` | 内联 SVG，由 Skia SVG DOM 绘制 |

`select` / `option` 目前没有浏览器式原生下拉控件行为。需要下拉框时，用普通节点组合按钮、菜单、遮罩和选项；C++ 侧可复用 `src/skui/public/skui_dropdown.h` 里的 `skui::DropdownState`，它负责打开/关闭、同步选中文本、切换箭头文本、显示/隐藏菜单和选中项 class。

## 通用属性

| 属性 | 说明 |
| --- | --- |
| `id` | 运行时更新和事件识别的主要定位方式 |
| `class` | 支持多 class，参与 CSS 匹配 |
| `style` | 内联 CSS 声明，优先级高于 `<style>` 规则 |
| `data-action` | 命中事件回调中的业务动作名 |
| `data-links` | `selectable` 的兼容/运行时文本区间动作表，格式为每行 `start:end:action`，区间按 `value` 的 UTF-8 字节偏移计算 |
| `value` | 输入框值；进度条当前值 |
| `max` | 进度条最大值 |
| `placeholder` | 输入框占位文本 |
| `contenteditable` | 枚举属性；支持 `true`、空值、`false`、`plaintext-only` 和从父节点继承 |
| `href` | `selectable` 内 `<a>` 的链接目标；点击时转换为 `open-url:` 动作 |
| `src` | `img` / `video` 的本地资源路径；video 也可直接交给 FFmpeg 打开 URL |
| `preload` | video 支持 `none`、`metadata`、`auto`；只有 `auto` 提前解码帧 |
| `data-predecode-frames` | video 显式预解码的高水位帧数 |
| `autoplay` / `loop` / `muted` | video 自动播放、有界无缝循环和静音属性 |
| `disabled` | 禁用当前节点及其子树的指针、文本选择和输入交互；不自带灰显外观 |
| `data-virtual-width` / `data-virtual-height` | 虚拟滚动内容尺寸，不需要真实子元素撑开 |

`disabled` 同时参与 `:disabled` 伪类匹配和实际交互禁用；`checked`、`selected` 当前主要用于 CSS 伪类匹配，不等同于完整浏览器控件状态。

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
- 结构伪类：`:first-child`、`:last-child`、`:nth-child(An+B)`、`:nth-child(odd)`、`:nth-child(even)`
- 否定伪类：`:not(...)`，参数支持单个复合选择器
- 装饰伪元素：`::before`、`::after`；由 `content` 创建，支持绝对定位盒绘制

优先级规则接近浏览器：id 高于 class / 属性 / 伪类，高于 tag；同优先级按源码顺序覆盖；内联 `style` 最后覆盖。

暂不支持：

- 兄弟选择器 `+` / `~`
- `:has()`、`:is()`，以及 `:not()` 内的选择器列表或复杂选择器
- CSS 变量
- 完整 CSS Animation 标准；当前提供面向雪碧图的 `@keyframes` / `animation` 子集
- 远程 CSS（`http://` / `https://`）和 `data:` stylesheet

## 外部样式表

支持通过本地文件拆分 CSS：

```html
<link rel="stylesheet" href="dynamic_dom.css">
```

`href` 相对路径按当前 HTML 文档的 `basePath` 解析；通过 `Runtime::loadDocument(path)` 加载时，`basePath` 是 HTML 文件所在目录；通过 `loadDocumentFromString(html, basePath)` 加载时，由调用方显式传入。外部样式表与 `<style>` 按文档出现顺序进入同一个级联规则列表，后出现的同优先级规则会覆盖先出现的规则。

当前只加载本地 CSS 文件；`http://`、`https://` 和 `data:` stylesheet 会被忽略。

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
| `display` | `flex`、`inline-flex`、`grid`、`inline-grid`、`none` |
| `visibility` | `visible`、`hidden` |
| `position` | `relative`、`absolute` |
| `z-index` | `auto`、正负整数 |
| `left` / `top` / `right` / `bottom` | `px`、`%`、`auto` |
| `inset` | 1-4 值，`px` / `%` / `auto` |
| `width` / `height` | `px`、`%`、`auto` |
| `min-width` / `min-height` | `px`、`%` |
| `max-width` / `max-height` | `px`、`%` |
| `margin` / 单边 `margin-*` | 1-4 值，`px` / `%` / `auto` |
| `padding` / 单边 `padding-*` | 1-4 值，`px` / `%` |
| `flex-direction` | `row`、`column` |
| `flex-wrap` | `nowrap`、`wrap` |
| `gap` / `row-gap` / `column-gap` | `px` / `%`，用于 flex 容器 |
| `flex-grow` / `flex-shrink` | 数字 |
| `flex` / `flex-basis` | 常用 1-3 值 shorthand；basis 支持 `px` / `%` / `auto` |
| `grid-template-columns` | `repeat(N, track)` 或轨道列表；支持 `px`、`%`、`fr`、`auto`、`minmax(0, track)` |
| `grid-template-rows` | 轨道列表；支持 `px`、`%`、`auto`，以及全 `fr` 轨道列表 |
| `grid-auto-rows` | 单个 `px`、`%`、`fr` 或 `auto` 轨道 |
| `grid-column` / `grid-column-start` / `grid-column-end` | 正负整数网格线、`auto`、`span N`；支持数值列定位和列跨度 |
| `justify-items` / `justify-self` | `auto`、`normal`、`stretch`、`start`、`center`、`end` 及常见 `flex-*` / `left` / `right` 别名 |
| `align-items` / `align-self` | `stretch`、`center`、`flex-start`、`flex-end` |
| `justify-content` | `flex-start`、`center`、`flex-end`、`space-between` |

布局由 Yoga 计算。显式 `display:flex` 的容器使用浏览器默认的 `flex-direction: row`；未显式设置 `display` 的旧写法保持原有默认纵向排布。

Grid 通过 Yoga 的横向换行布局和内部单元格节点实现，覆盖等分 `repeat(N, 1fr)`、固定/弹性混合列轨道、数值列线定位、固定同单位或全 `fr` 列跨度、显式行轨道和单项水平对齐。内部单元格不进入 DOM，也不影响选择器、事件目标和绘制顺序。它不是完整 CSS Grid 算法，暂不支持命名线、`grid-row` 显式行定位、`grid-template-areas`、固定与 `fr` 混合的行轨道分配、`auto-fit` / `auto-fill`、dense 自动放置和复杂隐式轨道。

`display:flex` 使用 `align-items` 和 `justify-content` 控制内容对齐。Grid 使用 `align-items` / `align-self` 控制单元格块轴对齐，使用 `justify-items` / `justify-self` 控制单元格行内轴对齐。没有显式 Flex 或 Grid 容器的普通文本和 `selectable` 不会因为这些属性而居中。

文本叶节点显式设置 `width:auto` / `height:auto` 时，与省略对应尺寸一致，使用文本固有尺寸参与布局；最终背景框会同时包含文本、padding 和 border。宽高均为固定值时不再执行固有尺寸测量。

`z-index` 按同级子树排序，数值越大越晚绘制并优先接收鼠标事件；`auto` 等同于默认层级 `0`，相同层级保持 DOM 顺序。当前实现把每个直接子节点及其后代作为一个整体排序，不实现浏览器完整的跨层叠上下文提升；需要覆盖另一个父节点下的元素时，应把层级设置在两边参与比较的同级祖先上。

显隐语义：

- `display:none` 会让节点退出布局和绘制，也不会参与命中测试；后续兄弟节点会重新排布。
- `visibility:hidden` 会保留节点布局占位，但节点及其子树不会绘制，也不会参与命中测试。

### 绘制

| 属性 | 说明 |
| --- | --- |
| `color` | 文本色；`progress` 的填充色；SVG 的 `currentColor` |
| `background-color` | 背景色 |
| `background` | 支持颜色、单独的 `url(...)`、多层线性/径向渐变；线性渐变支持角度、方向、`px` / 百分比色标，径向渐变支持 `at` 定位 |
| `background-image` | 本地位图 `url(...)` 或 `none` |
| `background-size` | 1-2 个 `px`、百分比或 `auto`；百分比相对元素背景区域 |
| `background-position` | 1-2 个 `px`、百分比或 `left` / `right` / `top` / `bottom` / `center`；百分比按浏览器的“容器尺寸减背景尺寸”公式计算 |
| `background-repeat` | `repeat`、`no-repeat`、`repeat-x`、`repeat-y` |
| `mask-image` / `-webkit-mask-image` | 单层线性或径向渐变 alpha 遮罩 |
| `border` | 简单 shorthand：宽度、`solid` / `none`、颜色 |
| `border-color` | 边框色 |
| `border-width` | 数字或 `px` |
| `border-style` | `solid`、`none` |
| `border-top` / `border-right` / `border-bottom` / `border-left` | 单边宽度、样式、颜色 shorthand；也支持对应的 `-color` / `-width` / `-style` 长属性 |
| `border-radius` | 1-4 个数字或 `px`，按 CSS shorthand 顺序展开四角 |
| `border-top-left-radius` / `border-top-right-radius` / `border-bottom-right-radius` / `border-bottom-left-radius` | 数字或 `px` |
| `box-shadow` | 逗号分隔的外阴影和 `inset` 阴影；支持偏移、模糊、扩散和颜色 |
| `text-shadow` | 逗号分隔的文字阴影；支持偏移、模糊和颜色 |
| `font-size` | 数字或 `px` |
| `font-weight` | `bold`、`600`、`700` 为粗体；其他为常规 |
| `filter` | `none`，或按顺序组合 `grayscale(...)`、`brightness(...)`、`drop-shadow(...)` |
| `content` | `::before` / `::after` 的创建条件；当前绘制子集使用空字符串装饰盒 |

渐变由 Skia `SkShaders` 生成，按 sRGB 插值并启用 Skia dithering，以减少低亮度 8 位颜色中的分层色带；渐变遮罩通过 Skia 离屏层和 `DstIn` 混合应用；`box-shadow`、`text-shadow` 和 `drop-shadow()` 分别使用 Skia 的路径、`SkMaskFilter` 与 `SkImageFilters`。这些效果没有调用浏览器内核，但 CSS 参数会映射到对应的 Skia 原生绘制能力。外阴影会裁掉盒内区域，`inset` 阴影通过“外部区域减去内孔”的路径向内模糊，不再使用粗描边近似。

`filter` 会在节点及其完整子树绘制完成后统一处理，因此背景、图片、文字、边框和不规则透明区域会一起变色，不会额外生成矩形遮罩。`drop-shadow()` 使用绘制结果的 alpha 轮廓生成阴影；暂不支持通用 `blur()` 和滤镜动画。例如：

```css
.disabled-look {
  filter: grayscale(100%) brightness(72%);
}
```

颜色支持：

- `transparent`
- `white`
- `black`
- `#RRGGBB`
- `rgb(r,g,b)`
- `rgba(r,g,b,a)`，`a` 可用 `0-1` 或 `0-255`

### 过渡动画

`transition` shorthand 当前支持 `height`、`opacity` 和 `transform`，可设置时长、延迟以及
`linear`、`ease` / `ease-in` / `ease-out` / `ease-in-out`、`cubic-bezier(...)` 缓动。

`height` 过渡会在每帧重新执行布局，因此 flex 容器中的后续兄弟节点会随高度插值平滑移动。
当前只插值两端均为明确数值且单位相同的高度，例如 `75px → 560px` 或 `20% → 60%`；
`auto`、缺省高度和不同单位之间按离散状态切换。

### 关键帧动画

当前支持轻量 CSS Animation 子集，覆盖浏览器式雪碧图、透明度和基础 transform 动画：

- `@keyframes` 和 `@-webkit-keyframes`
- `from`、`to`、百分比以及逗号分隔的关键帧选择器
- `animation` shorthand，可设置时长、延迟、迭代次数 / `infinite`、方向、fill mode、播放状态
- `linear`、`ease` / `ease-in` / `ease-out` / `ease-in-out`、`cubic-bezier(...)`、`step-start`、`step-end`、`steps(n, start|end)`
- 动画时间线只由 `Runtime::tick(deltaSeconds)` 推进，`Runtime::render()` 只绘制当前状态
- `tick()` 返回是否仍需要后续动画帧；引擎可传入固定步长或本帧真实增量，实现与自身 tick 同步
- Win32 封装会在每次实际绘制前使用单调时钟自动调用 `tick()`；正在运行的动画掉帧后会按实际经过时间追赶
- 当前关键帧可动画化属性包括 `background-position`、`opacity` 和 `transform`

`transform` 会按 CSS 函数顺序保存和绘制，支持 `translate(...)` / `translateX(...)` / `translateY(...)`、`scale(...)` / `scaleX(...)` / `scaleY(...)`、`rotate(...)` 等基础函数；百分比位移按元素自身尺寸计算。不同关键帧之间的 `transform` 函数列表需要兼容，才能进行逐项插值；不兼容时会按离散状态切换。`transform-origin` 支持常用关键字、`px` 和百分比，可用于接近浏览器的旋转、缩放原点行为。

引擎循环中的典型调用顺序为：

```cpp
const bool animationPending = runtime.tick(deltaSeconds);
runtime.render(canvas);
```

即使 `animationPending` 为 `false`，业务数据、输入或窗口尺寸变化仍可能要求重新绘制；该返回值只表示动画系统是否还需要继续推进。

9 列、6 行、24 FPS 雪碧图可使用：

```css
.sprite {
  width: 72px;
  height: 72px;
  background-image: url("note_sprite.png");
  background-size: 900% 600%;
  background-repeat: no-repeat;
  animation: note-sprite-frames 2.25s step-end infinite;
}
```

加载后立刻播放的对比动画可以直接写在静态 HTML/CSS 中，不需要脚本：

```css
.fade-chip {
  animation: auto-fade 1.6s ease-in-out infinite;
}

.origin-chip {
  transform-origin: left top;
  animation: origin-swing 1.2s cubic-bezier(0.25, 0.1, 0.25, 1) infinite alternate;
}
```

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
- 运行时可按元素 id 读取滚动状态、设置绝对偏移、相对滚动，或把后代元素滚入所有可滚动祖先的可视区域。

程序化定位接口：

```cpp
runtime.setScrollOffsetById("message-list", 0.0f, 240.0f);
runtime.scrollById("message-list", 0.0f, 40.0f);
runtime.scrollIntoViewById("message-120");

if (const std::optional<skui::ScrollState> state =
        runtime.scrollStateById("message-list")) {
    // state->scrollY / state->maxScrollY / state->viewportHeight / state->contentHeight
}
```

偏移会限制在有效滚动范围内。只有位置实际变化时才会重绘并派发 `Scroll` 事件；`scrollIntoViewById` 使用 nearest 对齐，并按从内到外的顺序处理嵌套滚动容器。

虚拟滚动推荐分两层使用：

- 聊天记录、大列表这类单维数据，使用 `skui::VirtualWindowState` 计算首个可见项、滚动偏移、缓存数量和是否需要刷新。
- 表格这类行列数据，优先使用 `skui::VirtualTableAdapter`，由业务层提供 `VirtualTableDataSource`，适配器负责同步虚拟尺寸、移动池化行/单元格、更新文本、class 和 `data-action`。

### 交互

| 属性 | 值 |
| --- | --- |
| `pointer-events` | `auto`、`none` |
| `cursor` | `auto`、`default`、`pointer`、`text`、`ew-resize`、`col-resize`、`e-resize`、`w-resize`、`ns-resize`、`row-resize`、`n-resize`、`s-resize`、`move`、`crosshair`、`not-allowed` |

`cursor` 会继承。Win32 宿主会把 `skui::Cursor` 映射到系统鼠标光标。

带 `disabled` 属性的节点会禁用整棵子树：不进入 `:hover` / `:active`，不派发鼠标和点击事件，也不能编辑或选择文字。命中禁用节点时事件仍由 UI 消费，不会穿透到其下方的 UI 或 3D 场景。`disabled` 本身不改变视觉外观；如果需要灰显，应像浏览器页面一样通过 CSS 显式设置，例如 `button:disabled { filter: grayscale(100%) brightness(72%); }`。

### 指针事件与输入透传

`pointer-events: none` 用于纯视觉元素，例如遮罩、光效、装饰图和提示框背景。设置后该节点不会参与鼠标命中测试，鼠标事件会继续查找其下方可命中的元素；如果最终没有命中可交互元素，`Runtime::handleEvent` 会返回 `false`，宿主可以把事件继续交给下一层界面或 3D 场景。

```html
<div class="screen">
  <div class="visual-glow"></div>
  <button id="open-menu" data-action="open-menu">Open</button>
</div>
```

```css
.screen {
  width: 3840px;
  height: 2160px;
  background-color: transparent;
}

.visual-glow {
  position: absolute;
  left: 0;
  top: 0;
  width: 3840px;
  height: 420px;
  pointer-events: none;
}
```

运行时可以用 `Runtime::setConsumesEventsById(id, false)` 临时关闭某个元素的输入消耗能力，语义等价于给该元素设置 `pointer-events: none`；传入 `true` 则恢复为 `pointer-events: auto`。这个接口适合弹层动画、临时禁用按钮、装饰层显示隐藏等场景，不需要业务层手动拼接完整 `style` 字符串。

SkUI 的事件返回值表示“UI 是否实际消费了事件”，不是“DOM 是否发生 hover、active 或 focus 状态变化”。因此透明全屏根节点、普通 `div`、普通装饰 `img` 不会因为被命中就阻断宿主输入；只有 `data-action`、输入框、`selectable`、滚动条、拖拽选择等真实交互路径会消费事件。

滚轮命中真实交互路径时同样会被消费，即使当前没有可滚动内容或滚动位置已经到达边界。普通非交互区域只有在确实推动了可滚动祖先时才会消费滚轮。

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

## Contenteditable 文档流

富文本式输入区使用浏览器标准标签，不需要 SkUI 自定义编辑器标签：

```html
<div id="composer" contenteditable="true">
  <p id="paragraph-a">文件在这里：</p>
  <div id="file-a"
       contenteditable="false"
       data-node-type="attachment">report.pdf</div>
  <p id="paragraph-b"><br></p>
</div>
```

当前实现支持：

- `contenteditable` 的枚举值与继承；无效值按继承处理。
- 普通 `p`、`div`、`span`、标题、`li` 和 `blockquote` 文本叶节点的光标、选区、剪贴板、撤销和 IME。
- Enter 把当前文本容器拆成相邻普通段落。
- Backspace / Delete 删除相邻 `contenteditable="false"` 原子节点，或合并相邻文本段落。
- `Input` 事件以最外层编辑宿主为目标，和浏览器 editing host 一致。
- `Runtime::selection()` 返回 `Selection` / `Range` 状态；`collapseSelection()` 与 `setSelectionBaseAndExtent()` 用元素 id 设置选区。
- `insertHtmlAtSelection()` 在当前选区插入普通 HTML；块节点插入会拆分当前文本容器。

`textContentById()` 和 `childElementIdsById()` 是 C++ DOM 桥接接口，业务层可按实际文档顺序读取文本和原子节点。当前 `Selection` 只表示同一个文本容器内的范围；跨段落连续选择、嵌套内联富文本编辑、`beforeinput` / `inputType` 和浏览器完整编辑命令尚未实现。

## 可复制文本

普通文本默认不可框选复制。需要复制时使用：

```html
<selectable class="message-text">能把最新的 Q2 报告发我吗?</selectable>
```

`selectable` 支持拖拽选择、双击选中单词或单个非 ASCII 字符、`Ctrl+A`、`Ctrl+C`。文本会按节点宽度自动折行，`<br>` 会产生显式换行。同一个 `selectable` 内的普通文本、`<br>` 和 `<a>` 会合并成统一文本流，因此绘制、链接命中、选择高亮和复制使用相同的行布局；当前不支持跨多个 `selectable` 连续框选。

静态 HTML 中应直接使用 `<a href>`。点击链接且没有拖出选区时，会向 `RuntimeOptions::onElementEvent` 发出 `Click` 事件，`event.action` 为 `open-url:` 加 `href`，`event.text` 为链接文本，`event.value` 为完整文本。拖拽选择仍按普通 `selectable` 处理，`Ctrl+C` 会复制完整选区文本，不会被链接拆断。

```html
<selectable>
  first<br>
  <a href="https://example.com">open link</a><br>
  last
</selectable>
```

运行时如果直接替换整段字符串，仍可使用 `value` 和 `data-links`：`data-links` 每行格式为 `start:end:action`，偏移按 `value` 的 UTF-8 字节序计算。建议聊天消息、日志项保持“一条消息一个 `selectable`”，不要为了链接拆成多个 `selectable`。

## 图片和 SVG

- `img[src]` 支持本地资源路径。`.svg` 文件按 SVG 文本读取；位图通过 Skia codec 支持 PNG、JPEG、WebP 和 BMP 异步读取和解码。
- 位图图片默认按浏览器的 eager 语义处理：样式重算后会扫描 DOM 中的非 SVG `img` 并建立异步请求，即使节点当前是 `display:none`。这用于按钮 normal / active 图这类状态切换场景，避免第一次显示隐藏状态图时出现“闪空”。
- 大图列表、图片滚动墙等不希望提前请求全部图片的场景，应在图片上写 `loading="lazy"`。lazy 图片不会在 DOM 扫描阶段提前请求，而是在节点进入当前画布裁剪区域附近时进入后台加载队列。默认边距为视口四周各一个视口尺寸，宿主可通过 `RuntimeOptions::lazyImagePreloadMarginViewports` 调整。该策略接近浏览器的视口交叉预加载，但没有浏览器按网络状态动态调整距离的调度逻辑。`SkiaImageScrollerDemo` 的缩略图使用的就是这个模式。
- 同一个 `img` 运行时切换 `src` 时，renderer 会保留该节点上一张已经显示成功的位图，直到新路径图片加载完成后再替换，避免按下态、悬停态或动态换图时先清空再绘制。
- 相同解析路径的位图图片会复用缓存，不会为多个 `<img>` 重复加载。
- 位图绘制会按节点布局盒缩放，并裁剪到节点盒；节点设置 `border-radius` 时也会按圆角裁剪。
- 内联 `<svg>` 和 SVG 文件都交给 Skia `SkSVGDOM` 渲染。
- `img` 和内联 `svg` 的 HTML `width` / `height` 展示属性会作为低优先级尺寸参与布局，CSS 尺寸可以覆盖它们。
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
- `setValueById`
- `setTextsById`
- `setAttributeById`
- `setAttributesById`
- `applyUpdates`
- `removeAttributeById`
- `appendHtmlById`
- `prependHtmlById`
- `replaceHtmlById`
- `removeElementById`
- `insertHtmlAtSelection`
- `collapseSelection`
- `setSelectionBaseAndExtent`
- `selection`
- `textContentById`
- `childElementIdsById`
- `setVisibleById`
- `setConsumesEventsById`
- `hasClassById`

大量列表、表格、聊天记录建议使用批量接口 `applyUpdates`，减少重复样式计算和布局。需要动态增删节点时，可以使用 `appendHtmlById`、`prependHtmlById`、`replaceHtmlById` 和 `removeElementById` 维护指定容器下的子树。动态片段会按当前运行时样式规则重新计算样式和布局。

运行时更新规则：

- `setStyleById` 和 `RuntimeUpdates::styles` 会替换该节点完整内联 `style` 声明，不会与旧内联样式做增量合并。
- `setTextById` 和 `RuntimeUpdates::texts` 更新节点文本；输入框、进度条和需要保留 `\n` 换行的 `selectable` 多行文本应通过 `setValueById` 或 `value` 属性更新。
- `setAttributeById`、`setAttributesById`、`removeAttributeById` 会同步已知属性到内部状态，包括 `id`、`class`、`style`、`value`、`max`、`placeholder`、`src`、`data-action`、`data-links`、`data-virtual-width`、`data-virtual-height`。
- `class` 属性更新后会重新参与选择器匹配；`style` 属性更新后会重新解析内联样式。
- `applyUpdates` 会按样式、文本、属性的顺序批量应用，并只请求一次重新布局。
- `appendHtmlById` / `prependHtmlById` 会把 HTML 片段插入到目标父节点子列表尾部或头部。
- `replaceHtmlById` 会用 HTML 片段替换目标节点；片段为空或包含多个根元素时会返回失败。
- `removeElementById` 会删除目标节点及其子树；根节点不能删除。
- `setVisibleById` 是 `display:none` / `display:flex` 的便捷接口，用于隐藏后不占布局的场景；需要隐藏但保留布局占位时应使用 `setStyleById(id, "visibility:hidden;")` 或 class 切换。
- `setConsumesEventsById` 是 `pointer-events:auto` / `pointer-events:none` 的便捷接口，用于运行时控制元素是否消费鼠标输入；它只影响命中和事件消费，不改变绘制和布局。

动态 DOM 示例：

- `SkiaDynamicDomDemo` 演示运行时追加、替换、删除消息节点，以及 `display:none` 和 `visibility:hidden` 两种隐藏方式。
- `SkiaDynamicDomDemo` 同时演示 `transition` 驱动的移动、旋转、缩放、渐隐和渐显效果。
- `SkiaDynamicDomDemo` 内置加载即播放的 CSS 动画对比区，用于和浏览器核对 `opacity`、`rotate`、`scale`、`transform-origin` 和 transform 函数顺序。
- 示例页面使用滚动容器验证动态增删节点后滚动条、裁剪和自动布局仍能更新。

## 通用 C++ 辅助能力

这些能力不是 HTML 标签，而是可被其他项目直接复用的 public 头文件：

| 头文件 | 用途 |
| --- | --- |
| `skui_runtime_helpers.h` | `RuntimeUpdateBatch`、逻辑宽高读取、`px(...)`、内联 style 拼接、action payload 拆分 |
| `skui_dropdown.h` | 下拉框状态控制：打开/关闭、选中项同步、菜单和遮罩显隐、选中 class 切换 |
| `skui_virtual_window.h` | 通用窗口化渲染状态：根据滚动位置和视口高度计算可见池范围 |
| `skui_virtual_table.h` | 表格窗口化渲染、表格面板自适应、工具栏自动换行高度计算、表格池化 DOM 刷新 |

这些辅助类只处理状态、几何和运行时更新，不绑定具体视觉样式。视觉仍建议写在 HTML/CSS 中，C++ 只通过 id、class、文本和属性更新状态。

## 当前限制

- 没有 JavaScript。
- 没有完整浏览器表单控件。
- 没有完整 CSS 标准或外部 stylesheet。transition 覆盖 `height`、`opacity` 和 `transform`；关键帧动画覆盖
  `background-position`、`opacity` 和兼容函数列表内的 `transform`。
- Grid 是面向卡片/指标面板的布局子集；伪元素只支持绝对定位装饰盒和空 `content`，不参与 Yoga 布局或事件命中。
- 多层渐变共享当前 `background-size` / `background-repeat`；水平和垂直线性渐变支持按指定尺寸平铺，径向和任意角度渐变尚不支持完整二维平铺。`mask-image` 当前只支持单层渐变 alpha 遮罩，不支持 URL、多层遮罩和独立的遮罩尺寸、位置或重复属性。
- `img` 只支持本地资源路径；位图支持 PNG、JPEG、WebP 和 BMP。懒加载属性使用浏览器一致的 `loading="lazy"`，不支持旧式 `data-loading`。暂不支持网络 URL、`srcset` 和浏览器图片事件。
- 文本排版是单行、`selectable` 显式多行或简单多行编辑框，不是富文本排版引擎；`selectable` 暂不支持跨 DOM 节点连续选择。
- 中文双击选词目前按单个非 ASCII 字符处理，不做自然语言分词。
- 虚拟滚动需要业务层提供数据源并根据 `Scroll` 刷新池化 DOM；SkUI 提供滚动范围、裁剪、事件，以及 `VirtualWindowState` / `VirtualTableAdapter` 辅助类，但不会自动从任意 DOM 推导大数据源。
