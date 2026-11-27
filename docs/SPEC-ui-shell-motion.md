# UI 外壳与动效重构规格（企业微信式）

> 版本：v1.0.9（2026-11-14 按当前代码校准）
> 状态：三段式信息架构、导航/日志/测试切换、导航胶囊、650 ms 电量环和 180 ms DPI
> 滑块补间已实现。第 4 节记录实际曲线、定时器数量与真实动画场景；第 4.5 节集中列出
> 尚未实现的能力，避免把设计目标当成已交付行为。
> 目标：把当前“标题栏 + 左列表 + 右卡片”的展示型界面，重构为高效的信息架构：
> 左侧导航、对象列表、内容区三段式；动效统一为 Google Material 风格。
> 本规格同时约束设备页、日志页、鼠标测试页。

## 1. 信息架构

```text
┌ 原生标题栏 ──────────────────────────────────────────────────────────┐
│ ┌ 导航 76 ┐ ┌ 列表 300 ─┐ ┌ 内容 flex ────────────────────────────┐ │
│ │ 图标+文字│ │ 设备 / 筛选 │ │ 内容头 48                              │ │
│ │ 设备     │ │ 列表行 56   │ │ 标题 + 副标题          主/次操作按钮   │ │
│ │ 日志     │ │             │ ├────────────────────────────────────────┤ │
│ │ 测试     │ │             │ │ 卡片网格 / 曲线 / 轨迹                  │ │
│ │          │ │             │ │                                        │ │
│ │ 版本     │ │             │ │                                        │ │
│ └──────────┘ └─────────────┘ └────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

- 最小窗口 1000×740（96 DPI，外框尺寸）；`WM_GETMINMAXINFO` 会再按当前显示器工作区夹取，
  因此 1080p@150% 等可用工作区小于逻辑尺寸时，窗口仍能缩到屏幕内。
- 初始窗口 1000×780（当前实现），按目标（主）显示器 DPI 计算并夹到工作区；
  设计稿曾写 1180×780，1180 宽尚未启用。
- 导航栏固定 76 px；设备列表固定 300 px；内容区自适应。
- 日志页、测试页不显示设备列表栏（导航 + 内容占满），避免重复列表。
- 设备页三段式：导航 → 设备列表 → 设备详情。
- 设备页当前不滚动：曾经的滚动路径因卡片区高度始终等于视口高度而使
  `DevMaxScroll()` 恒为 0；2026-11 已删除该函数、竖向滚动条和滚轮/拖拽分支，
  不要按旧规格恢复。
- 原生标题栏保留系统按钮；不再绘制“2.4G 设备中心 / 电量·DPI·轮询率”大标题。

## 2. 主题 token

| Token | 值 | 用途 |
| --- | --- | --- |
| `kBg` | `#F5F6F7` | 内容区背景 |
| `kRail` | `#F7F8FA` | 导航栏 |
| `kPanel` | `#FFFFFF` | 列表/卡片 |
| `kAccent` | `#2B7DF6` | 主色（按钮、选中、曲线） |
| `kAccentSoft` | `#EAF2FF` | 选中背景 |
| `kText` | `#1F2329` | 主文字 |
| `kSub` | `#646A73` | 次文字 |
| `kMuted` | `#8F959E` | 弱文字 |
| `kDivider` | `#E5E6EB` | 分隔线/卡片边框 |
| `kGreen` | `#00B42A` | 在线/充电/正常 |
| `kAmber` | `#FF7D00` | 警告/跳点 |
| `kRed` | `#F53F3F` | 错误 |
| `kHover` | `#F2F3F5` | 悬停 |

> 当前 `src/ui_theme.h` 实际提供 `kBg / kCard / kBorder / kText / kMuted / kSub / kHover /
> kAccent / kAccentSoft / kGreen / kAmber / kRed / kTrack`。上表的 `kRail / kPanel /
> kDivider` 是设计别名，代码中尚未落成独立 token：导航栏底色为内联 `RGB(247,248,250)`，
> 列表/卡片与分隔线分别复用 `kCard` 与 `kBorder`。

