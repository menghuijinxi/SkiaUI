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

## 持续动画让 Win32 窗口显示正常但无法交互

- 现象：CSS 雪碧动画能够持续显示，但窗口拖动、按钮点击、键盘输入和关闭操作表现为无响应。
- 影响范围：通过 `RuntimeOptions::requestRedraw` 连续驱动动画，并由 `Dx12WindowApp` 转换为 Win32 重绘请求的窗口。
- 根因：动画每次绘制都会请求下一帧；`kRequestRedrawMessage` 使用带 `RDW_UPDATENOW` 的 `RedrawWindow`，同步进入下一次 `WM_PAINT`。持续动画因此让 UI 线程长时间停留在“重绘消息 -> 同步绘制”的循环中，输入消息无法获得正常的调度优先级。
- 排查路径：确认 Runtime 只合并一次待处理重绘消息；继续沿 `requestRedraw` 检查 Win32 消息处理，发现自定义重绘消息强制同步绘制；最后确认 D3D swap chain 已使用 `Present1(1, ...)` 跟随垂直同步，不需要再用 `RDW_UPDATENOW` 追帧。
- 最终方案：自定义动画重绘消息只调用普通窗口失效，不同步触发绘制；让 Windows 在鼠标、键盘、窗口管理消息之后按低优先级派发 `WM_PAINT`。拖拽等明确需要低延迟反馈的交互路径仍可保留即时重绘。
- 验证方式：使用 Visual Studio 自带 CMake 构建 `SkiaDynamicDomDemo` 和测试目标；运行全部 CTest；让雪碧动画持续运行后检查进程 `Responding` 状态，并发送标准关闭消息，确认窗口在限定时间内正常退出。
- 可复用经验：持续动画应采用类似浏览器 `requestAnimationFrame` 的“请求下一帧但不递归同步绘制”模型。垂直同步只负责限制呈现频率，窗口消息循环仍必须保留输入优先级。
- 动画时钟应与绘制解耦：核心 `Runtime` 只在 `tick(deltaSeconds)` 中推进 transition 和 keyframes，`render()` 不读取墙钟。Win32 封装在 `WM_PAINT` 的实际绘制前自动 tick；接入游戏或其他引擎时由宿主循环传入自己的固定或可变时间步长。
- 快速检查清单：
  - 连续刷新回调是否在绘制过程中再次同步进入绘制。
  - `InvalidateRect` / `RedrawWindow` 是否错误使用 `RDW_UPDATENOW`。
  - 是否合并重复的重绘请求，避免消息队列无限增长。
  - swap chain 是否已经使用同步间隔，避免重复实现帧率限制。
  - 动画运行时是否能及时处理 `WM_CLOSE`、鼠标和键盘消息。

## 点击回调修改 class 导致 transition 起点闪烁

- 现象：点击切换两个带 `height` 或 `opacity` transition 的元素时，目标状态先完整显示一帧，随后回到旧状态再开始过渡；渲染负载较高时更容易观察到。
- 影响范围：`Runtime::handleEvent` 派发的业务回调中调用 class、style、visible 等会触发布局的 DOM 接口，尤其是回调内部还使用 `RuntimeUpdateBatch` 时。
- 根因：业务回调中的内层批处理结束后会立即刷新布局，但鼠标事件尚未完成 `:active`、`:hover` 和 pressed 状态更新；事件尾部随后再次请求布局，形成同一次输入中的两次样式提交。
- 最终方案：`Runtime::handleEvent` 自身建立最外层 `RuntimeUpdateBatch`。业务回调的批处理成为嵌套事务，所有 DOM 与指针状态变化在事件退出时统一提交。
- 验证方式：`SkuiInteractionTests` 在 Click 回调中切换两个带高度过渡的元素，并断言回调返回前不得触发动画重绘请求，同时确认最终 class 状态正确。
- 可复用经验：输入回调应具备浏览器式的样式提交边界。业务代码可以同步修改 DOM，但布局、transition 起点捕获和重绘调度应在当前输入事件全部处理完成后统一执行。
- 快速检查清单：
  - 动画是否出现“目标状态闪一帧，再回到起点”的固定顺序。
  - DOM 更新是否发生在 Click、MouseUp 或键盘事件回调内部。
  - 回调中的 `endUpdate()` 是否可能早于宿主事件状态更新而刷新布局。
  - 回归测试是否覆盖回调尚未返回时的重绘请求。
