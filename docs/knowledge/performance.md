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

## 滚轮输入采用直接偏移映射

现象：为滚轮加入目标曲线、速度积分或惯性后，连续输入会出现段落感、延迟或周期性停顿。
多轮参数调整仍会在不同滚轮节奏下产生新的手感问题。

影响范围：SkUI 的纵向和横向鼠标滚轮滚动。滚动条滑块拖动本来就是指针到偏移的一比一映射。

最终取舍：

- 不再为鼠标滚轮维护目标位置、速度、摩擦、惯性或缓动状态。
- 每个滚轮事件使用 `-wheelDelta / 120 * 48` 计算本次偏移，并在同一个事件中直接写入
  `scrollX` 或 `scrollY`。
- 按住 Shift 时把同一偏移映射到横向滚动；纵向和横向偏移都只在有效滚动范围内 clamp。
- 滚轮事件不会请求后续动画帧；滚动条位置和 `Scroll` 事件中的偏移立即反映本次输入结果。
- 滚动条点击和拖动继续直接更新偏移，不经过滚轮动画状态。
- CSS opacity/transform transition、鼠标拖动的即时重绘和 DXGI 垂直同步仍保留，
  与滚轮直接映射互不耦合。

验证方式：

```powershell
cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build build\ninja-vcpkg && ctest --test-dir build\ninja-vcpkg --output-on-failure'
```

滚动帧节奏监控：

```powershell
$env:SKIATEST_FRAME_PACING_CSV="$PWD\tmp\frame-pacing\scroll.csv"
$env:SKIATEST_FRAME_PACING_FLUSH="1"
.\build\ninja-vcpkg\SkiaDynamicDomDemo.exe
Remove-Item Env:SKIATEST_FRAME_PACING_CSV
Remove-Item Env:SKIATEST_FRAME_PACING_FLUSH
```

- `wheel_input` 和 `pointer_drag_input` 分别记录滚轮与按住鼠标拖动产生的输入。
- `frame_presented` 记录单帧工作耗时、输入到呈现延迟、重绘消息排队时间和后端类型。
- 只有上一帧明确请求了下一帧时，`continuous_interval=1` 的帧间隔才参与掉帧统计；
  滚动到边界后没有视觉变化的输入空档不会被误报为掉帧。
- `summary` / `scroll_summary` 中的 `late_frames`、`missed_frames`、
  `worst_interval_ms` 和 `worst_input_latency_ms` 用于快速判断连续动画与滑块拖动是否卡顿。
- 需要继续定位具体渲染阶段时，可同时设置 `SKIATEST_PERF_CSV`，关联
  `runtime_render`、`ganesh_flush_submit` 和 `present` 等详细阶段。

2026-07-10 的后续 120 Hz 滚动轨迹中，349 个连续帧的平均间隔为 8.093 ms，
P95 为 8.509 ms，最差为 10.151 ms；没有超预算帧，也没有估算漏帧。
该轨迹包含 16 次滚轮输入，输入间隔约为 172-313 ms，未包含滑块拖动。
这组数据说明当时感知到的“段落感”不是掉帧，而是滚轮动画状态和输入节奏相互作用造成的错觉。
最终删除滚轮动画状态，以确定、即时的直接映射换取稳定行为。

可复用经验：输入设备手感无法仅靠一组通用的 easing 或摩擦参数覆盖。没有完整的平台级手势、
合成线程和设备分类能力时，直接映射通常比自定义惯性模型更可预测。

快速检查清单：

- `wheelDelta=-120` 应当在同一个事件中增加 48px 偏移，反向输入立即减去 48px。
- 连续同向输入应逐次累加，并在内容边界处 clamp。
- 滚轮输入不应创建动画状态，也不应请求后续动画重绘。
- `Scroll` 回调应立即收到更新后的 `scrollX` / `scrollY`。
- Shift+滚轮应直接更新横向偏移。
- 拖动滑块时，滑块位置必须直接跟随指针。