- 圆角：卡片 8–12 px（视卡片层级），按钮 6 px，列表选中 6 px，徽章 4 px。
- 间距：4/8/12/16/20/24；内容边距 20（头部文字、分隔线、卡片网格共用同一条 20 px 轴线）。
- 字体：Microsoft YaHei UI；标题 16 semibold，区块 11 medium，正文 10，
  辅助 9。数字使用 Segoe UI（保持现有）。
- **毛玻璃材质**（2026-11 起）：窗口底为垂直渐变 `kBgTop #F5F8FD → kBgBottom #E2EAF6`
  加两团大半径柔光（右上冷蓝、左下暖绿，alpha 12–30）。卡片/导航/页脚改为
  半透明白（alpha 168–236）+ 1 px 深色细边（`#0F172A` @ 12%）+ 3 层下偏移投影。
  投影与背景缓存避免逐帧重建；电量卡的充电脉冲改为局部重绘（见 4.1）。
- 卡片内边距 16；卡片间距 12。

## 3. 组件

### 3.1 导航栏

- 宽 76；顶部应用图标 36×36（圆角 8，蓝色底 + 白色电池图形）。
- 导航项 64×52：图标 20 + 文字 11；选中项背景 `kAccentSoft`、文字 `kAccent`、
  左侧 3 px 蓝色指示条（圆角）；未选中 `kSub`；悬停 `kHover`。
- 项目：设备、日志、测试。
- 底部：版本号 9 px `kMuted`；设置/退出保留在托盘菜单。

### 3.2 设备列表

- 头部 48：标题“设备”11 medium + 数量徽章 + 刷新图标按钮（28×28）。
- 行高 56：
  - 左：设备图形 28（现有 `DrawDeviceGlyph`）。
  - 中：名称 10.5 semibold；副标题 9 `kMuted`（协议 · 连接）。
  - 右：电量环 34（`batteryApplicable`）或 `USB`/`BT` 徽章（28×20）。
  - 选中：`kAccentSoft` 背景、名称 `kAccent`；悬停 `kHover`。
- 行间 2 px 间距；列表左右内边距 8。
- 超过可视高度时使用 ListBox 原生竖向滚动条（Win11 为细条）；6 px 自绘胶囊条
  当前只用于日志曲线的横向滚动。

### 3.3 内容头

- 高 48；左边距 20。
- 标题 16 semibold；副标题 9 `kMuted`（VID/PID · 协议）。
- 右侧操作：主按钮（应用设置，`kAccent` 实心，白字）+ 次按钮（刷新/导出等，
  白底 1 px 边框）。
- 日志页：标题“日志与电量曲线”，右侧设备下拉、范围下拉、导出、打开文件夹。
- 测试页：标题按模式显示“鼠标测试 / 键盘测试”；工具行左侧“鼠标 / 键盘”切换。
  鼠标模式右侧目标设备下拉、重新检测、窗口 5s/10s、重置；
  键盘模式右侧 APM、ANSI 104 徽标、清空。

### 3.4 卡片

- 白底、1 px `kDivider`、圆角 8、内边距 16。
- 标题 11 medium `kSub`；数值 16/24 semibold；辅助 9 `kMuted`。
- 设备页网格（内容宽 ≥ 640）：
  - 第一行：电量卡 + DPI 卡（各 50%，高 190）。
  - 第二行：性能卡 + 电量曲线卡（各 50%，高度自适应剩余空间）。
  - 有线/无电量设备：第一行连接信息卡 + DPI 卡；第二行性能卡 + 设备信息卡（各 50%）。
- 内容宽 < 640 时设计为单列；当前实现始终双列（见 4.5 未实现项），
  最小窗口下卡片仍按两列排布。
- 单列时整页纵向滚动（未实现）；当前设备页不滚动，日志页只保留曲线横向滚动条。

### 3.5 按钮 / 下拉 / 列表

- 主按钮：`kAccent` 底、白字、圆角 6、高 30、内边距 14；悬停亮 8%。
- 次按钮：白底、`kDivider` 边框、`kText`；悬停 `kHover`。
- 下拉：白底、1 px 边框、高 30、圆角 6；保持原生 COMBOBOX 但设置字体/高度。
- 活动日志列表：行高 40，表头 32，`kDivider` 横线；保留 ListView，
  原生滚动条在 Win11 已是细条；后续可替换为自绘。

