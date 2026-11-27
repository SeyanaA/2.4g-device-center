# 有线 / 蓝牙设备支持规格

> 版本：v1.0.9 待实现（本地 v1.0.8 未发布）
> 目标：设备中心同时列出 2.4G、USB 有线、蓝牙鼠标/键盘，以及**已连接的蓝牙音频耳机**
> （经典 A2DP/HFP，无 HID 接口）；蓝牙音频电量优先 Windows 属性，缺失时对 Samsung
> 耳机走私有 SPP 只读查询；有线设备不显示电量；
> 无线设备插线（有线模式）仍按无线设备处理，并明确标注。

## 1. 产品规则

### 1.1 支持范围

- 列出所有当前连接的 USB 有线、蓝牙 HID **鼠标/键盘/耳机**；无 HID 的经典蓝牙音频耳机
  见 §1.5。
- 未知厂商协议也列出：名称、VID/PID、连接方式、接口信息；不提供写入。
- 不列：接收器/ Dongle 本身、虚拟 HID（HID VHF / Root / Virtual）、
  只有消费者控制（多媒体键）而没有鼠标/键盘/耳机顶层集合的设备、系统电池虚拟设备。
- **耳机判定**（2026-11-15 加入）：
  - 顶层集合命中 HID Telephony 页（`0x0B/*`），或 Consumer 页 `0x0C/0x05`
    （Headphone）→ 耳机；`0x0C/0x06` 是 Graphic Equalizer，不是耳机；
  - 只有 Consumer Control（`0x0C/0x01`）时，产品名需含
    headset/headphone/earphone/earbud/耳机/耳麦/头戴 之一，才按耳机归类——
    键盘的媒体键集合也用 Consumer Control，不能仅凭它判断；
  - 类型优先级：鼠标 > 键盘 > 耳机（耳机也可能暴露消费者集合）；
  - 无名设备回退为 `<厂商>耳机`。
- 耳机不显示 DPI / 轮询率 / 去抖 / 角度卡：
  - 有电量时设备页排布为 **电量 | 连接 / 设备信息 | 电量曲线**；
  - 无电量（如纯有线耳机）时沿用通用排布（连接 + 信息 + 两张“不可提供”占位卡）。

### 1.2 电量规则

新增两个设备字段（`src/protocols.h`）：

- `bool wirelessCapable`：物理型号具备无线/电池能力，与当前链路无关。
- `bool batteryApplicable`：UI/日志/通知是否把“电量”视为该设备属性。

计算规则（`FinalizeDeviceCapabilities`，扫描线程调用两次：补系统电池之前先算一次，
合并后作为发布前最终状态再算一次；函数幂等，只按当前字段重新赋值派生字段）：

```text
wiredHeadset = (conn == Wired && kind == Headset)

wirelessCapable =
    conn != Wired
 || (!wiredHeadset && batteryValid)
 || IsKnownWirelessIdentity(identity)
 || LooksWirelessProduct(name + " " + product)

batteryApplicable =
    batteryValid
 || proto == SystemBattery
 || (proto != GenericHid && proto != BluetoothAudio && conn != Wired)
 || (conn == Wired && wirelessCapable)

if (!batteryApplicable) chargingKnown = false;
```

- 蓝牙音频设备（`Proto::BluetoothAudio`）没有 HID/电池节点：即使 `conn=Bluetooth`
  也不排电量卡、不写 BATTERY/CHARGE/SAMPLE；若将来系统或厂商协议提供
  `batteryValid=true`，仍按 `batteryValid` 分支显示电量。

- 有线设备（非无线型号）：`batteryApplicable=false`，右侧不出现电量卡、不出现迷你曲线；
  设备卡连接标签为 `USB 有线`；列表右侧不画电量环，改画 `USB` 徽章。
- 无线设备插线：`conn=Wired`、`wirelessCapable=true`，连接标签
  `USB 有线 · 无线模式`，电量卡/曲线保留；与接收器条目按 identity 合并，
  两种链路同时在线时优先有线条目（见 §1.4）。
- 蓝牙已知协议（如 Logitech HID++ over `0xFF43`）：`batteryApplicable=true`，电量卡保留。
- 蓝牙通用 HID（未知协议）：`batteryApplicable=false`，除非 Windows 电池服务提供读数
  （此时 `batteryValid=true`，显示电量卡）。
- 低电/充满通知、30 分钟 SAMPLE、电量曲线：只对 `batteryApplicable && batteryValid`
  的设备生效。`batteryApplicable=false` 的设备不产生 BATTERY/CHARGE/SAMPLE。
