# 2.4G Device Center · 项目压缩上下文

> 版本：v1.0.5 · 更新：2026-10-31
>
> 用途：让接手的开发者或 Agent 在不通读全部源码的情况下恢复上下文。
> 事实以当前检出源码为准；协议细节以 [PROTOCOLS.md](../PROTOCOLS.md) 为准。

## 1. 项目定位

一个原生 Windows 托盘小工具，用系统 HID 直接读取 2.4G 无线外设的电量、充电状态、电压、
DPI、轮询率、按键去抖和角度捕捉，并在已验证的协议上写入设置。

- C++20 + Win32 API + GDI/GDI+，静态链接，单文件 exe。
- 不使用 Electron、WebView、.NET、Python 或 Qt 运行时。
- 不做宏、灯效、固件写入；后台轮询只读，写入仅由用户点击「应用设置」触发。
- 目标系统：64 位 Windows 10/11（x86-64）。

## 2. 架构结论：Win32 API 不等于 32 位程序

这是本项目最容易误解的一点：

| 概念 | 结论 |
| --- | --- |
| `Win32` | Windows API 家族名，从 16 位时代沿用至今，**不是** 32 位目标 |
| 实际编译目标 | `x86_64-w64-mingw32`，PE32+ |
| PE Machine | `0x8664` |
| 指针/句柄 | 8 字节（`sizeof(void*) == 8`） |
| 优化 | `-O2` + `-ffunction-sections -fdata-sections -Wl,--gc-sections` |
| 运行库 | 静态链接，无 MinGW/VC++ 运行库 DLL 依赖 |

因此**不需要从 Win32 迁移到 Win64**：项目从一开始就是 Win64 进程，只是使用 Win32 API。
为防止后续误编译成 32 位，代码和构建脚本各有一道硬约束：

- `src/common.h`：
  - `#if !defined(_WIN64) #error ...`
  - `static_assert(sizeof(void*) == 8, ...)`
- `build.ps1`：链接后读取 PE 头，Machine 必须是 `0x8664`，否则构建失败并打印
  `Architecture: x86-64 / PE32+ (Win64)`。
- `WirelessCenter.exe --diag` 输出第一行包含 `Architecture: x86-64 / PE32+ (64-bit process)`。

**不要**手工定义 `_WIN64`；它由编译器根据 `-m64` 设置。若将来评估 ARM64，需要重新检查
`SP_DEVICE_INTERFACE_DETAIL_DATA_W`、`HIDP_CAPS`、窗口消息参数等指针宽度敏感结构，
不能只换编译器目标。

## 3. 进程、线程与状态所有权

```text
┌──────────────────────────── UI 线程 ────────────────────────────┐
│ main.cpp: 单实例 Mutex、DPI 感知、窗口/托盘、消息循环            │
│ ui.cpp:   设备列表、圆环电量、滑杆、按钮、确认框、动画定时器     │
│  状态：选中设备、UI 草稿、动画进度（只用于显示）                 │
└───────────────▲───────────────────────────────┬─────────────────┘
                │ WM_APP_SNAPSHOT                │ Apply(ApplyRequest)
                │ WM_APP_APPLY_RESULT            ▼
┌───────────────┴─────────────────────────────────────────────────┐
│ DeviceManager（唯一设备状态所有者，manager.cpp）                 │
│  · 后台线程每 5 s（2–300 s 可配）枚举/轮询                       │
│  · std::mutex 保护 Snapshot，条件变量唤醒刷新                    │
│  · std::deque<ApplyRequest> 串行写入队列，不并发写设备           │
│  · 合并旧读数：设备休眠时标记 stale，保留上次电量                 │
└───────────────┬─────────────────────────────────────────────────┘
                │ proto::Read* / proto::Apply*
                ▼
┌─────────────────────────────────────────────────────────────────┐
│ HID 层（hid.cpp）                                                │
│  · SetupDiGetClassDevs / HidD_GetPreparsedData / HidP_GetCaps    │
│  · CreateFileW + overlapped ReadFile/WriteFile + 超时            │
│  · HidD_GetFeature / HidD_SetFeature                              │
│  · 同步 IO 看门狗：CancelSynchronousIo 防厂商驱动卡死             │
│  · Windows 电池类 IOCTL_BATTERY_QUERY_* 回退                     │
└─────────────────────────────────────────────────────────────────┘
```

关键规则：

- `DeviceManager` 是设备状态的唯一所有者；UI 不直接调用 HID。
- 所有写操作走同一条 `Apply` 队列；UI 只负责收集草稿和确认。
- 后台轮询绝不写闪存；未知/未验证设备默认只读。
- 轮询与写入共用 HID 句柄时串行化，避免报告交错。

