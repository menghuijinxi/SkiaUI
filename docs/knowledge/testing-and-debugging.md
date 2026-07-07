# 测试与调试经验

## 自绘 Win32 窗口吞掉鼠标按下后输入失效

- 现象：SkUI demo 窗口里鼠标按下后，按钮 active 态、点击后的文本输入或键盘操作表现为丢失。
- 影响范围：通过 `Win32EventAdapter` 接收 `WM_LBUTTONDOWN`、`WM_MBUTTONDOWN`、`WM_RBUTTONDOWN` 或双击的 Win32 demo。
- 根因：adapter 在处理 client-area button-down 时直接返回 `0`，只调用 `SetCapture(hwnd)`，没有把 keyboard focus 显式交给 host window。自绘窗口没有子控件替它接管焦点，后续 `WM_KEYDOWN`、`WM_CHAR`、IME 输入容易断在窗口边界。
- 排查路径：先用 `Runtime::handleEvent` 直发 `MouseDown` 证明核心 Runtime 会触发回调、dirty 和 active 渲染；再在 `Win32EventAdapter::handleMessage(WM_LBUTTONDOWN)` 层加回归，确认失败点是 `GetFocus() != hwnd`。
- 最终方案：把 button-down 和 double-click 的共同入口封装为 `beginMousePress(hwnd)`，按下时先 `SetFocus(hwnd)` 再 `SetCapture(hwnd)`。
- 验证方式：`SkuiInteractionTests` 覆盖 adapter 层的 MouseDown 回调、dirty repaint callback、active 像素变化和 host window focus；完整构建后运行 `ctest --test-dir build\ninja-vcpkg --output-on-failure`。
- 可复用经验：自绘 Win32 host 一旦选择消费鼠标消息并跳过 `DefWindowProc` 的默认处理，就必须显式补齐窗口激活、焦点、capture 和重绘调度这些平台边界行为。
- 参考文档：[SetFocus](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setfocus)、[WM_MOUSEACTIVATE](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mouseactivate)。
