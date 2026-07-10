# Performance Notes

## 图片解码缓存需要预算和淘汰

现象：包含大量或可切换图片的页面会把已解码位图长期留在内存里。滚动、切换 `img`
的 `src`，或者浏览长图片列表时，内存会随历史访问图片数量增长。

影响范围：`Runtime` 通过 Skia 解码的位图图片缓存。SVG 当前不是这个缓存的对象。

参考项目：Chromium 的 `GpuImageDecodeCache` 把图片解码结果放在有预算的 cache 里，并区分
正在使用的工作集和可淘汰项；WebKit 的 `MemoryCache` 也会 prune decoded data，而不是永久保留
所有历史图片。

解决方案：SkUI 的位图缓存记录每个 `Ready` 条目的解码字节数和最近使用帧。每帧渲染后、
以及后台加载完成后，如果缓存总字节数超过 `RuntimeOptions::bitmapCacheBudgetBytes`，就淘汰
不是当前帧使用的最旧 `Ready` 条目。当前帧工作集会被保护，因此单张大图不会在正在绘制时被清掉。

验证方式：

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul && cmake --build --preset ninja-vcpkg-release && ctest --test-dir build\ninja-vcpkg --output-on-failure'
```

快速检查清单：

- 切换图片源后，旧图片超过预算时应被淘汰。
- 再次切回旧图片时，第一帧可以显示背景色，随后异步重新加载并渲染图片。
- 当前帧正在使用的图片不能为了满足预算被立即删除。
- 关闭 runtime 时，缓存字节数要和缓存条目一起清零。

## 图片请求需要区分 eager 和 lazy

现象：按钮按下态、hover 态和动态换图如果等到第一次绘制才请求图片，第一次切换会短暂显示空白；但图片滚动列表如果所有 `img` 都提前请求，又会造成大量后台解码和缓存压力。

影响范围：`Runtime` 中的非 SVG `img` 位图请求、状态图切换和长列表图片加载。SVG 文件仍按文本资源路径读取，不走位图请求队列。

参考项目：浏览器会把普通 `<img>` 当作 eager 资源建立请求，即使元素当前不可见；同时通过 `loading="lazy"` 让页面作者显式选择懒加载。浏览器在同一图片元素换源时也会区分 current image 和 pending image，避免新资源未完成前立刻清空旧画面。

解决方案：SkUI 默认在样式重算后扫描 DOM 中的非 SVG `img` 并建立异步请求，覆盖 `display:none` 的状态图，防止首次显示时闪空。显式 `loading="lazy"` 的图片跳过这个提前请求，只在进入绘制路径时排队，适合 `SkiaImageScrollerDemo` 这类大量缩略图滚动列表。同一个 `img` 切换 `src` 时保留上一张已显示位图，直到新图片 ready 后替换。

验证方式：

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul && cmake --build build\ninja-vcpkg --target SkuiInteractionTests SkiaImageScrollerDemo && build\ninja-vcpkg\SkuiInteractionTests.exe'
```

快速检查清单：

- 普通隐藏状态图应该在显示前已经完成异步请求。
- `loading="lazy"` 的隐藏图不应该在 `display:none` 阶段提前请求。
- lazy 图片首次进入绘制路径后应能异步加载，并通过 `requestRedraw` 驱动下一帧显示。
- 同一个 `img` 切换 `src` 时，新图未完成前应继续显示旧图。
- 大图滚动列表应优先使用 `loading="lazy"`，避免 DOM 阶段请求全部图片。

## 滚动平滑度需要同时保证状态插值和帧调度

现象：鼠标滚轮会把内容一次性跳到新的偏移，拖动滚动条滑块时则会出现停顿和跨帧跳跃，
整体手感明显弱于浏览器。

影响范围：SkUI 的纵向和横向滚轮滚动、滚动条滑块拖动，以及所有依赖
`RuntimeOptions::requestRedraw` 连续刷新的动画。

根因：

- 运行时收到滚轮事件后直接写入最终 `scrollX` / `scrollY`，没有目标偏移和时间插值。
- Win32 使用普通无效区刷新；连续鼠标移动期间，低优先级 `WM_PAINT` 可能被合并或延后。
- DXGI 交换链使用同步间隔 0 呈现，没有用显示器刷新节奏约束连续动画帧。

排查路径：

1. 从 `Runtime::handleEvent()` 的 `MouseWheel` 分支确认偏移是否一次写到最终值。
2. 检查动画是否持续调用 `requestRedraw`，以及每次渲染后是否仍会请求下一帧。
3. 检查鼠标捕获期间的 `WM_MOUSEMOVE` 是否会立即提交脏帧。
4. 检查 `Present1` 的同步间隔，排除无节奏快速提交造成的抖动和撕裂。

最终解决方案：

- 为滚动节点维护可累加的目标偏移，并使用保留速度的临界阻尼弹簧追踪目标。
- 同一方向的后续滚轮事件只扩展目标，不同步跳变位置，也不重置已有速度和计时。
- 一个轴首次启动时允许小幅预响应；另一个轴已有动画不会阻止该轴独立启动。
- 点击或拖动滚动条时取消该节点的滚轮动画，滑块继续与指针保持一比一映射。
- 鼠标捕获期间立即绘制拖拽产生的脏帧；动画重绘消息也立即进入绘制。
- DXGI 使用同步间隔 1 呈现，让动画帧跟随显示器刷新节奏。

验证方式：

```powershell
cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build\ninja-vcpkg && ctest --test-dir build\ninja-vcpkg --output-on-failure'
```

可复用经验：平滑滚动不是单独修改 easing 就能解决。必须同时检查输入状态更新、动画帧续订、
窗口消息优先级和交换链呈现节奏；任何一层仍是离散跳变，最终看到的滚动都会不连贯。

快速检查清单：

- 第一帧只能移动部分距离，不能直接到最终偏移。
- 动画未结束时，每帧都必须继续请求重绘。
- 连续滚轮输入应累加目标并保留速度，不能在每次事件时重启动画。
- 活动轴的连续输入不能同步修改当前位置，避免形成阶梯式顿落。
- 拖动滑块时应取消惯性动画，滑块位置必须直接跟随指针。
- 内容尺寸变化后，动画目标和当前偏移都必须重新限制在有效范围内。
- Windows 呈现路径应按垂直同步提交连续动画帧。