- **耳机**（2026-11-15）：Windows 电池服务回补允许 `conn=Wired` 的 `Kind::Headset`
  设备（耳机通常没有厂商电量通道），避免同名电池另起一条 `SystemBattery` 重复条目。
  有线耳机即使被系统电池补出 `batteryValid=true`，也不能据此把连接标签改成
  「USB 有线 · 无线模式」；`wirelessCapable` 只保留 identity/产品名两条判定路径。
  **2026-11-17 修正**：`wirelessCapable=true` 的有线条目（无线型号插线）也必须
  吸收系统电池；`FinalizeDeviceCapabilities` 已把它设为 `batteryApplicable=true`，
  否则会出现电量卡存在但系统电池另起条目/被同名逻辑吞掉。
  独立电池条目的类型识别扩展为 headset/headphone/earphone/earbud/耳机/耳麦；
  传输方式按电池设备实例 ID 里的 BT 枚举器（`bthenum`/`bthle`/`bthhfenum`）判定蓝牙，
  否则按 2.4G 接收器处理。

### 1.3 无线设备有线模式的判定优先级

1. `Device.identity` 属于已知无线机型（`zaopin:z4d`、`valkyrie:vk-m3-lite`、
   `valkyrie:vk-m3`，或 `zaopin:` / `valkyrie:` 前缀）。
2. 协议本身表明是无线链路（Rapoo 状态报文 `wired` 位、Logitech/ASUS/Razer 的无线 PID）。
3. 产品名/型号名包含无线关键词：`wireless`、`2.4g`、`dual mode`、`dual-mode`、
   `tri-mode`、`三模`、`双模`、`无线`、`蓝牙`、`bluetooth`、`lightspeed`。
4. 设备返回了真实电量读数（有电池 → 必然无线能力）。

不得仅凭“当前走 USB”就判定为有线设备。已知无线设备插线时，`conn` 仍为 `Wired`
（物理链路事实），UI 通过 `wirelessCapable` 追加 `· 无线模式`。

### 1.4 双链路同时在线时的合并优先级

同一 `identity` 的多个传输条目由 `DedupeDevicesByIdentity` 合并，`MergeScore`
越大越优先：

| 条件 | 加分 |
| --- | --- |
| `online` | +1000 |
| `batteryValid` | +100 |
| `charging` | +50 |
| `online && conn == Wired` | +200 |
| `dpi.valid` | +4 |
| `polling.valid` | +2 |
| `debounce.valid` | +1 |
| `angle.valid` | +1 |

- 有线链路在线即说明数据线已物理连接，因此对所有机型（包括 Zaopin / Valkyrie）
  优先保留有线条目：连接标签显示 `USB 有线 · 无线模式`，电量卡/曲线继续显示。
- 该加分是 **+200**，高于 `batteryValid(+100)` 与 `charging(+50)` 之和：
  只要有线条目仍在扫描结果里，即使无线条目还保留旧电量/充电读数，也由有线条目胜出；
  `CopyMissing` 会把对方的电量补到有线条目上，因此“保留电量”不受影响。
- 有线条目离线时（例如拔线后只剩接收器）不获得该加分，仍由 online / 电量 /
  充电等评分决定，界面回到 `2.4G 无线`。
- `CopyMissing`/旧快照补电量时必须同时复制 `chargingKnown`；否则有线条目在第一次
  `FinalizeDeviceCapabilities` 被置为“未知充电”后，合并补回电量仍永远显示未知。
- `CHARGE` 事件只在前后两条记录都 `chargingKnown=true` 时写入，避免把未知状态
  伪报成充电/放电跳变。Nordic 54 / Lofree 的电量帧没有充电位，必须显式
  `chargingKnown=false`。

### 1.5 蓝牙音频设备（2026-11-18，P0 BUG）

**问题**：经典蓝牙耳机（如 Galaxy Buds4 Pro）只暴露 A2DP/HFP 音频链路，
没有 HID 接口，也没有 Windows Battery 类节点。原实现只枚举 HID 接口和
`GUID_DEVCLASS_BATTERY`，因此设备列表完全看不到它（`--diag`：蓝牙接口 0、
Windows HID 电池 0）。

**规则**：

1. 在扫描线程中额外枚举**当前已连接**的蓝牙设备，只读，不配对、不连接、
   不写入、不发起任何打印/设备请求。