### 3.6 键盘导航（2026-11-21 实现）

**焦点模型**：

- 主窗口自身代表导航栏：`GetFocus()==hwnd` 时，当前 `focusNav_`（设备/日志/测试）
  显示焦点框。
- `Tab` / `Shift+Tab` 在主窗口消息循环的 `PreTranslateMessage` 中统一拦截，按
  “导航栏 → 当前页面可见原生控件（阅读顺序）→ 嵌套子控件（测试目标下拉、音频
  面板列表/下拉）→ 导航栏”循环；不依赖 `IsDialogMessage`，也不修改原生控件。
- 导航栏：`↑/←` 上一项、`↓/→` 下一项、`Home/End` 首尾；焦点移动只移动焦点框，
  `Enter/Space` 才切页，避免连续触发 340–420 ms 页面动画。
- 页面快捷键：`Ctrl+1/2/3` 直达设备/日志/测试；`Ctrl+Tab` / `Ctrl+Shift+Tab`
  循环页面；`F5` 刷新（日志页同时重载日志）；音频面板展开时 `Esc` 返回设备页。
- 原生控件保留自身行为：设备列表方向键选择设备（`LBN_SELCHANGE` 继续走
  `OnSelectIndex`）、下拉 `Alt+↓`/方向键、按钮 `Space`、编辑框输入、DPI 滑块
  方向键/Home/End。
- 焦点可视化：
  - 导航项：系统虚线焦点框（内缩 2 px）；
  - 自绘按钮/档位按钮/设备列表选中行：`DrawFocusRect` 内缩虚线；
  - DPI 滑块：轨道外 1 px 虚线焦点框；
  - 原生列表/下拉/编辑/复选框继续使用系统焦点视觉。
- 测试页：目标设备下拉可通过 `Tab` 到达；自绘测试控件（鼠标/键盘切换、清空、
  窗口秒数、日志/诊断标签、重绘/重置）本轮仍保持鼠标操作——键盘测试页的
  `Tab` 必须继续被 Raw Input 记录，不能改成会抢占按键的全局快捷键。
- 音频面板：面板内原生列表/下拉通过递归枚举子控件进入 Tab 链；自绘按钮仍为
  鼠标操作。
- 不新增可见控件、不改变布局；焦点环只影响绘制，不触发重新布局或设备扫描。

**状态所有权与事件顺序**：

```text
主消息循环
  GetMessage → MainWindow::PreTranslateMessage
    ├─ Tab / Shift+Tab → 构建当前可见焦点链 → SetFocus(下一个) 或 SetFocus(hwnd)
    ├─ Ctrl+Tab        → SetView(下一个页面) + 焦点回导航栏
    ├─ Ctrl+1/2/3      → SetView(对应页面) + 焦点回导航栏
    └─ F5              → RequestRefresh / ReloadLogs
  → TranslateMessage → DispatchMessage

hwnd 获得焦点（导航）
  WM_KEYDOWN ↑↓←→/Home/End → focusNav_ 移动 → 只重绘导航栏
  Enter/Space               → SetView(focusNav_) → 原页面切换流程
```

- `focusNav_` 只属于 MainWindow UI 线程；不写入 `Snapshot`，不参与设备轮询。
- `BuildFocusChain()` 只收集 `IsWindowVisible && IsWindowEnabled && WS_TABSTOP`
  的子控件；按当前页面/音频展开状态排序，再追加递归枚举到的嵌套原生控件，
  隐藏/禁用控件不会被 Tab 命中。
- 焦点变化只 `InvalidateRect(&rcRail_)`（必要时由控件自身重绘）；不调用
  `Layout()`，不启动动画定时器。

**验收**：

1. 启动后 `↓`/`Enter` 可切到日志/测试；`Ctrl+1/2/3` 直达；`Ctrl+Tab` 循环。
2. `Tab` 从导航进入设备列表，方向键切换设备且内容区同步；`Shift+Tab` 反向回绕。
3. 设备页 Tab 依次到达设备列表、应用设置、刷新、DPI 输入、滑块、档位、轮询、
   去抖、角度；日志页到达设备/天数/导出/文件夹/列表；测试页到达目标下拉。
