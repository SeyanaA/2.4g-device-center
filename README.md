# 2.4G Device Center · 2.4G 设备中心

[English](README.en.md) | 中文

A tiny native Windows x64 C++ tray utility for 2.4 GHz wireless peripherals: battery, DPI,
polling rate and key settings. Pure Win32 API + GDI/GDI+, a single ~0.8 MB executable.

一个原生 Windows x64 C++ 小工具，使用 Win32 API 和系统 HID 直接读取 2.4G 无线鼠标、键盘、耳机的电量，并在支持的设备上调整 DPI、轮询率、按键去抖和角度捕捉。

- **原生轻量**：纯 Win32 API + GDI/GDI+，x64（PE32+）单文件 exe，当前约 **0.8 MB**；圆环充能、充电脉冲、DPI 滑杆平滑过渡。
- **圆环电量 UI**：主界面大圆环、设备列表小圆环、托盘图标直接显示百分比。
- **系统通知**：低电量（20%）、严重低电（10%）和充电完成（100% 或拔电 ≥99%）通过 Windows 通知中心提醒；每台设备独立去重。
- **只读优先**：后台轮询只发送电量/状态查询；只有点击「应用设置」才写入设备。
- **不包含宏、灯效、固件写入**。
- 支持 `--demo` 演示模式，没有硬件也能预览界面。

![界面截图](docs/screenshot.png)

## 下载 / Download