## 4. 源码地图

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 入口、CLI（`--once/--diag/--demo/--tray/--help`）、单实例、DPI 感知、消息循环 |
| `src/common.*` | 字符串/路径/INI/日志、x64 编译期守卫 |
| `src/hid.*` | HID 枚举、接口描述、带超时读写、同步看门狗、系统电池 |
| `src/protocols.*` | `Device`/`DpiInfo`/`PollingInfo`/`DebounceInfo`/`AngleInfo`/`ApplyRequest` 数据模型 |
| `src/device_merge.*` | 跨传输设备身份合并、快照保留与去重 |
| `src/battery_notify.*` | 低电/严重低电/充电完成通知状态机（去重与重新武装） |
| `src/proto_logitech.cpp` | Logitech HID++ 2.0 读/写 |
| `src/proto_asus.cpp` | ASUS ROG 私有 HID 读/写 |
| `src/proto_razer.cpp` | Razer 90 字节 feature report 读/写 |
| `src/proto_rapoo.cpp` | Rapoo output/feature/status 三报告读/写 |
| `src/proto_generic.cpp` | Compx（含 Zaopin 写 DPI）、Nordic 54、Aurora、Cherry、Lofree、系统电池 |
| `src/manager.*` | 后台轮询、写入队列、demo 设备 |
| `src/tray.*` | 动态圆环托盘图标（4× 超采样） |
| `src/ui.*` | Win32 主窗口、GDI+ 矢量图标/圆环、动画、确认框 |
| `src/resource.rc` | 图标、版本信息、manifest 引用 |
| `src/app.manifest` | Per-Monitor DPI 感知、Common Controls v6 |
| `build.ps1` / `Makefile` / `build.cmd` | 构建入口 |
| `tools/` | 可选的 w64devkit 自解压包与说明 |

## 5. 读写链路与安全设计

### 5.1 轮询（只读）

`DeviceManager::ScanOnce()` → 各 `proto::Read*()` → 生成 `Device` 列表 → 与上一轮快照合并：

- 设备消失或休眠：`online=false`、`stale=true`，保留上次电量并显示灰环。
- 重新出现：恢复实时读数。
- 系统电池作为通用回退，永远只读。
- 跨传输身份：同一只 Z4D 在 2.4G 接收器（PID F527）和 USB 线（PID F526）之间切换时，
  `Device.identity` 都是 `zaopin:z4d`。`device_merge.*` 先按 identity 去重，优先保留
  在线、有电量、正在充电的条目；两条链路都在线且都在充电时优先 2.4G
  （官方驱动读取的链路）。旧传输的同 identity 条目不作为离线设备保留。
  因此拔掉接收器改用 USB 后只显示一个条目，电量取 USB 充电链路的实时值。
- 电量使用设备上报的原始百分比字节，不做 5% 取整。官方驱动不是持续轮询：本机日志
  在 16:53:07 从 F526 采样到 `5E 01 10 44`（94%、充电、4164 mV）后没有再记录新的
  `BatteryLevel`；同一接口在 16:56 读到 `5F 01 10 61`（95%、充电、4193 mV）。
  官方界面停在 94% 时，本应用已显示实时 95%；1% 差异是采样时间差，不是刻度换算。

### 5.2 应用设置（写入）

```text
UI「应用设置」
  → 校验/组装 ApplyRequest
  → Zaopin 且写 DPI：弹出风险确认框（默认按钮为「否」）
  → DeviceManager::Apply() 入队
  → 后台线程 ProcessApply()
  → proto::ApplyXxx()
  → HID 写入
  → 成功/失败结果 PostMessage(WM_APP_APPLY_RESULT)
  → UI 显示结果并刷新
```

可写矩阵：

| 协议 | DPI | 轮询率 | 去抖 | 角度捕捉 |
| --- | --- | --- | --- | --- |
| Logitech HID++ 2.0 | 读/写 | 读/写 | — | — |
| ASUS ROG | 4 档读/写 | 读/写 | 读/写 | 读/写 |
| Razer | 读/写 | 读/写 | — | — |
| Rapoo | 7 档读/写 | 读/写 | 读/写 | 读/写 |
| Compx（Zaopin Z4D） | 启用档位/当前档读/写 | 只读 | 只读 | — |
| Nordic 54 / Aurora / Cherry / Lofree / 系统电池 | 只读 | 只读 | 只读 | — |

未知设备不会盲写。`DeviceManager` 对非可写协议返回「该协议只读，不支持修改设置」。

