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
