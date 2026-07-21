# 测试与调试经验

## WASAPI 暂停后音频线程意外退出

- 现象：带音轨视频首次播放或重新缓冲后卡住，恢复时进入 `Failed`，错误显示音频输出无法启动；无音轨视频不受影响。
- 影响范围：事件驱动共享模式 WASAPI 输出在 `Stop` 后继续复用同一工作线程，以支持暂停、重新缓冲和恢复的路径。
- 根因：`IAudioClient::Stop` 后可能残留一次渲染事件信号。等待循环只在 `running == true` 时接受渲染事件，把暂停状态下合法的 `WAIT_OBJECT_0 + 1` 落入失败分支并退出线程；后续 `Start` 命令因此无人处理。
- 排查路径：先用真实带声素材确认视频解码正常、失败只发生在音频恢复；再分别测量裸 WASAPI 的设备消费帧数和 FFmpeg 播放器媒体时钟，排除采样率及时间戳换算；最后在等待分支记录返回值和 `running`，确认是暂停态渲染事件而非无效句柄。
- 最终方案：渲染事件无论运行状态都属于合法唤醒；只有运行中才向设备写帧，暂停时直接忽略，工作线程继续等待控制命令。未知等待结果仍记录返回值和 `GetLastError()`。
- 验证方式：48 kHz 双声道输出 1 秒消费约 48,000 帧；真实 12.03 秒带声视频逐秒推进媒体时间，暂停后可恢复；10 FPS 与显示器上限帧率下音频速度不变且不再进入 `Failed`。
- 可复用经验：事件驱动设备的事件可能跨越 `Start` / `Stop` 状态边界。等待结果是否合法应由句柄身份判断，当前运行状态只决定是否执行 I/O，不能决定事件本身是不是错误。
- 快速检查清单：
  - 等待分支是否把“合法句柄 + 当前不运行”误判为 `WAIT_FAILED`。
  - 暂停是否保留工作线程、控制事件和设备事件的生命周期。
  - 失败日志是否同时包含等待返回值与 `GetLastError()`。
  - 回归是否覆盖播放、暂停、等待一个设备周期后恢复。

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

## 可交互叠层在没有滚动位移时透传滚轮

- 现象：按钮和滚动条能够阻断鼠标点击，但滚轮停在同一面板背景上时会继续操作下层 3D 场景；滚动容器到达边界后也可能重新透传。
- 影响范围：宿主根据 `Runtime::handleEvent` 返回值决定是否把 `MouseWheel` 继续交给游戏、编辑器或其他下层输入系统的集成。
- 根因：滚轮分支只把 `scrollNearest` 是否改变滚动偏移写入 `consumed`，没有复用鼠标按下分支的交互目标判定，也没有把滚动条命中本身算作消费。
- 最终方案：滚轮在滚动偏移发生变化、命中滚动条或命中 `data-action`、输入控件、`selectable` 等可交互目标时返回 `true`。普通非交互元素仍只在实际推动可滚动祖先时消费滚轮。
- 验证方式：`SkuiInteractionTests` 分别覆盖透明空白、普通装饰节点、`data-action`、运行时关闭和恢复 `pointer-events`，并断言未发生滚动位移时的返回值。
- 可复用经验：事件消费不能只等同于视觉或布局状态是否变化。叠层 UI 的交互边界本身就是宿主输入路由契约，尤其要覆盖“无可滚动内容”和“到达边界”这两类零位移情况。

## 样式重算让进行中的 transition 丢失当前采样值

- 现象：`opacity` transition 已经渐显一段时间后，界面突然跳到另一个透明度，再次从较低透明度渐显；逐帧观察像同一个动画被启动了两次。
- 影响范围：transition 运行期间发生 class、inline style、文本、输入消费状态或其他会触发布局重算的 DOM 更新；业务层先移除再恢复同一个 class 时更容易复现。
- 根因：样式重算前虽然保存了 transition 的目标值和当前渲染值，也保留了 `ActiveAnimation`，但重算会清空节点的 `animatedStyle`。目标值未变化时，transition 启动逻辑直接返回，没有把快照中的当前采样值重新写回，导致重算帧临时使用最终样式；下一 Tick 又继续按原动画时间线采样。
- 最终方案：目标值未变化且对应 transition 仍在运行时，把快照中的当前 `height`、`opacity` 或 `transform` 写回 animated style。业务状态刷新同时使用 `beginUpdate()` / `endUpdate()` 包住“移除再添加”等中间操作，只让运行时看到最终 DOM 状态。
- 验证方式：让图片从透明渐显到 50%，在批量更新中依次添加和移除隐藏 class；`endUpdate()` 后像素必须仍接近 50% alpha，再推进剩余一半时长后必须完全显示。普通构建和 UE57 专用 v143 构建都运行完整 CTest。
- 可复用经验：判断 transition 是否“未重启”不能只检查动画对象和目标值；样式重算还必须恢复这一帧实际参与渲染的采样值。业务回流、网络状态同步和幂等刷新也应当作为一次 DOM 事务提交。
- 快速检查清单：
  - 异常跳变是否紧跟 class、style、文本或输入状态更新。
  - 样式重算是否清除了 animated style，却没有重新采样仍在运行的 transition。
  - 幂等刷新是否逐条提交了“移除 class -> 添加同一 class”的中间状态。
  - 回归是否在动画中点触发无最终状态变化的批量刷新，而不只测试静态起点和终点。