4. 焦点框在导航项、自绘按钮/档位、设备列表选中行、DPI 滑块可见；布局不位移。
5. `F5` 刷新；音频面板 `Esc` 返回；Tab 循环不卡死，不把焦点交给隐藏控件。

## 4. 动效系统

### 4.1 底层（已实现）

`src/ui_anim.h/.cpp`（纯 Win32，无依赖）：

```cpp
// Material 3 令牌 + 欠阻尼弹簧；弹簧的输出可以 > 1（过冲）。
enum class Easing {
    Linear, Standard, StandardDecelerate, StandardAccelerate,
    Emphasized, EmphasizedDecelerate, EmphasizedAccelerate,
    Decelerate /*= EmphasizedDecelerate*/, Accelerate /*= EmphasizedAccelerate*/,
    Spring /*ζ=0.62, ≈8.3% 过冲*/, SpringSoft /*ζ=0.78, ≈2.1% 过冲*/,
};

float Ease(Easing e, float t);   // t 夹到 0..1；Spring 返回值可 > 1
double NowMs();                  // QueryPerformanceCounter → 毫秒（double）
bool SystemAnimationsEnabled();  // SPI_GETCLIENTAREAANIMATION

class Animator {
    AnimId Add(float from, float to, int durationMs, Easing, apply, delayMs = 0);
    AnimId AddAt(double nowMs, ...);
    bool Tick(double nowMs);
};
```

- **时间源必须用 QPC**：`GetTickCount64` 的分辨率约 15.6 ms，120 Hz（8 ms）
  下每一帧都会被量化成整块跳动。
- **定时器跟随刷新率**：`monitorRefreshHz` 优先取 DWM 的 `rateCompose`
  （`DwmGetCompositionTimingInfo`），失败退回 `EnumDisplaySettings`，最终 60。
  间隔 = `clamp(round(1000/hz), 4, 16)`：60 Hz→16 ms，120 Hz→**8 ms**，
  144 Hz→7 ms。动画期间成对调用 `timeBeginPeriod(1)` / `timeEndPeriod(1)`，
  否则 `WM_TIMER` 仍会被系统粒度压到 ~15.6 ms。`WM_DISPLAYCHANGE` /
  `WM_DPICHANGED` 会重算并重启定时器。
- 全应用仍只有一个**动画**定时器（id=2）；id=1（1 s toast 清理）与
  id=3（50 ms 测试数据）不是动画定时器。
- 动画按时间推进，不按帧数；丢帧不改变时长。
- `SystemAnimationsEnabled()==false`：`Animator::Add` 立即 apply 终值，
  电量环/滑块/入场动画直接到位，充电脉冲不跑。
- **绘制缓冲**：主窗口持久化 `paintMem_/paintBmp_`，不再每帧
  `CreateCompatibleBitmap`；背景渐变缓存为位图（`PaintBackdrop`）。
- **局部重绘（`ps.rcPaint`，2026-11-24 实现 B05）**：`WM_PAINT` 不再无条件整窗重画，
  `Paint(dc, ps.rcPaint)` 先由 `paintregion::Decide`（`src/paint_region.h/.cpp`，纯几何）
  判定策略：
  - **整帧**：脏区为空、覆盖整个客户区、绘制缓冲刚重建（`paintFresh_`）、
    内容入场（`introActive_`）或音频面板滑动（`audioSliding_`）。
  - **增量**：其余情况只重画脏区，步骤固定为
    ① `PaintBackdropRegion(mem, dirty)` 用背景缓存恢复脏区底色 →
    ② 按 Z 序（导航栏 → 设备列表 → 内容 → 页脚）重画**与脏区相交**的绘制块，
    GDI 与 GDI+ 全部裁剪到脏区（GDI+ 上下文在 `BeginFrame` 前设裁剪）→
    ③ 只 `BitBlt` 脏区到窗口。窗口其余像素本来就是上一帧的正确结果。
  - 绘制块判定外扩 `Scale(6)`：玻璃卡片阴影 3 px、焦点框 2 px 会画到矩形外，
    不外扩会留下残影。脏区为单条边（如页脚 1 px 状态变化）也不会漏块。
  - 背景缓存不可用（尺寸不匹配/尚未生成）时**退回整帧**，绝不把内容画在
    不完整的背景上。音频快照只在滑动期间存在，因此只在整帧路径绘制。
  - 典型收益：导航悬停/胶囊动画只使能 `rcRail_`，页脚 toast 只使能 `rcFooter_`，
    信息卡/DPI 卡只使能各自矩形——内容区 8 张玻璃卡片不再随之重画。