2. 使用 `BluetoothFindFirstDevice` / `BluetoothFindNextDevice`：
   `fReturnConnected=TRUE`，`fReturnAuthenticated=FALSE`，`fReturnRemembered=FALSE`，
   `fIssueInquiry=FALSE`。这样只返回当前已连接的设备，不会把“已配对但未连接”
   的耳机列成在线。
3. 只收音频类：`ulClassofDevice` 的 Major Device Class 为 `0x04`（Audio/Video）。
   其中 Minor Device Class 为 Wearable Headset / Hands-free / Headphones，
   或名称含 headset/headphone/earphone/earbud/buds/耳机/耳麦/头戴 的，归为
   `Kind::Headset`；其余音频设备（如音箱）归为 `Kind::Other`。
4. 条目字段：
   - `id = "BT:<12 位地址>"`，地址来自 `BLUETOOTH_DEVICE_INFO.Address`，稳定可去重；
   - `name/product` 取 `szName`（空时用“蓝牙音频设备”）；
   - `proto = Proto::BluetoothAudio`，`conn = Conn::Bluetooth`，`online = true`；
   - `battery = -1`、`batteryValid = false`、`charging = false`、
     `chargingKnown = false`、`batterySource = None`；
   - `note` 默认 `蓝牙音频设备 · Windows 未提供电量读数（无 HID/电池节点）`；
     读到电量后改为 `…电量来自 Windows 蓝牙属性` 或 `…电量来自耳机私有协议（只读）`；
   - `batteryApplicable` 在无真实电量时为 false（设备页不排电量卡、列表画 `BT` 徽章）；
     有真实电量（Windows 属性或 Samsung SPP）时为 true，按 §1.2 显示电量卡。
5. 与现有条目去重：同一物理设备若已由 HID 路径列出（容器 ID 相同，或名称互相
   包含且长度 ≥3），不再新增音频条目；音频耳机与 HID 鼠标/键盘共用设备列表，
   但不影响它们的电量/写入逻辑。
6. 电量来源按优先级取第一个可用值，绝不伪造/估算：
   1. **Windows 蓝牙属性**：BTHENUM `DEV_*` 节点上的
      `DEVPKEY_Device_BatteryLevel`（0..100）；
   2. **Samsung 耳机私有 SPP（只读）**：仅当设备当前已连接，且
      `BluetoothEnumerateInstalledServices()` 返回
      `2e73a4ad-332d-41fc-90e2-16bef06523f2` 时，打开 RFCOMM 读取
      `EXTENDED_STATUS_UPDATED`/`STATUS_UPDATED` 中的真实电量；
   3. 都没有时保持 `battery=-1`，不产生电量事件。
   - SPP 只读：不配对、不改设置、不写入设备；只建立短连接读取一次状态后关闭。
   - 轮询：同一设备成功读数缓存 60 s，失败后 30 s 内不重试；失败时保留上一次
     真实读数，不把读数改成 0。
   - 只有 `batteryValid=true` 才进入 BATTERY/SAMPLE 事件、电量卡与 7 天曲线。
   - 协议细节、字段偏移与降级规则见 `SPEC-battery-reading.md` §6。
   - Windows 属性仍优先：有属性时不再发起 SPP 连接。

**验收**：

- 连接 Buds4 Pro 后，设备列表出现“煜 的 Buds4 Pro · 蓝牙 · 耳机”，
  电量来自 Samsung SPP 只读查询，显示电量环/电量卡；连接卡写明
  `蓝牙音频设备 · 电量来自耳机私有协议（只读）`。
- 若某蓝牙音频设备的 `DEVPKEY_Device_BatteryLevel` 有 0..100 值：优先使用该值，
  显示电量卡，电量变化写 BATTERY、30 分钟 SAMPLE 与曲线。
- SPP 查询失败（设备忙、被手机占用、协议无应答）：不显示假电量；
  有上一次真实读数时继续显示该读数，否则保持无电量卡。
- 断开耳机后，下一次扫描（默认 5 s）从列表移除。
- 原有蓝牙 HID 鼠标/键盘、USB 有线设备、2.4G 设备行为不变。
- `--once` 列出该设备与电量；`--diag` 输出 `蓝牙音频设备: N`。


## 2. 数据与状态所有权

