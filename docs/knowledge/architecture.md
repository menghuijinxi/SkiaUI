# 架构决策记录

## Windows 文字缩放进入统一内容倍率

- 日期：2026-07-18
- 影响范围：SkUI Runtime、Win32/DX12 宿主、自定义后端接入。

### 现象

Windows“文本大小”大于 100% 时，如果只把每个节点的 `font-size` 乘以系统文字倍率，
字体物理尺寸虽然接近浏览器，但盒模型和 CSS 视口仍保持原尺寸，导致换行、溢出、重叠和响应式
布局与 Chromium 明显不同。

### 根因

Chromium Windows 显示层把文字倍率同时作为独立状态保留，并合入内容设备倍率：

```text
combinedDeviceScale = nativeDpiScale * textScaleMultiplier
```

普通页面使用该合并倍率进行布局、绘制和输入坐标转换。只有页面显式声明
`<meta name="text-scale" content="scale">` 时，Blink 才从布局倍率中除掉文字部分，交给页面处理。

参考源码：

- [Chromium `screen_win.cc`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/display/win/screen_win.cc#72)
- [Chromium `display.h`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/display/display.h#147)
- [Blink `web_frame_widget_impl.cc`](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/core/frame/web_frame_widget_impl.cc#2423)

### 最终方案

SkUI 的普通页面采用相同的默认模型：

```text
effectiveScale = dpiScale * userScale * textScale
```

`effectiveScale` 统一用于 CSS 逻辑视口、布局、Skia 绘制和输入命中坐标。解析器不再遍历 DOM
修改各节点的 `font-size`。`textScale` 仍作为独立状态保留，供 Win32 开关、动态更新以及未来实现
`text-scale` 页面声明时使用。

顶层 Win32 窗口和系统绘制的非客户区仍按原生 DPI 创建，不把文字倍率用于调整外层窗口尺寸；
文字设置变化只重新布局和重绘客户区内容。

### 验证方式

- 断言 Runtime 的有效倍率和逻辑视口同时包含 DPI、用户倍率和文字倍率。
- 在同一离屏画布分别以 1×、2× 文字倍率渲染，确认字体和非文本方块使用相同物理倍率。
- 启动 Win32 demo，确认系统文字倍率开启时页面正常重排，关闭开关后恢复仅 DPI 缩放。

### 快速检查清单

- 字体大小正确但控件重叠时，先确认是否只改了 `font-size`。
- 视口、绘制和输入必须读取同一个 `effectiveScale`。
- 平台 DPI 与文字倍率应分别保存，不能丢失文字倍率的独立语义。
- 不要把文字倍率应用到 Windows 原生边框等非客户区尺寸。