- **电量卡直达路径**：充电脉冲/电量补间仍走 `RepaintBatteryCard()`——
  从背景缓存恢复该卡区域 → 只重画这一张卡 → 只 BitBlt 该区域，不经过 `WM_PAINT`。
  只有电量环补间或充电脉冲**确实改变画面**时才重绘（`ringChanged || charging`）；
  以前只要有任何动画在跑就无条件 `InvalidateRect(rcBattery_)`，导航悬停时会把
  电量卡并进脏区，使 76 px 宽的导航栏脏区 bbox 膨胀成 674×741，增量重绘失去意义。
- **统计**：`paintFull_ / paintDirty_ / paintDirtyPx_`；窗口销毁时写一行
  `Paint: full=N dirty=M dirtyPx=X clientPx=Y`（`LogLine`，info 级），
  用于核对增量比例。

### 4.2 曲线与时长（当前实际使用）

| Easing | cubic-bezier / 参数 | 当前用途 |
| --- | --- | --- |
| Standard | `(0.2, 0, 0, 1)` | 导航悬停 120 ms |
| EmphasizedDecelerate | `(0.05, 0.7, 0.1, 1)` | 页面切换内容入场 420 ms；音频面板进场 360 ms |
| EmphasizedAccelerate | `(0.3, 0, 0.8, 0.15)` | 音频面板退场 280 ms |
| Spring | ζ=0.62，≈8.3% 过冲，峰值 t≈0.35 | 导航胶囊 340 ms；DPI 滑块 300 ms |
| SpringSoft | ζ=0.78，≈2.1% 过冲 | 电量环 650 ms；toast 进场 260 ms |
| StandardDecelerate / StandardAccelerate / Emphasized / Linear | 见 M3 | 已定义，供后续使用 |

- 弹簧的过冲只作用在几何量上：电量环把过冲夹在 0–100，数字保持单调
  （不会出现 62→63→62）；滑块位置由 `SliderFractionValue` 夹在轨道内。
- 系统关闭动画时全部直接跳终值。

### 4.3 实际动画场景

1. **页面切换（设备/日志/测试）** —— 两步同时进行：
   - 导航胶囊：340 ms `Spring` 平移（约 8% 过冲）+ 同长度的 0→1→0 拉伸包络
     （±3 px，果冻感）；系统关闭动画时直接到位。
   - 内容入场（Apple Store 式）：420 ms `EmphasizedDecelerate`，
     透明度 0→255、整体上浮 18 px。内容先画到离屏缓冲（`introMem_`，
     底部先拷入真实背景以保证半透明卡片合成正确），再以恒定 alpha
     `AlphaBlend` 回主缓冲，并用内容矩形裁剪。
     原生子控件（列表、下拉、按钮、滑块、ListView 等）在入场期间隐藏，
     进度到 **92%** 时结束并恢复可见性——此时内容已基本不透明，不会“弹”出来。
2. **导航悬停**：120 ms `Standard` 状态层渐变（`navHover_[3]`），
   指针离开窗口由 `WM_MOUSELEAVE` 清零。`SetNavHover` 每次都**先取消在飞轨道**
   再判断“是否已到位”：轨道刚 Add、尚未 Tick 时 `navHover_` 仍是旧值 0，
   如果按旧值提前 continue，快速划过导航项会留下一条永不反向的轨道，
   表现为鼠标已离开、该导航项仍高亮（2026-11-24 修复）。
3. **电量环**：650 ms `SpringSoft`，切换设备从 0 补间、同设备从当前值补间；
   数字单调。充电中额外跑 1400 ms 正弦脉冲（QPC 相位），走局部重绘。