### 5.3 Zaopin 写入保护

`ApplyZaopinDpi()` 的顺序是固定的：

1. 只允许写设备已启用的档位（`stage < maxStages`）或当前档。
2. 读取旧记录。
3. **原值回写自检**：把读到的旧记录原样写回并读回，确认写通道真的可用。
4. 写入新记录。
5. 读回逐字节校验。
6. 任一步失败：尝试恢复旧记录，并返回错误。
7. 若同时切换当前档，当前档字节也走同样的读回校验/恢复流程。

没有 AES、没有 commit 命令、没有全局 CRC；ACK 是 17 字节原样回显（也可能被设备批量延迟返回）。

### 5.4 电量通知

`battery_notify.*` 是纯状态机，`ui.cpp` 在每次快照后调用，结果交给
`TrayIcon::Balloon()` → `Shell_NotifyIcon(NIF_INFO)`：

| 事件 | 条件 | 通知 |
| --- | --- | --- |
| 低电 | 未充电，≤20% | 电量偏低（警告） |
| 严重低电 | 未充电，≤10% | 电量严重不足（警告） |
| 充电完成 | 充电中到 100%，或 1→0 且 ≥99% | 充电完成（信息） |

- 按 `Device.identity` 分设备保存状态；低电在充电开始或回到 >25% 时重新武装，
  充满在开始新充电（<99%）或放电到 <90% 时重新武装。
- 首次看到设备只补发低电，不补发“充电完成”，避免启动弹窗。
- Windows `HKCU\...\PushNotifications\ToastEnabled=0` 时系统会静默丢弃通知；
  应用底部显示“系统通知已关闭”，托盘菜单“通知设置”打开 `ms-settings:notifications`。
- 详细规则与验收场景见 [SPEC-notifications.md](SPEC-notifications.md)。

## 6. 协议速查

完整帧、偏移和来源见 [PROTOCOLS.md](../PROTOCOLS.md)。这里只保留接手时最需要的识别信息。

| 协议 | 识别/通道 | 关键命令 | 校验 |
| --- | --- | --- | --- |
| Logitech HID++ 2.0 | usage page `0xFF00`，report `0x10/0x11` | feature `0x0005` 电量、`0x1004` DPI、`0x1000` 轮询率、`0x1001`、`0x2201/0x2202`、`0x8060/0x8061`、`0x8100` | HID++ 帧 |
| ASUS ROG | 64/65 字节私有报告，report ID 0/3 | `0x0712` 电量、`0x1201` DPI、`0x0412` 轮询率、`0x0012`、`0x3151` 去抖、`0x0350` 保存 | 厂商帧 |
| Razer | 90 字节 feature report，report ID 0 | 电量/DPI/轮询率 feature | CRC：字节 2..87 异或 |
| Rapoo | output report 6 / feature report 8 / status report 7 | EEPROM 640/648/662/664/704/705/707/712 | 厂商帧 |
| Compx | usage `0xFF02`，17 字节 report ID 8 | `0x04` 状态/电量、`0x08` 读闪存、`0x07` 写闪存 | `0x55 - sum(bytes[1..15])` |
| Nordic 54 | usage `0xFF05`，64 字节 | cmd `0x72` | 厂商帧 |
| Aurora / mouse.xyz | usage `0xFFFF`，65 字节 feature | cmd `0x83`，旧版 `0x8F` | 厂商帧 |
| Cherry | VID `0x046A` | 厂商 HID | 厂商帧 |
| Lofree | usage `0x388D` WebHID | 厂商 HID | 厂商帧 |
| 系统电池 | `GUID_DEVCLASS_BATTERY` | `IOCTL_BATTERY_QUERY_TAG/INFORMATION/STATUS` | Windows |

## 7. Zaopin Z4D 实机结论（本机已验证）

- 设备：`VID_3554`；2.4G 接收器 `PID_F527`（product `Z4D`），USB 有线/充电
  `PID_F526`（product `2.4G Dual Mode Mouse`）；Compx `0xFF02` 17 字节通道。
- 接口：`MI_01 & col05`。
- 实测读数：2.4G 电量 50%、3883 mV；USB 充电 95%、4147 mV；当前档 `0` → 3100 DPI、
  轮询率 4000 Hz、去抖 8 ms。官方驱动日志另有一次 `54 01 0F C6` → 84%、充电、4038 mV。
- 两条传输通过 `identity=zaopin:z4d` 合并为一个设备；显示名称统一为 `Zaopin Z4D`，
  电量/充电状态/连接方式取当前在线或充电链路；两条都在充电时优先 2.4G 链路。