```text
EnumerateHidInterfaces()
  └─ HidInterface: bus(USB/Bluetooth), containerId, path/basePath, usage, report 长度
        │
        ▼
proto::ReadLogitech/Asus/Razer/Rapoo/Generic/Valkyrie/MoreBetter   （已知协议）
        │
        ▼
proto::AddWiredHidDevices(all, devices)     ← USB/BT 通用回退
        │                                   已知协议已覆盖的 VID/PID 跳过
        ▼
proto::AddBluetoothAudioDevices(            ← 已连接的经典/LE 蓝牙音频耳机
        EnumerateBluetoothAudioDevices(),      无 HID、无电池节点时仍列出
        devices)
        │
        ▼
proto::FinalizeDeviceCapabilities(devices)  ← 先算 wirelessCapable/batteryApplicable；
        │                                   下一步 AddSystemBatteries 依赖 wirelessCapable
        ▼
proto::AddSystemBatteries(devices)          ← 同名设备补电量而不是新增条目
        │
        ▼
MergeDeviceSnapshots（去重 → 旧快照补缺失读数 → 丢弃离线条目）
        │
        ▼
proto::FinalizeDeviceCapabilities(devices)  ← 合并后重算，发布前的最终状态
        │
        ▼
LogSnapshotChanges → Publish（DeviceManager 唯一所有者）
```

- 只在扫描线程读写这些字段；UI 只读快照。
- 回退设备 `online=true`，`batteryValid=false`，`battery=-1`，`chargingKnown=false`。
- 回退设备 `proto=Proto::GenericHid`，只读；`Apply` 返回“该协议只读”。
- `id` 以 `G:` 开头，使用 containerId（无则 `vid:pid:product`）保证稳定唯一。
- `identity` 留空（未知设备不做跨传输合并）；已知机型仍由原协议设置 identity。

## 3. HID 枚举扩展

`HidInterface` 增加：

- `enum class HidBus { Unknown, Usb, Bluetooth } bus`
- `std::wstring containerId`（`DEVPKEY_Device_ContainerId`；取不到时为空）

判定：

- 路径包含 `bthhfenum` / `bthledevice` / `BTHENUM` → Bluetooth。
- 路径包含 `hid#vid_` → USB。
- 路径看不出时逐级查父设备实例 ID：`BTHENUM` / `BTHLE` / `BTHHFENUM` /
  `BTHUSB` / `BTHPORT` 均视为蓝牙；先检查起始节点，深度上限 16。
- 实例 ID 的 `vid_`/`pid_` 与蓝牙常见的 `vid&`/`pid&` 都要解析；8 位十六进制
  取低 16 位。
- 其它 → Unknown（不参与通用回退）。

`AddWiredHidDevices` 规则：

1. 按 containerId 分组；无 containerId 时按 `vid:pid:小写 product` 分组。
2. 组内必须有 usage page `0x0001` 且 usage `0x0002`（鼠标）或 `0x0006`（键盘）。
3. 排除虚拟/系统：VID `0x5E2A`、product 含 `HID VHF`/`virtual`/`虚拟`/
   `root`；路径含 `hid_device_system_vhf`。
4. 排除 product 含 `receiver`/`dongle`/`接收器` 的组（接收器本体不列出）；
   “鼠标+键盘+wireless” 的复合集合按接收器处理，避免把接收器列成设备。
5. 与已知协议去重：若 `out` 中已有 `vid+pid` 相同且（`Device.containerId`
   相同或 basePath 相同或名称互相包含）的设备，跳过。
   注意：已知协议把 `d.ifaces` 设为本次枚举的全部接口，不能用它反查容器；
   否则组自身的接口必然命中，会误跳过另一台同型号设备。只有双方都有
   `Device.containerId` 时才用容器做最终裁决（不同容器 = 不同物理设备）。
6. 连接方式：Bluetooth → `Conn::Bluetooth`；USB → `Conn::Wired`。
7. 名称：product；为空时用 `manufacturer + 鼠标/键盘`。
8. note：
   - USB 且 `LooksWirelessProduct`：`无线设备的有线模式 · 未读到电量通道`
   - USB 其它：`USB 有线设备 · 不提供电量（无电池）`
   - 蓝牙：`蓝牙 HID 设备 · 电量由系统电池服务提供时才显示`
9. `ifaces=all`，`path` 选厂商页（usagePage ≥ 0xFF00）优先，否则鼠标/键盘集合。

`AddSystemBatteries` 扩展：

- 已在 `out` 中找到同名设备（互相包含、忽略大小写）且该设备 `!batteryValid` 时，
  把系统电池的 `battery/charging/millivolts` 写进该设备，`batteryValid=true`，
  `chargingKnown=true`，note 追加 ` · 电量来自 Windows 电池服务`，不再新增条目。
- 原“已有有效电量则跳过”的行为保留。

## 4. UI

### 4.1 设备页