4. **DPI 滑块**：程序化赋值 300 ms `Spring`，由共享 Animator 驱动；
   拖动/滚轮/键盘立即跟手并取消补间；丢失 capture 立即结束拖动。
5. **Toast**：260 ms `SpringSoft` 进场，文字上滑 3 px + 颜色从页脚底色渐入；
   消失仍是瞬时（到期清理）。
6. **音频面板入场/退场**（2026-11-24）：
   - 展开：`audioExpanded_` 立即置 true，列表隐藏、内容头切到“音频控制”；
     面板先 `PrintWindow` 截成快照并隐藏真实子窗口，主窗口把快照从右侧外
     （`client.right`）滑入内容区
     `{rcContent_.left, rcContentHeader_.bottom, rcContent_.right, rcContent_.bottom}`，
     360 ms `EmphasizedDecelerate`，同时 alpha 0→255。
   - 收起：先截快照、隐藏真实面板，再恢复设备页布局并启动 420 ms 内容入场
     （原生控件隐藏、92% 时恢复）；快照 280 ms `EmphasizedAccelerate` 滑出到
     `client.right`，alpha 255→0。两者交叉过渡，避免原生控件在半透明快照上方穿透。
   - 采用“快照滑动”而不是移动真实子窗口：原生 ListBox/ComboBox 随窗口移动时
     会和父窗口重绘抢帧，产生鬼影；快照画在主窗口缓冲里，合成顺序稳定。
   - 反向操作：滑动中再次展开/收起会取消旧轨道，从当前进度反向，不跳帧。
   - 滑动期间 `Layout()` 不重设面板 bounds，避免和动画抢同一个位置；窗口尺寸/
     DPI 变化在滑动结束后由常规 `Layout()` 收敛。
   - 系统关闭动画时直接到位（展开立即显示、收起立即隐藏）。

### 4.4 状态所有权与事件顺序

```text
UI 线程
  WndProc / SliderProc (WM_APP_SNAPSHOT / WM_COMMAND / mouse / slider value)
    → 更新快照/选中/视图状态
    → Animator.Add(...)（导航胶囊、悬停、滑块、toast、内容入场、音频面板滑动）
    → InvalidateRect(FALSE)
  WM_TIMER(id=2, 自适应 4–16 ms；120 Hz 屏 = 8 ms)
    → Animator.Tick(NowMs())              // 全部补间
    → 音频面板滑动：SetPanelBounds(x) + InvalidateRect(内容区)
    → 电量环手动推进（Ease(SpringSoft, t)，650 ms）
    → 只有电量卡在动时 RepaintBatteryCard()（直达 BitBlt，不经 WM_PAINT）
      否则 InvalidateRect(受影响的绘制块)
  WM_PAINT(ps.rcPaint)
    → Paint(dc, ps.rcPaint)
    → paintregion::Decide：整帧（空/满/缓冲新建/入场/滑动）或脏区
    → 增量：恢复脏区背景 → 按 Z 序重画相交绘制块（裁剪到脏区）→ BitBlt 脏区
    → 没有轨道/环/脉冲/入场时 StopAnimTimer()
```

- 动画状态只属于 UI 线程；Manager/快照不感知动画。
- 视图切换在同一个消息处理内完成：`SetView` 先 Add 导航胶囊轨道，再切换数据/可见性并
  `Layout()`；胶囊回调只改 `navIndicatorY_`，不会在切换前单独绘制旧内容。
- `Animator` 没有属性 key；需要覆盖同一属性时由调用方先 `Cancel(previousId)`。
  当前导航胶囊和 DPI 滑块都按这个模式实现。
- DPI 滑块与 `MainWindow` 共享 `Animator`：`SliderState` 只持有 `Animator*` 和
  `AnimId`；滑块销毁时在 `WM_NCDESTROY` 取消轨道，避免回调悬空。

### 4.5 尚未实现（not implemented yet）

- 内容**退场**过渡（旧内容淡出）与**卡片错峰**（30 ms/张，最多 5 张）；
  当前只有新内容统一淡入 + 上浮。
