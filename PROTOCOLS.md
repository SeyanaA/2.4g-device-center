# 协议笔记

本文记录本项目实际使用的 HID 帧格式。协议事实来自公开逆向资料、社区源码和本机抓包/探测；本项目只重写协议逻辑，不复制第三方代码。

> 安全规则：后台轮询只发状态/电量查询；写操作只能在用户点击「应用设置」后发生。未知设备默认只读。

## 通用 HID 约定

- 所有 `WriteFile` 长度必须等于 `OutputReportByteLength`；`ReadFile` 的缓冲区不能小于 `InputReportByteLength`。
- Windows 的 HID 路径通常包含 `vid_xxxx&pid_yyyy&mi_XX&colXX`。`col` 是 collection，配置通道往往不是鼠标/键盘 collection。
- 鼠标休眠时，无线接收器可能仍在线，但对设备查询无响应。必须保留「接收器在线 / 设备休眠」状态，不能把 0 当成 0%。

---

## 1. Logitech HID++ 2.0

适用：Logitech Lightspeed / Unifying / Bolt 接收器，以及直连 USB / 蓝牙的 HID++ 设备。

HID 通道（vendor page `0xFF00`，蓝牙 `0xFF43`）：

| 集合 | report ID | 长度 | 用途 |
| --- | --- | --- | --- |
| short | `0x10` | 7 | HID++ 1.0 寄存器 |
| long | `0x11` | 20 | HID++ 2.0 feature call |

2.0 请求：

```text
11 <deviceIndex> <featureIndex> <(function<<4)|0x0A> <16 bytes params>
```

接收器配对表（device index 1..6，接收器自身 0xFF）：

```text
TX 10 FF 83 B5 20|slot ...       // 读配对信息
   响应 [7..8] = wireless PID，[11]&0x0F = 设备类型
TX 10 FF 83 B5 40|slot ...       // 读设备名，[5]=长度，[6..] = UTF-8
```

功能 ID：

| Feature | 用途 |
| --- | --- |
| `0x0000` | IRoot：feature index / protocol version |
| `0x0005` | Device Name / Device Type |
| `0x1004` | Unified Battery：fn1 → `[0]=percent, [1]=approx, [2]=status` |
| `0x1000` | Battery Status：fn0 → `[0]=level, [1]=next, [2]=status` |
| `0x1001` | Battery Voltage：fn0 → `[0..1]=mV BE, [2] bit7=charging` |
| `0x2201` | Adjustable DPI：fn2 读，fn1 列表，fn3 写 |
| `0x2202` | Extended DPI：fn5 读，fn2 ranges，fn3 presets，fn6 写 |
| `0x8060` | Report Rate：fn0 mask，fn1 读，fn2 写（ms） |
| `0x8061` | Extended Report Rate：fn1 mask，fn2 读，fn3 写（code） |
| `0x8100` | Onboard Profiles：fn2 模式，fn11 当前 DPI 档，fn12 切换 |

`0x8061` 速率 code：0=125, 1=250, 2=500, 3=1000, 4=2000, 5=4000, 6=8000 Hz。