- 电量显示 `payload[0]` 原始百分比，不做 5% 取整；与官方驱动的 1% 差异来自链路/采样边界。
- `maxDPI = 1`，所以设备只启用 1 个 DPI 档；UI 只显示/允许写这一个档。
- 闪存布局：

| 地址 | 含义 |
| --- | --- |
| `0x00` | 轮询率位掩码 |
| `0x02` | `maxDPI`（档位数量） |
| `0x04` | 当前档位 |
| `0x0C + 4*i` | 第 i 档 DPI 记录：`[x, y, DPIex, checksum]` |
| `0x2C + 4*i` | RGB |
| `0x4C` | DPI 指示灯 |
| `0xA9` | 去抖 |

- DPI 解码：

```text
code = x | (((DPIex >> 6) & 3) << 8)
unit = (DPIex & 0x22) ? 100 : 50
DPI  = (code + 1) * unit
if (DPIex & 0x11) DPI *= 2
```

- 读闪存：`08 08 00 ah al len`，响应可能延迟/批量返回，按地址+长度匹配；
  数据起始偏移：带 report ID 为 `o+6`，去掉 report ID 为 `o+5`。
- 写闪存：`08 07 00 ah al len data... chk`；当前档写 `[stage, 0x55-stage]`。
- 已验证：3100 → 1600 → 3100 写入与恢复；同值自检、读回校验均通过。
- 限制：回报率/去抖/宏/灯效仍只读；增删档位请用官方 Mouse Drive。

## 8. UI 与托盘

- 主电量：GDI+ 大圆环，中心大号百分比；充电时绿色脉冲+闪电/圆点。
- 圆环动画：ease-out 650 ms，33 ms 定时器；空闲时停止定时器。
- DPI 滑杆：对数刻度、180 ms 平滑过渡、16 ms 定时器；预设档可点击。
- 设备列表：约 32 px 小圆环 + 大号数字 + GDI+ 矢量设备图标。
- 托盘：4× 超采样圆环徽章，白色底面、深色大数字、外侧彩色进度弧；
  16 px 下 100% 显示 `F`，悬停提示仍为 `100%`；深浅任务栏都有描边。
- 标题副标题：`电量 · DPI · 轮询率 · 原生 HID`（不要写「无 WebView」之类宣传语）。
- `--demo`：8 台演示设备，无硬件也能看完整界面。
- `--tray`：启动后只显示托盘图标。
- 关闭窗口隐藏到托盘；右键菜单可打开、刷新、开机自启、看日志、退出。
- 开机自启只写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `2.4GCenter` 值。

## 9. 构建与发布

### 9.1 工具链

- w64devkit v2.10.1 x64，自解压包，SHA-256：
  `ADD1EDF984CE4D473CFEB7C52E0AB38DB73A855BBA6C7F564D5E16648EB2896A`
- `build.ps1` 查找顺序：
  1. `-Compiler` 参数
  2. `$env:W64DEVKIT\bin\g++.exe`
  3. `tools\w64devkit\bin\g++.exe`
  4. `tools\w64devkit*.7z.exe`（首次构建自动解压）
  5. 常见安装路径 / `PATH`
- 必需：`g++`、`windres`。

### 9.2 命令

```powershell
.\build.ps1
# 或
mingw32-make -f Makefile
```

关键编译参数：`-std=c++20 -m64 -O2 -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE
-D_WIN32_WINNT=0x0A00 -municode -mwindows -static -s -Wl,--gc-sections`。

链接：`gdi32 gdiplus user32 shell32 advapi32 hid setupapi comctl32 ole32`。

产物：`WirelessCenter.exe`（x64、PE32+、静态链接、已去符号）。

### 9.3 发布

- 仓库：<https://github.com/SeyanaA/2.4g-device-center>
- 版本资源：`src/resource.rc`（当前 `1.0.5.0`）。
- runtime 包：`2.4G-Device-Center-v1.0.5-win64.zip`
- buildkit 包：`2.4G-Device-Center-v1.0.5-buildkit-win64.zip`
- 发布方式：GitHub REST API（本机无 `git.exe`），`gh` CLI 已登录 `SeyanaA`。
- 每次发版必须：
  1. 更新 `resource.rc` 版本。
  2. 重新构建并确认 PE Machine `0x8664`。
  3. 跑 `--demo` 或 `--once` 冒烟。
  4. 打两个 zip、记录 SHA-256。
  5. 上传源码 + 创建 Release + 上传资产。
  6. 验证 `/releases/latest` 与实际资产名。

## 10. 验证记录

已验证：