- Toast **退场**动画（目前只有进场；到期瞬时清除）。
- 列表行的 120 ms 悬停/按压过渡（导航项已有，列表行仍是选中瞬时换色）。
- 增量重绘按“绘制块”粒度（导航栏/列表/内容/页脚）而不是按单个卡片；
  卡片内阴影重叠区域仍是整块重画，进一步细化需要每块单独登记脏矩形。
- 日志列表按行 diff；当前仍整表重建，但会按首行/选中行内容恢复滚动位置与选中
  状态（2026-11-17），不再每 10s 跳回顶部。首行恢复包含 `newTop==0` 的情况；
  用户正在展开日志筛选下拉框时跳过本次重建，避免下拉被强制关闭。
- 动画定时器只在电量卡可见（设备页、未展开音频面板、窗口未最小化）时运行；
  充电脉冲不能在日志/测试/音频页常驻。隐藏到托盘或最小化时停止测试页
  Raw Input/50 ms 定时器、音频面板 2 s/33 ms 刷新与麦克风采集；恢复后重新进入
  测试视图并重读一次音频状态。
- 键盘导航已实现（见 §3.6）；自绘测试页按钮仍为鼠标操作。
- 设备页单列退化与纵向滚动；当前固定双列，设备页不滚动。
- 默认初始宽度 1180（当前代码 1000）；是否按设计稿调整需另行确认。

## 5. 迁移与删除

- 删除标题栏大标题/副标题、`chkStartup_` 顶部复选框（托盘菜单已有开机自启）。
- “应用设置”移入设备内容头右侧；“日志/测试”移入导航栏；“刷新”移入列表头和
  日志内容头。
- 旧 `btnView_` 的“日志/返回”语义由导航替代。
- 保留托盘菜单：打开主界面、立即刷新、开机自启、通知设置、测试通知、
  日志与电量曲线、鼠标 / 键盘测试、打开日志、退出。
- 视图切换统一入口 `SetView(View)`；`View { Device, Log, Test }`。
  `SetLogView/SetTestView` 仅作为内部实现，导航、托盘菜单和设备页迷你曲线不得直接调用。
- 设备页、日志页、测试页共享内容头/卡片/按钮绘制函数，禁止复制粘贴。

## 6. 验收场景

1. 启动：导航 + 设备列表 + 设备详情三段式；无旧大标题；初始窗口 1000×780
   （按目标显示器 DPI 缩放并夹到工作区）无横向滚动。
2. 点击导航“日志”：列表栏隐藏，内容切换为日志页；返回“设备”恢复列表。
3. 点击导航“测试”：进入鼠标测试页；设备列表栏隐藏。
4. 选中不同设备：列表选中态与详情同步；电量环动画 650 ms。
5. 动画开关关闭（系统“显示动画”关闭）：导航胶囊、电量环、DPI 滑块立即到终值，
   16 ms 动画定时器不会常驻；toast/测试数据定时器独立存在。
6. 连续快速切换视图：无闪烁、无崩溃、无叠加定时器。
7. 有线设备：详情无电量卡/曲线，连接信息卡 + DPI/性能卡重排。
8. 日志页 1–7 天与细胶囊滚动条保持原有行为。
9. 测试页 8 kHz 输入时动画不影响数据刷新（50 ms 数据定时器独立）。
10. 最小窗口：工作区允许时为 1000×740；设备页当前双列、不滚动；日志/测试页无控件重叠。
    单列布局与设备页纵向滚动列在 4.5 未实现项。
11. 增量重绘（B05）：鼠标扫过导航栏时只有左侧导航栏区域刷新，内容区卡片不重画；
    页脚 toast 只刷页脚；信息卡/DPI 卡数值变化只刷该卡所在区域；
    卡片阴影、焦点框、圆角边缘无残影/接缝。切换页面、拖动窗口、改 DPI、
    音频面板滑动等复合场景仍整帧重绘，画面与整帧路径一致。
    导航栏悬停离开后不得残留高亮；一次“悬停→离开”往返后的整窗截图与静止态
    逐像素一致（`k26_paint_idle.png` / `k27_paint_after_hover.png`）。