参考：[Solaar](https://github.com/pwr-Solaar/Solaar)、[viorizz/mouseutility](https://github.com/viorizz/mouseutility)。

---

## 2. ASUS ROG 私有 HID

适用：ROG 鼠标/键盘（含 OMNI 接收器 `0x1ACE`）。VID `0x0B05`，vendor usage page `0xFF01`（部分型号 `0xFF00`）。

- 普通设备：64/65 字节 output/input report，report ID `0`。
- OMNI/部分接收器：report ID `3`，64 字节；payload 相对普通设备偏移 1 字节。

请求（以 64 字节为例，C# 参考实现中的 `raw` 已去掉 report ID）：

```text
buf[0] = reportId
buf[1] = cmd low
buf[2] = cmd high
buf[3..] = params
```

响应 echo `[1..2] == cmd`；`[1]=FF,[2]=AA` 表示接收器在但设备没回答（休眠）。
去掉 report ID 后，payload 从 `[4]` 开始。

| 命令 | 含义 | 关键字节 |
| --- | --- | --- |
| `0x0712` | 鼠标电量 | `[4]=percent`，`[9]!=0` 充电（旧型号 raw×25） |
| `0x1201` | 键盘电量 | `[5]=percent`，`[8]==1` 充电 |
| `0x0412` param 2 | DPI 表 | 4 档：X=`[4+i*4]|[5+i*4]<<8`，Y=`[6+i*4]|[7+i*4]<<8`，DPI=X*50+50 |
| `0x0012` | 当前 profile | `[10]=profile`，`[11]=activeStage+1` |
| `0x0412` param 0 | 轮询/去抖/角度 | `[12]&7` 轮询索引，`[14]` 去抖索引，`[16]` 角度捕捉 |
| `0x3151` params 9,0,stage+1,0 | 切换 DPI 档 | active stage |
| `0x3151` params stage,0,lo,hi | 写 DPI | idpi=(dpi-50)/50 |
| `0x3151` params 4,0,idx,0 | 轮询率 | `{125,250,500,1000}` |
| `0x3151` params 5,0,idx,0 | 去抖 | `{4,8,12,16,20,24,28,32}` ms |
| `0x3151` params 6,0,on,0 | 角度捕捉 | |
| `0x0350` | 保存 | 无参数 |

旧型号（Chakram/Pugio/Keris/Impact II Wireless/Gladius II Wireless/Strix Carry）电量 raw×25；Strix Carry 电量偏移为 `[6]`。

参考：[libratbag asus.c](https://github.com/libratbag/libratbag/blob/master/src/asus.c)、[rog-mouse-control-center](https://github.com/Artwinter/rog-mouse-control-center)、[peripheral-battery-tray](https://github.com/SteveWang92/peripheral-battery-tray)。

---

## 3. Razer HID

适用：Razer 鼠标/键盘/耳机。VID `0x1532`。Windows 上使用 **90 字节 feature report**（前面再加 report ID `0x00`，共 91 字节）。

报文：

```text
buf[0] = 0                 // report ID
report[0] = status         // 0x02 = OK, 0x00/0x01 = busy
report[1] = transactionId
report[5] = dataSize
report[6] = commandClass
report[7] = commandId
report[8..] = arguments
report[88] = XOR(report[2..87])
```

读响应时寻找 class/id echo；transaction id 按 class 探测：候选 `FF, 1F, 3F, 08, 00`。无线接收器可能返回上一条/空报文，必须校验 echo，不能把 0 当电量。

| 功能 | class/id | 解析 |
| --- | --- | --- |
| 电量 | `0x07/0x80`, size 2 | `percent=round(report[9]*100/255)` |
| 充电 | `0x07/0x84`, size 2 | `report[9] != 0` |
| DPI 读 | `0x04/0x85`, size 7, arg[0]=1 | `x=report[9]<<8|report[10]`，`y=report[11]<<8|report[12]` |
| DPI 写 | `0x04/0x05`, size 7 | `[1, xHi,xLo, yHi,yLo,0,0]` |
| 轮询读（标准） | `0x00/0x85`, size 1 | code 1=1000, 2=500, 8=125 |
| 轮询写（标准） | `0x00/0x05`, size 1 | code |
| 轮询读（扩展） | `0x00/0xC0`, size 1 | 1=8000, 2=4000, 4=2000, 8=1000, 0x10=500, 0x40=125 |
| 轮询写（扩展） | `0x00/0x40`, size 2 | `[0, code]` |

参考：[OpenRazer](https://github.com/openrazer/openrazer)、[Non-Synapse-Mouse](https://github.com/Damnedapa/Non-Synapse-Mouse)。

---

## 4. Rapoo 2.4G

适用：Rapoo VT / gaming 系列（VID `0x24AE`）。

HID：

- Output report ID `6`：32 字节命令。
- Feature report ID `8`：32 字节响应。
- Input report ID `7`：周期状态报文（约 3 秒），含电量/DPI/档位/轮询率。

命令：

```text
out[0] = 6
out[1] = conn       // 0xA5 无线，0xFF 有线
out[2] = cmd        // 0xA4 读 EEPROM, 0xA5 写 EEPROM
out[3] = len
out[4..7] = addr LE32
out[8..] = data
```

Feature 8 响应：Linux 下可能是 `[0]=8,[1]=status,[2..4]=echo,[5..]=data`；Windows 通常带 report ID，使用同一布局。status `0x01=OK`，`0x02=busy`，其余失败。

Profile 0 基址 `0x0600`，字段偏移：

| 偏移 | 字段 |
| --- | --- |
| +640 | 轮询率：8=125, 4=250, 2=500, 1=1000, 132=2000, 130=4000, 129=8000 |
| +648 | 7 档 DPI X，u16 LE |
| +662 | 启用档数 - 1 |
| +664 | 当前档位（0..6） |
| +704/+705 | 按下/抬起去抖 ms |
| +707 | bit0 角度捕捉，bit1 ripple |
| +712 | 7 档 DPI Y，u16 LE |

状态报文（report 7，含 report ID）：`[1]&0x0F` 连接类型（0 无线，2 有线），`[2]` 当前档，`[3..4]` DPI X LE，`[5..6]` DPI Y LE，`[8]` 电量%，`[11]` 轮询 code。

参考：[rapoo-software-linux PROTOCOL.md](https://github.com/pedro3z0/rapoo-software-linux/blob/main/docs/PROTOCOL.md)、[rapoo-mouse-battery](https://github.com/pspfreak/rapoo-mouse-battery)。

---

## 5. Compx 公版（含 Zaopin Z4D）

适用：大量国产 2.4G 鼠标，VID 例如：

- `3554`：LAMZU、Redragon、**Zaopin**
- `373B`：ATK / VXE（老 Compx 型号）
- `25A7`：Redragon / Areson
- `3367`：Endgame Gear
- `33E4`：G-Wolves
- `3151`：Ajazz

通道：usage page `0xFF02`，usage `0x0002`，input/output 均 17 字节，report ID `8`。

帧：

```text
p[0] = 0x08
p[1] = cmd
p[2..15] = payload
p[16] = (0x55 - sum(p[0..15])) & 0xFF
```

常用命令（本机 Zaopin Z4D 实测）：

| cmd | 响应 | 说明 |
| --- | --- | --- |
| `0x04` | `08 04 00 00 00 02 <pct> <chg> <mV hi> <mV lo> ...` | 电量 |
| `0x17` | `08 17 00 00 00 0A ...` | 部分 VXE 型号电量 = `[5]*2` |
| `0x03` | `08 03 00 00 00 01 01 48 51 C5 ...` | 状态/唤醒 |
| `0x12` | `08 12 00 00 00 02 01 22 ...` | 固件版本 |
| `0x0E` | `08 0E ... 01 ...` | 当前配置/profile |

电量解析（Z4D 实测）：

```text
pct = rx[6]
charging = rx[7] != 0
millivolts = (rx[8] << 8) | rx[9]
```

Compx 闪存读（Z4D 实测可用，无 AES）：

```text
TX: 08 08 00 <addrHi> <addrLo> <len> ... <checksum>
RX: 08 08 00 <addrHi> <addrLo> <len> <data> 00 00 <checksum>
```

- 响应可能被批量/延迟返回；必须按 `addr`/`len` 匹配，不能只看第一条。
- Z4D 每次返回 8 字节数据。

Z4D 闪存字段（当前固件）：

| 偏移 | 含义 |
| --- | --- |
| 0x00 | 报告率位掩码（1=1000,2=500,4=250,8=125,16=2000,32=4000,64=8000；本机 0x20=4000Hz） |
| 0x02 | maxDPI / 启用档位数（本机 1） |
| 0x04 | 当前 DPI 档（0-based，本机 0） |
| 0x06/0x08 | X/Y 休眠/省电 |
| 0x0A | 静默高度 |
| 0x0C..0x2B | 8 个 DPI 档记录：`[x][y][DPIex][crc]` |
| 0x2C..0x4B | 8 个 DPI 灯颜色：`[R][G][B][crc]` |
| 0x4C..0x4F | DPI 灯效：mode/brightness/speed/enable |
| 0xA9 | 按键去抖 ms（实测 8） |
| 0xAB | Motion Sync / 相关开关 |

DPI 记录解码：

```text
crc = (0x55 - x - y - DPIex) & 0xFF
code = x | (((DPIex >> 6) & 0x03) << 8)
unit = (DPIex & 0x22) ? 100 : 50
DPI  = (code + 1) * unit
if (DPIex & 0x11) DPI *= 2
```

Z4D 实际闪存 8 个槽位：`3100, 800, 1600, 400, 3200, 6400, 6400, 6400`；但 `maxDPI=1`，官方 UI 只启用第 0 槽，所以当前 DPI = 3100。回报率 0x20 → 4000 Hz，去抖 0xA9 = 8 ms。
写路径（本版本仅对 Zaopin 开放 DPI，其他 Compx 仍只读）：

```text
TX: 08 07 00 <addrHi> <addrLo> <len> <data...> <checksum>
```

- DPI 档位 i：`addr=0x0C+4*i`，data=`[xDPI, yDPI, DPIex, 0x55-(x+y+ex)]`。
- 当前档：`addr=0x0004`，data=`[stage, 0x55-stage]`。
- **无 AES、无 commit、无全局 CRC**；ACK 为整帧原样回显。
- 本程序只写设备启用的档位/当前档：先原值回写自检，再写新值并读回校验；失败尝试恢复原值，应用前弹确认。
- 未实现回报率/去抖/按键/宏/灯效写入；这些仍只读。
- `AesEncryption` 仅用于官方固件升级文件，与 HID 配置无关。

参考：[svetikas/battery](https://github.com/svetikas/battery)、[OpenVXE](https://github.com/BuSd777/OpenVXE)、[vxe-battery-tray PROTOCOL.md](https://github.com/ivancacic/vxe-battery-tray/blob/main/PROTOCOL.md)、[lamzu-battery](https://github.com/lavandiI/lamzu-battery)。

---

## 6. Nordic 54

适用：ATK / VXE 新接收器（usage page `0xFF05`，64 字节 input/output）。

请求（64 字节，首字节 `0x08` 为 report ID）：

```text
08 7D 72 02 00 <link> 07 01
```

`link=1` 表示 2.4G 接收器后的鼠标，`link=0` 表示数据线直连。响应匹配 `[1]==0x72 && [5]==0x07`：

```text
[2] == 0 ? [7]=电量% : 鼠标休眠
[8] | [9]<<8 = 电池电压 mV
```

参考：[vxe-battery-tray](https://github.com/ivancacic/vxe-battery-tray)、[ATK-A9-Battery](https://github.com/Lkig/ATK-A9-Battery)。

---

## 7. Aurora / mouse.xyz 网页驱动

适用：LAMZU Maya X/Inca/Paro、G-Wolves Fenrir/Lycan 等使用 [mouse.xyz](https://mouse.xyz) WebHID 白标驱动的设备（VID 如 `373E`、`37B0`、`33E4`）。

通道：usage page `0xFFFF`，65 字节 feature report（含 report ID `0`），interface 2。

当前协议（Aurora）：

```text
TX(64): [2]=0x02, [3]=0x02, [5]=0x83
RX:     [1]=0xA1, [4]=0x02, [6]=0x83, [7]=charging, [8]=percent
```

旧版 `0x8F`：

```text
TX: [1]=0x02, [2]=0x8F
RX: [1]=0xA1, [2]=0x02, [3]=0x8F, [5]=charging, [6]=percent
```

只读取电量/充电；不发送 0xB0、固件写入等命令。

参考：[gwolves-battery-overlay PROTOCOL.md](https://github.com/maximelonguepe/gwolves-battery-overlay/blob/main/docs/PROTOCOL.md)、[lamzu-ctl](https://github.com/anany1702/lamzu-ctl)。

---

## 8. Cherry 键盘

VID `0x046A`，PID `0x01AC` / `0x00EA`，`col04`，64 字节 output report。

```text
TX: 04 20 00 1A 06 ...
RX: 04 20 00 1A 06 00 00 00 <percent> <charging> ...
```

参考：[1gcat/BatteryTool](https://github.com/1gcat/BatteryTool)。

---

## 9. Lofree WebHID 键盘

VID `0x388D` / PID `0x0025`，usage page `0xFF1C`，report ID `4`，63 字节 input/output。

```text
START:  [0]=4,[1]=0,[2]=0,[3]=1
CMD:    [0]=4,[1]=0,[2]=0,[3]=0x1A
END:    [0]=4,[1]=0,[2]=0,[3]=2
电量:   命令响应 payload[0]（Win32 缓冲区 [8]）
```

参考：[hid-battery-monitor lofree-hyzen-protocol.md](https://github.com/nguyenhanhatminh0105-hue/hid-battery-monitor/blob/main/docs/lofree-hyzen-protocol.md)。

---

## 10. Windows HID 电池回退

枚举 `GUID_DEVCLASS_BATTERY`，用 `IOCTL_BATTERY_QUERY_TAG` / `QUERY_INFORMATION` / `QUERY_STATUS`：

```text
percent = Capacity * 100 / FullChargedCapacity
charging = PowerState & BATTERY_CHARGING
millivolts = Voltage
```

只保留名称/硬件 ID 看起来像 HID 鼠标/键盘/耳机/无线设备的电池，避免列出笔记本主电池。

---

## 11. 本机 Zaopin Z4D 实测记录

```text
设备: VID_3554 PID_F527  product "Z4D"
配置通道: \\?\hid#vid_3554&pid_f527&mi_01&col05#...
UsagePage=0xFF02 Usage=0x0002 In=17 Out=17 Feature=0

TX: 08 04 00 00 00 00 00 00 00 00 00 00 00 00 00 00 49
RX: 08 04 00 00 00 02 32 00 0F 30 00 00 00 00 00 00 D6
    -> 50%, 未充电, 3888 mV

TX: 08 12 00 ... 3B
RX: 08 12 00 00 00 02 01 22 ... -> 固件 2.1.34

闪存读取实测（唤醒后）:
TX: 08 08 00 00 0C 08 ... 31
RX: 08 08 00 00 0C 08 3D 3D 00 DB 0F 0F 00 37 00 00 87
    -> 0x0C 档 1/2；当前 0x04=0，3100 DPI

官方驱动日志（2026-10-22，71%）:
BatteryLevel Len(4): 47 00 0F 90 -> 71%, 0, 3984 mV
ReadFlashData 0x00 Len(6912): 整片配置
ReadFlashData 0x4C Len(8): 02 53 80 D5 03 52 01 54

官方驱动日志（2026-10-30，USB 充电中）:
BatteryLevel Len(4): 54 01 0F C6 -> 84%, 1, 4038 mV
```

同一只 Z4D 有两种 USB 身份：

```text
2.4G 接收器: PID_F527, product "Z4D"
USB 有线/充电: PID_F526, product "2.4G Dual Mode Mouse"
```

两者都是 Compx `0xFF02` 17 字节通道，闪存布局相同；应用用 `identity=zaopin:z4d`
把两条传输合并成一个设备。两条链路同时在线时优先取 2.4G 链路（官方驱动读取的链路）；
当前链路提供电量、充电状态和连接标签，旧链路不会作为第二个设备保留上次读数。

电量字节 `payload[0]` 就是百分比，官方驱动和本应用都直接显示原始值，不做 5% 取整。
两条链路或两次采样落在 1% 更新边界两侧时，可能与官方驱动相差 1%；这是采样差异，
不是刻度换算（官方日志 4038 mV 时为 `0x54`=84%，同期另一条链路可能读到 85%）。

官方驱动不是持续轮询：本机日志在 16:53:07 从 F526 采样到 `5E 01 10 44`
（94%、充电、4164 mV）后没有再记录新的 `BatteryLevel`；同一接口在 16:56
读取到 `5F 01 10 61`（95%、充电、4193 mV）。因此官方界面停在 94% 时，
本应用已经显示实时的 95%。

官方驱动还包含 `AesEncryption`、`ReadEncryptedData`、`CS_UsbServer_ReadCurrentDPI` 等符号，
说明官方软件有自己的会话/加密层。但本机实测普通 `0x08` 读闪存可用，响应可能延迟或批量返回，
必须按“地址 + 长度”匹配，不能把一次超时当成没有 DPI；`0x07` 写入前仍必须做自检和读回校验。

---

## 12. Valkyrie VK M3 系列

适用：Valkyrie VK M3 Lite / VK M3。控制通道是 **interface 2**、usage `0x0001:0x0000`，
33 字节输出 / 33 字节输入（Windows 缓冲区包含前导 report ID 0；codec payload 为 32 字节）。

| 连接 | VID:PID | 产品名 |
| --- | --- | --- |
| USB 有线 | `248A:5D2E`（同系列 `5C2E`/`5E2E`） | `VK M3 lite` |
| 2.4G 接收器 | `249A:5C2F`（也有 `248A:5C2F`） | `Wireless-Receiver` |

身份读取：

```text
TX (native, 33 bytes): 00 10 00 ... 00
RX (32-byte payload): 10 00 01 0B 4D 32 32 30 84 01 11 01 00 4E 01 ...
```

| 偏移 | 含义 |
| --- | --- |
| 0–3 | `10 00 01 0B` 响应头 |
| 4–7 | 型号 ASCII：`M220` = VK M3 Lite，`E023` = VK M3 |
| 8–9 | 固件版本，小端 |
| 10 | 传感器选择（本机 `0x11`） |
| 11 | 传感器/变体字段 |
| 12 | 电池状态；本机有线为 `1`、接收器为 `0`（按 bit0 视为充电/有线） |
| 13 | 电量百分比 |
| 14 | 连接状态原始字段 |
| 31 | byte 4..30 的累加和（mod 256） |

本机实测：

```text
有线  : 10 00 01 0B 4D 32 32 30 17 03 11 01 01 64 01 ... 73
        -> M220, 固件 0x0317, 电量 100%, 状态 1
接收器: 10 00 01 0B 4D 32 32 30 84 01 11 01 00 41 00 ... B9
        -> M220, 固件 0x0184, 电量 65%, 状态 0
```

轮询率读取命令 `0x12`，响应头 `12 00 01 01`，byte 4 编码：
`1=1000 Hz, 2=500 Hz, 4=250 Hz, 8=125 Hz`。

DPI 读取命令 `0x13`，响应头 `13 00 01 19`，byte 4 低半字节为启用档数、
高半字节为当前档；从 byte 5 开始每档 4 字节（X/Y 各 2 字节小端）。
传感器 `0x11` 的原始值解码：`<200 → raw*50`，`200..220 → (raw-100)*100`，
`221..232 → (raw-208)*1000`。

当前实现只读电量/DPI/轮询率；写入未实现。`M220` 的有线和接收器条目用
`identity=valkyrie:vk-m3-lite` 合并，同时在线时优先有线，拔线后显示接收器电量。

接收器会延迟/重复返回上一轮 33 字节帧；每次发 `0x10/0x12/0x13` 前必须先 drain
输入队列，否则可能把旧的 `0x10` 电量帧当成新读数，表现为 90%→87% 的跳变。
Valkyrie 只提供整数百分比（byte 13）和充电标志（byte 12），**没有电压字段**，
不要按电压曲线二次换算。

官方驱动交叉核对（`D:\VK M3 Series`）：

- `config.xml` 把 `dev_id=M220` 映射到 `VK M3 lite`，列出 USB `248A:5D2E`/`5C2E`
  和 2.4G `249A:5C2F`，控制接口都是 `MI_02`。
- `VK M3 Series.pdb` 含 `DeviceMainDlg::GetMouseStatusData`、`battery_value`、
  `charge_flag` 符号，和响应 byte 13 / byte 12 的解析对应。
- `device/mouse_vk m3 lite.xml`：`dpi_count=6`，默认档位 800/1200/2000/5000/8000/12000，
  `report_rate default_value=1`（1000 Hz），与本机读取一致（本机第 3 档被用户改为 3300）。
- 官方 UI 在 USB 连接时顶部显示满格电池 + 闪电图标，确认 byte 12 的 charge flag 为 1
  表示“充电中”，byte 13 的 100 表示满电。

---

## 13. 新增设备的调试流程

1. 运行 `WirelessCenter.exe --diag`，找到设备的 vendor collection：
   - 17/17 字节 + `0xFF02`：Compx 家族。
   - 64 字节 + `0xFF05`：Nordic 54。
   - 65 字节 feature + `0xFFFF`：Aurora/mouse.xyz。
   - 90/91 字节 feature + Razer VID：Razer。
   - ASUS VID + 64/65 字节 vendor page：ASUS ROG。
   - 33 字节 out/in + usage `0x0001:0x0000`：Valkyrie VK M3 系列。
2. 先只发只读查询（电量/状态/版本），抓 TX/RX。
3. 确认电量偏移、充电字节、电压字节，再接入 `proto_*`。
4. 涉及写入时，必须能在设备上读回验证；未知加密/校验不要猜写。
5. 把 `--diag` 输出、设备 VID/PID、产品名、TX/RX 十六进制附在反馈里。

## 参考项目

- https://github.com/pwr-Solaar/Solaar
- https://github.com/libratbag/libratbag
- https://github.com/openrazer/openrazer
- https://github.com/viorizz/mouseutility
- https://github.com/OpenMouse-Project/mouse-protocol
- https://github.com/FIZMIE/mist-m3
- https://github.com/yangyiLTS/valkey-mouse-macos
- https://github.com/Artwinter/rog-mouse-control-center
- https://github.com/SteveWang92/peripheral-battery-tray
- https://github.com/1gcat/BatteryTool
- https://github.com/pedro3z0/rapoo-software-linux
- https://github.com/pspfreak/rapoo-mouse-battery
- https://github.com/BuSd777/OpenVXE
- https://github.com/ivancacic/vxe-battery-tray
- https://github.com/lavandiI/lamzu-battery
- https://github.com/maximelonguepe/gwolves-battery-overlay
- https://github.com/Damnedapa/Non-Synapse-Mouse
- https://github.com/nguyenhanhatminh0105-hue/hid-battery-monitor