从 [GitHub Releases](https://github.com/SeyanaA/2.4g-device-center/releases/latest) 下载
`2.4G-Device-Center-v1.0.6-win64.zip`，解压后运行 `WirelessCenter.exe`（仅支持 64 位 Windows）。

## 快速开始

直接运行 `WirelessCenter.exe`。

- 左栏选择设备，右侧查看电量、DPI 和性能设置。
- 托盘图标显示当前主要设备（优先最低电量鼠标）的电量百分比。
- 关闭窗口会隐藏到托盘；右键托盘菜单可退出、刷新、开机自启。
- 鼠标休眠时圆环会变灰并显示 `--` 或上次读数，移动鼠标后 5 秒内自动恢复。
- 官方厂商驱动（如 Zaopin Mouse Drive、Logitech G HUB、Razer Synapse）可能会独占 HID 接口；读不到时先退出它们。

命令行：

```text
WirelessCenter.exe            启动主界面
WirelessCenter.exe --demo     演示设备预览
WirelessCenter.exe --tray     启动后只显示托盘图标
WirelessCenter.exe --once     控制台枚举设备后退出
WirelessCenter.exe --diag     输出 HID 接口诊断，便于适配新设备
```

`--once` / `--diag` 同时会把结果写到
`%APPDATA%\2.4GCenter\last-console.txt`，日志在 `%APPDATA%\2.4GCenter\log.txt`。

## 架构：Win32 API 与 Win64

`Win32` 是 Windows API 家族名，不是 32 位目标。本项目从第一天起就编译为 **x86-64 / PE32+**：

- 编译器目标 `x86_64-w64-mingw32`，PE Machine `0x8664`，指针和句柄均为 8 字节。
- `src/common.h` 在非 `_WIN64` 或指针不是 8 字节时直接 `#error`，防止误编译成 32 位。
- `build.ps1` 链接后读取 PE 头校验 Machine，不是 `0x8664` 就构建失败。
- `--diag` 第一行会打印 `Architecture: x86-64 / PE32+ (64-bit process)`。
- 使用 `-m64 -O2` 与 section GC，静态链接，不依赖 MinGW/VC++ 运行库。

32 位 Windows 不在支持范围内；也不需要从 Win32 API 迁移到所谓“Win64 API”——Win32 就是 64 位 Windows 的原生 API。更完整的架构与交接说明见 [docs/PROJECT-CONTEXT.md](docs/PROJECT-CONTEXT.md)。

## 支持范围

| 协议/品牌 | 电量 | DPI | 轮询率 | 其它 |
| --- | --- | --- | --- | --- |
| Logitech HID++ 2.0 | ✅ | ✅ 读/写 | ✅ 读/写 | 接收器配对表、设备名、板载档位 |
| ASUS ROG 私有 HID | ✅ | ✅ 4 档读/写 | ✅ 读/写 | 按键去抖、角度捕捉、保存 |
| Razer HID | ✅ | ✅ 读/写 | ✅ 读/写 | 125–8000 Hz（按型号） |
| Rapoo 2.4G | ✅ | ✅ 7 档读/写 | ✅ 读/写 | 去抖、角度捕捉、状态报文 |
| Compx 公版 | ✅ | 部分型号可写（Zaopin 已启用档位） | ❌ | Zaopin / LAMZU / VXE / Redragon / G-Wolves / Endgame 等 |
| Valkyrie VK M3 系列 | ✅ | ✅ 只读 | ✅ 只读 | VK M3 Lite 有线 `248A:5D2E` / 2.4G `249A:5C2F` |
| Nordic 54 | ✅ | ❌ | ❌ | ATK/VXE 新接收器 |
| Aurora / mouse.xyz | ✅ | ❌ | ❌ | LAMZU / G-Wolves 网页驱动系列 |
| Cherry HID | ✅ | ❌ | ❌ | 无线键盘 |
| Lofree WebHID | ✅ | ❌ | ❌ | Hyzen 等键盘 |
| Windows HID 电池 | ✅ | ❌ | ❌ | 通用回退路径 |

已验证/本机联调：

- **Zaopin Z4D**（`VID_3554`；2.4G 为 `PID_F527`，USB 有线/充电为 `PID_F526`；Compx 0xFF02 17 字节通道）：电量、充电、电压、当前 DPI、启用的 DPI 档位、回报率和按键去抖可读；**DPI 档位可写**。
  - 实测：2.4G 50%、3883 mV；USB 充电 95%、4147 mV、当前 3100 DPI、4000 Hz、去抖 8 ms；设备 `maxDPI=1`，因此只启用 1 个档位。
  - 同一只鼠标在接收器和 USB 线之间切换时会按物理设备合并为一个条目；电量、充电状态和连接方式取当前在线/充电链路的实时读数，不再把旧的无线读数保留成第二个设备。
  - 两条链路同时在线时优先显示 2.4G 链路（与官方驱动读取的链路一致）；电量使用设备上报的原始百分比，不做 5% 取整。官方驱动界面可能保留上次采样值（日志中读取一次 `0x5E`=94% 后不再刷新），本应用每 5 秒读取一次，所以充电时可能看到 1% 的“时间差”，不是刻度换算。
  - DPI 写入只针对设备启用的档位/当前档；先做一次原值回写自检，再写新值并读回校验；失败会尝试恢复原值，应用前会弹确认框。
  - 回报率/去抖/宏/灯效仍为只读；需要增加档位数量请用官方 Mouse Drive。
- **Valkyrie VK M3 Lite**（有线 `VID_248A / PID_5D2E`，2.4G 接收器 `VID_249A / PID_5C2F`）：电量、充电标志、DPI 档位和轮询率可读；设置只读。
  - 控制通道是 interface 2、usage `0x0001:0x0000`，33 字节写 / 32 字节 payload；身份命令 `0x10`，电量在响应 byte 13，没有电压字段。
  - 接收器可能延迟/重复返回上一轮帧；发送命令前会先清空输入队列，避免旧电量帧造成 90%→87% 的跳变。
  - 实测有线：100%、充电标志 1、3300 DPI、1000 Hz；接收器：65%、充电标志 0。
  - 已用官方驱动 `D:\VK M3 Series` 交叉核对：`config.xml` 的 `dev_id=M220`、`MI_02` 和 VID/PID 与实测一致；PDB 的 `GetMouseStatusData`、`battery_value`、`charge_flag` 符号对应 byte 13/12；`mouse_vk m3 lite.xml` 的 6 档 DPI/1000 Hz 默认值一致；官方 UI 的满格+闪电图标确认 byte 12=1 为充电中。
  - 有线和接收器同时在线时按 `identity=valkyrie:vk-m3-lite` 合并，优先显示有线链路；拔线后显示接收器的无线电量。
- 其它品牌协议来自公开逆向资料和社区项目，具体型号固件可能有差异；用 `--diag` 反馈即可继续适配。

蓝牙设备只使用 Windows 暴露的标准 HID 电池/厂商 HID++ 路径，不做 BLE GATT 扫描。

## 电量通知

- 使用 Windows 系统通知（托盘 `Shell_NotifyIcon`，Win10/11 会进入通知中心），不是应用内弹窗。
- 未充电且电量 ≤20%：通知一次「电量偏低」；继续降到 ≤10%：再通知一次「电量严重不足」。
- 充电中到 100%，或充电位从 1 变 0 且电量 ≥99%：通知「充电完成」。
- 每台设备独立记录状态；充电开始、电量回到 25% 以上、或放电到 90% 以下会重新允许下一次提醒，避免反复弹窗。
- 首次启动时如果设备已经低电，会立即提醒；如果已经满电/正在充电，不会补发“充电完成”。
- 系统通知总开关关闭时，应用底部会显示 `系统通知已关闭`，托盘右键菜单提供「通知设置」直接打开 Windows 通知设置，并提供「发送测试通知」验证通知是否可见。
- 行为细节见 [docs/SPEC-notifications.md](docs/SPEC-notifications.md)。

## 界面与图标

- 主电量：大号圆环仪表，中心百分比，充电时绿色+闪电/圆点。
- 设备列表：32 px 左右的电量圆环，数字使用大号字形；设备图标为 GDI+ 矢量轮廓（鼠标/键盘/耳机/通用），高 DPI 下抗锯齿清晰。
- 托盘：圆环+百分比数字；100% 且在 16 px 托盘下显示 `F`（悬停提示仍显示 100%）。
- 深色/浅色任务栏都能看清：托盘数字带深色描边，圆环使用中性灰轨道。
- DPI 滑杆为对数刻度，低 DPI 段不会挤在左侧。

## 构建

### 自包含构建包（推荐）

从 [Releases](https://github.com/SeyanaA/2.4g-device-center/releases/latest) 下载
`2.4G-Device-Center-v1.0.6-buildkit-win64.zip`，解压后双击 `build.cmd`，或运行：

```powershell
.\build.ps1
```

包内已带官方 **w64devkit v2.10.1 x64**（`g++`、`windres`、头文件、库）。第一次构建会自动解压到
`tools\w64devkit`，**不需要系统安装 MinGW**。w64devkit 安装包原样分发，SHA-256：

```text
ADD1EDF984CE4D473CFEB7C52E0AB38DB73A855BBA6C7F564D5E16648EB2896A
```

### 使用已有工具链

需要 MinGW-w64（GCC 13+）或 w64devkit，包含 `g++`、`windres`：

```powershell
$env:W64DEVKIT = "C:\path\to\w64devkit"
.\build.ps1

# 或显式指定
.\build.ps1 -Compiler "C:\msys64\mingw64\bin\g++.exe"
```

或使用 Make（工具链需在 `PATH`）：

```powershell
mingw32-make -f Makefile
```

产物：`WirelessCenter.exe`（x64 / PE32+，静态链接，无 VC++/MinGW 运行库依赖）。
构建脚本会拒绝任何非 `0x8664` 的 PE 输出。

依赖的系统 DLL：`kernel32 user32 gdi32 gdiplus shell32 advapi32 hid setupapi comctl32 ole32`。
不使用 Qt、.NET 或 Python 等重型运行时。

## 目录

```text
src/                          C++ 源码
  hid.*                       SetupAPI/HID 枚举、带超时读写
  protocols.*                 Device 数据模型
  device_merge.*              跨传输设备身份合并、快照保留与去重
  battery_notify.*            低电/严重低电/充电完成通知状态机
  proto_logitech.cpp          HID++ 2.0
  proto_asus.cpp              ASUS ROG 私有 64 字节协议
  proto_razer.cpp             Razer 90 字节 feature report
  proto_rapoo.cpp             Rapoo output 6 / feature 8 / status 7
  proto_generic.cpp           Compx / Nordic 54 / Aurora / Cherry / Lofree / 系统电池
  proto_valkyrie.cpp          Valkyrie VK M3 系列电量/DPI/轮询率（只读）
  manager.*                   后台轮询、设备合并、写入队列、demo
  tray.*                      动态圆环托盘图标
  ui.*                        Win32 主界面 + GDI+ 矢量图标/圆环
  resource.rc/app.manifest    图标、版本信息、DPI 感知
resources/app.ico             exe/窗口图标
docs/screenshot.png           界面截图
docs/zaopin-z4d.png           Zaopin Z4D 界面（演示数据；实机读数见正文）
docs/PROJECT-CONTEXT.md       压缩后的项目上下文与交接说明
PROTOCOLS.md                  协议帧与逆向来源
docs/SPEC-notifications.md    电量通知规格
README.en.md                  English README
tests/test_device_merge.cpp   跨传输合并单元测试
tests/test_battery_notify.cpp 电量通知状态机单元测试
tests/test_valkyrie.cpp       Valkyrie 身份/校验单元测试
```

> 本机 Zaopin 驱动的完整只读逆向报告与闪存 dump 属于本地研究产物，不随公开仓库发布；
> 可复现的协议结论已整理在 `PROTOCOLS.md`。

## 安全与边界

- 后台自动查询只包含：状态、电量、版本、配置、状态报文；不写闪存、不写宏、不写灯效、不碰固件。
- 写设置仅由用户点击「应用设置」触发；ASUS 会再发保存命令，Rapoo/Compx 的 EEPROM 写入直接持久化。
- Zaopin DPI 写入会先做原值回写自检，再写入并读回校验；失败恢复原值，且应用前需要用户确认。
- 对未知/未验证的公版设备，优先只读；不会盲目写入 DPI 表。
- 不修改官方驱动目录、注册表（除可选的 HKCU 开机自启）或系统 HID 栈。
- 厂商名称和商标归各自所有者；本项目与厂商无关联。

## 已知限制

- **Valkyrie VK M3 Lite**：目前只读电量/DPI/轮询率；2.4G 接收器的 DPI/轮询率读取可能因固件帧错位而不可用，电量仍可读；没有电压字段，显示的是设备原始百分比，掉电曲线由设备电量计决定，不做二次换算。
- **系统通知**：功能已实现，但需要 Windows 通知总开关开启；关闭时应用底部会提示，托盘菜单可打开通知设置。
- Logitech / ASUS / Razer / Rapoo 的真实硬件写操作仍待更多设备验证。
- 蓝牙只走 Windows 标准 HID 电池路径，不做 BLE GATT 扫描。
- 仅支持 64 位 Windows 10/11；当前未签名，SmartScreen 可能提示。
- 不包含宏、灯效和固件写入。

## 协议资料与致谢

协议帧、特性 ID 和社区项目来源见 [PROTOCOLS.md](PROTOCOLS.md)。主要参考：

- Solaar（Logitech HID++ 2.0）
- libratbag（ASUS ROG、Razer、Roccat 等）
- OpenRazer（Razer 90 字节协议）
- viorizz/mouseutility（Windows HID++ 实现）
- kyoken/libratbag asus、rog-mouse-control-center、peripheral-battery-tray（ASUS ROG 电量/DPI）
- rapoo-software-linux（Rapoo 完整协议文档）
- OpenVXE / VXE battery tray / LAMZU battery / G-Wolves mouse.xyz overlay（Compx、Nordic 54、Aurora/WebHID）
- OpenMouse mouse-protocol、mist-m3、valkey-mouse-macos（Valkyrie VK M3 身份/电量/DPI/轮询率）
- Non-Synapse-Mouse（Razer Windows feature report 实现）
- BatteryTool / lofree-hyzen-protocol / hid-battery-monitor（Cherry、Lofree、通用 HID 电池）

本项目只重写了协议事实，未复制上述项目的源代码。

## License

MIT。详见 `LICENSE`。