- `Layout()` 依据所选设备的 `batteryApplicable` 决定是否排布电量卡（140 px）与
  迷你曲线（280 px）；隐藏时 DPI/性能卡上移，`devContentH_` 同步减少，
  滚动条按新高度计算。
- `Paint()`：`batteryApplicable=false` 时跳过 `PaintBatteryCard` 与 `PaintMiniChart`。
- 设备卡连接标签（新 `DeviceConnText`）：
  - `Wireless24` → `2.4G 无线`
  - `Bluetooth` → `蓝牙`
  - `Wired && wirelessCapable` → `USB 有线 · 无线模式`
  - `Wired` → `USB 有线`
- 左栏列表：`batteryApplicable=false` 时不画电量环，画 `USB` / `BT` 圆角徽章
  （灰底、深灰字）；有电量的无线设备保持原电量环。
- 托盘：优先选择有有效电量的设备；只有有线/蓝牙无电量设备时，图标画 `USB`/`BT`
  徽章，tooltip 列出设备名与连接方式，不写“等待电量”。

### 4.2 日志与 CLI

- 有线/蓝牙设备插入/拔出写 `CONNECT`/`DISCONNECT`：
  - CONNECT 详情：`USB 有线` 或 `蓝牙`（无 pct/mv/chg）。
  - DISCONNECT 详情：`USB 有线 设备已断开`（`batteryApplicable=false` 时）。
- 不写 BATTERY/CHARGE/SAMPLE。
- 活动日志列表与导出包含这些 CONNECT/DISCONNECT；
  电量曲线对无 pct 的设备显示“暂无电量记录”。
- `--once`：电量列显示 `--`，连接方式显示 `USB 有线`/`蓝牙`。
- `--diag`：每行接口前缀 `[USB]` / `[BT]`，并输出
  `bus=USB|BT container=<前 8 位>`；末尾汇总
  `USB 接口: N · 蓝牙接口: M · 有线/蓝牙回退新增: K`。
  另输出 `蓝牙音频设备: A` 及每个设备的地址/名称/连接状态（A 为当前连接且
  Major Device Class 为 Audio/Video 的条目数，包含无 HID 的经典耳机）。
  `K` 的语义：`--diag` 先跑与正式扫描完全相同的读取管线
  （`ReadLogitech` → `ReadAsus` → `ReadRazer` → `ReadRapoo` → `ReadGeneric` →
  `ReadValkyrie` → `ReadMoreBetter`），再调用 `AddWiredHidDevices`，`K` 是
  已知协议去重后**实际新增**的通用回退条目数，不是未经去重的候选总数。

## 5. 验收场景

1. 插 USB 有线鼠标/键盘（未知协议）：列表出现，连接 `USB 有线`，无电量卡/曲线，
   DPI/性能卡上移，右侧滚动条按新高度工作。
2. 拔掉该设备：日志出现 `DISCONNECT  USB 有线 设备已断开`，列表移除。
3. 无线鼠标插线（Zaopin/Valkyrie）：只显示一个条目，连接
   `USB 有线 · 无线模式`，电量/充电仍显示；接收器与数据线同时在线时同样以
   有线为准，电量取有线条目读数；拔线后回到 `2.4G 无线`。
4. 蓝牙鼠标/键盘：列表出现，连接 `蓝牙`；Windows 电池服务有读数时显示电量，
   没有时不显示电量卡。
5. 已知蓝牙协议（Logitech HID++ `0xFF43`）：电量卡正常。
6. `--diag` 能区分 USB/BT 接口并打印 containerId；`--once` 输出连接方式。
7. 有线设备不触发低电/充满通知，不写 SAMPLE，不进入电量曲线数据点。
8. 列表徽章：有线 `USB`、蓝牙 `BT`，无线有电量设备仍是圆环。
9. 关闭/重启应用后，有线设备重新出现，identity/id 稳定。
10. 设备在无线与有线之间切换时，设备页布局随 `batteryApplicable` 立即重排，
    不出现空白电量卡或残留曲线。
11. 连接经典蓝牙耳机（无 HID）：
    - Samsung SPP 耳机（如 Buds4 Pro）：显示电量环/电量卡，连接卡标注
      “电量来自耳机私有协议（只读）”；
    - 其它无电量来源的耳机：无电量环/电量卡，连接卡写明“Windows 未提供电量读数”。
    音频卡可按名称匹配到播放端点。
12. 断开该耳机：下一次扫描后从列表移除；重新连接后恢复。
13. `--diag` 输出 `蓝牙音频设备: N` 并列出地址/名称；`--once` 设备行协议名为
    “蓝牙音频”，电量为实际读数或 `--`。