## UE 中半透明图片颜色明显变暗

- 现象：同一张带 sRGB 配置和半透明通道的 PNG，在 UE 原生 UI 中呈亮青色，在 SkiaUI 中却偏灰、偏暗；不透明图片差异相对不明显。
- 根因：图片按 sRGB 数值解码后，在没有颜色空间信息的情况下先进行预乘；输出纹理又被 UE 标记为 sRGB，采样时再次执行 sRGB 到线性的转换，等价于在错误的空间中预乘透明度。
- 处理方式：位图解码和缓存图片显式标记 `SkColorSpace::MakeSRGB()`。若目标是匹配浏览器和 UE Slate 的 UI 观感，UE GPU surface 使用 `SkColorSpace::MakeSRGB()`，承载 surface 的 RHI 纹理不设置 `TexCreate_SRGB`，Slate Viewport 同时设置 `ESlateDrawEffect::PreMultipliedAlpha | ESlateDrawEffect::NoGamma`。这样既不会重复乘透明度，也不会把已经编码为 sRGB 的预乘像素再次做 Gamma 转换。
- 回归验证：把 sRGB 像素 `(166, 255, 253, 128)` 绘制到黑色背景时，线性 surface 的结果应接近 `(48, 128, 126, 255)`，sRGB UI surface 的结果应接近浏览器合成值 `(83, 128, 127, 255)`。两种输出必须分别测试，不能用线性结果推断 Slate UI 的最终观感。
- 排查清单：
  - PNG 是否携带 sRGB、gAMA 或 ICC 信息，解码目标是否保留明确的颜色空间。
  - Skia surface 的颜色空间是否和宿主纹理的采样及 Gamma 转换语义一致。
  - 透明像素是否在与最终混合一致的线性空间中预乘。
  - RHI 纹理的 `TexCreate_SRGB` 是否造成额外一次解码。
  - Slate 的混合状态是否与 Skia Surface 的 `kPremul_SkAlphaType` 约定一致。

## 显式 width:auto 让绝对定位文本背景丢失固有宽度

- 现象：绝对定位文本省略 `width` 时背景能包住文字和左右 padding，明确写成 `width:auto` 后背景框却只剩 padding，文字从右侧溢出。
- 影响范围：没有子节点、依靠自身文本测量尺寸，并显式设置 `width:auto` 或 `height:auto` 的文本叶节点。
- 根因：样式层用 `std::optional<Length>` 同时表达“未声明”和“已声明 auto”。布局层原先以 `!style.width && !style.height` 判断是否安装 Yoga 文本测量函数，导致显式 `auto` 被误判成固定尺寸并跳过固有尺寸测量。
- 最终方案：统一用 `needsIntrinsicMeasure()` 判断尺寸未声明或单位为 `LengthUnit::Auto`；宽高任一方向需要自动计算时都保留文本测量函数，固定的另一方向继续交给 Yoga 约束。
- 验证方式：`SkuiInteractionTests` 使用绝对定位、显式 `width:auto`、左右 padding 的文本标签，分别检查文字右侧和底部仍属于背景框；修改前该用例稳定失败，修改后通过全部 CTest。
- 可复用经验：使用可选值表达 CSS 时，“属性不存在”和“属性值为 auto”在级联层不同，但在固有尺寸计算中经常属于同一分支。布局判断应检查值的语义，不能只检查容器是否有值。
- 快速检查清单：
  - 显式 `auto` 是否被误当作固定尺寸。
  - 测量函数的启用条件是否分别考虑宽度和高度。
  - Yoga 返回的内容尺寸是否仍由 padding、border 和 box-sizing 正确扩展。
  - 回归是否同时覆盖省略尺寸与显式 `auto`。