- 构建目标 `x86_64-w64-mingw32`，输出 PE32+ / Machine `0x8664`。
- `--diag` 能枚举 HID 接口和系统电池。
- Zaopin Z4D 实机：电量/充电/电压/当前 DPI/档位/回报率/去抖读取正确。
- Zaopin DPI 写入：3100 → 1600 → 3100，读回校验和失败恢复路径已跑通。
- 跨传输合并：`tests/test_device_merge.cpp` 9 项用例覆盖接收器/数据线切换、旧快照去重、
  实时值优先、两条链路同时充电时优先 2.4G，以及缺失能力补齐；实机 USB 充电时只显示
  1 个 `Zaopin Z4D`（原始电量 95%、充电、USB 有线）。
- 电量通知状态机：`tests/test_battery_notify.cpp` 11 项用例覆盖低电/严重低电/充满、
  去重、充电重新武装、启动满电不补发和双设备独立；`Shell_NotifyIcon` 调用返回成功。
- 本机 `HKCU\...\PushNotifications\ToastEnabled=0`，Windows 会静默丢弃系统通知；
  应用已显示底部“系统通知已关闭”，托盘菜单可打开 `ms-settings:notifications`。
- `--demo` 8 设备界面、动画、圆环、滑杆、托盘图标。
- buildkit 在干净目录首次自动解压并编译成功。
- 桌面快捷方式指向项目内 exe，可正常启动。

未验证 / 环境受限：

- Logitech、ASUS、Razer、Rapoo 的真实硬件写操作（协议实现来自公开资料，逻辑已审查）。
- 蓝牙 BLE GATT（本项目只走 Windows 标准 HID 电池/厂商 HID++）。
- Windows 10 早期版本、ARM64、32 位 Windows（明确不支持）。
- 代码签名（当前未签名，SmartScreen 可能提示）。

## 11. 安全边界

- 后台只读；写入只在「应用设置」后发生。
- 不做宏、灯效、固件/引导区操作。
- 不需要管理员权限；唯一可选注册表写入是 HKCU 开机自启。
- 不修改厂商驱动目录、系统 HID 栈或其他进程。
- 未知协议默认只读，不盲目写 DPI 表。
- 厂商名称/商标归各自所有者，本项目与厂商无关联。

## 12. 已知限制与下一步

1. 更多真实设备验证：特别是 Logitech/ASUS/Razer/Rapoo 的写路径。
2. Zaopin 轮询率/去抖写入：目前只读；需要更多固件样本确认写入地址和校验。
3. Compx 其它型号（LAMZU/VXE/Redragon/G-Wolves/Endgame）的 DPI 表布局可能不同，
   需要按型号探测后再开放写入。
4. 跨传输合并目前只对 Zaopin Z4D（PID F527/F526）启用；其它型号的接收器/有线 PID
   需要实测后加入 `identity`，不要按品牌一刀切。
5. 系统通知依赖 Windows 总开关；关闭时应用只能提示，不能强制弹出。
6. 可选：为帧编解码增加单元测试（当前以实机读回为主要验证手段）。
7. 可选：代码签名、ARM64 评估、Windows 10 兼容性矩阵。

## 13. 交接速查

```powershell
# 构建
cd D:\Zcode\项目\2.4G-Device-Center
.\build.ps1

# 冒烟
.\WirelessCenter.exe --diag
.\WirelessCenter.exe --demo

# 跨传输合并测试（使用 build 工具链）
g++ -std=c++20 -m64 -I src tests\test_device_merge.cpp src\device_merge.cpp src\common.cpp `
    -o tests\test_device_merge.exe -static -lshell32 -lole32
.\tests\test_device_merge.exe

# 电量通知状态机测试
g++ -std=c++20 -m64 -I src tests\test_battery_notify.cpp src\battery_notify.cpp `
    src\device_merge.cpp src\common.cpp -o tests\test_battery_notify.exe -static -lshell32 -lole32
.\tests\test_battery_notify.exe

# 版本/架构
(Get-Item .\WirelessCenter.exe).VersionInfo.FileVersion
```

关键路径：

- 源码：`D:\Zcode\项目\2.4G-Device-Center\src`
- 产物：`D:\Zcode\项目\2.4G-Device-Center\WirelessCenter.exe`
- 发布包：`D:\Zcode\项目\2.4G-Device-Center\dist`
- 日志：`%APPDATA%\2.4GCenter\log.txt`
- 控制台诊断：`%APPDATA%\2.4GCenter\last-console.txt`
- 本地逆向资料：`_research/`、`docs/research/`（不随公开仓库发布）
