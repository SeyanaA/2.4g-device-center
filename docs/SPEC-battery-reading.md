# 电量读取规格（Compx / Zaopin / Samsung 耳机）

> 状态：实现中 · 适用 v1.0.7+
>
> 目标：电量只来自设备真实应答；读不到就按“离线”处理，绝不拿占位帧凑数字。

## 1. 数据源与唯一所有者

- `DeviceManager::ScanOnce()` 调用 `proto::ReadGeneric()` 读取；`Device` 上的
  `battery` / `charging` / `millivolts` / `batteryValid` 由协议层写入。
- `MergeDeviceSnapshots()` 只做跨传输合并与离线过滤，不产生新读数。
- UI 不解析协议帧，只显示快照。

## 2. 帧接收规则

Compx `0xFF02` 通道一次 `ReadFile` 可能返回多帧（实测一次 51 字节 = 3 帧），
也可能只返回半帧。因此：

1. 必须在缓冲区**任意偏移**寻找 `08 <cmd>` 帧，不能只看 `buf[0]`。
2. 帧必须是 17 字节且校验和成立：`sum(frame[0..15]) + frame[16] == 0x55`。
3. 支持省略 report ID 的 16 字节应答：补回 `0x08` 后按同一校验和验证。
4. 发命令前先 drain 输入队列，丢弃上一轮残留帧，避免把旧 `0x04` 当新读数。
   Nordic 54 / Cherry / Lofree 使用同一规则；Nordic 的匹配条件不区分 link，
   不 drain 时可能把 link1 的旧帧当成 link0。
5. 解析失败继续等，直到超时；超时才算“没读到”。
6. 电量帧没有充电位（Nordic / Lofree / Compx `0x17` 回退）时必须
   `chargingKnown=false`，不能把“未知”写成未充电。

## 3. 电量命令

| 命令 | 用途 | 解析 |
| --- | --- | --- |
| `0x03` | 状态/唤醒 | 忽略内容，仅用于唤醒休眠鼠标 |
| `0x04` | 电量（唯一可信来源） | `pct=frame[6]`、`charging=frame[7]!=0`、`mv=(frame[8]<<8)|frame[9]` |
| `0x17` | 部分 VXE/ATK 型号的电量 | `pct=frame[5]*2`，**只对 VID `0x373B` 开放** |

`0x17` 对 Zaopin Z4D（VID `0x3554`）返回固定占位帧：

```text
08 17 00 00 00 0A 00 00 00 00 00 00 00 00 00 00 2C
```

`frame[5]` 恒为 `0x0A`，按 `*2` 解析会得到 **20%**。这不是电量，必须拒绝：

- `CompxIsBatteryFallbackVid(vid)` 只允许 `0x373B`。
- 即使 VID 允许，也要拒绝“除 `frame[5]` 全为 0 的占位帧”。
- 占位帧被拒绝后按“没读到”处理，不产生电量。

## 4. 读不到时的行为

- `batteryValid=false` → `online=false` → 被 `MergeDeviceSnapshots()` 过滤，
  列表/托盘不显示该设备（与“离线设备不展示”一致）。
- 不显示“上次读数 20%”，不显示 0%，不根据电压估算百分比。
- 鼠标唤醒或移动后，下一轮扫描（默认 5s）重新读取并显示。

## 5. 验收场景

1. Z4D 清醒：`0x04` → `5F 00 0F EC` → 95%、未充电、4076 mV。
2. 一次读到 `0x03`+`0x04` 批量帧：仍能取到 `0x04`，不误判失败。
3. `0x04` 超时但 `0x17` 返回占位帧：设备不显示，日志记录一次读取失败。
4. 校验和错误的帧：丢弃，不使用其中任何字节。
5. 报告中 `pct > 100`：丢弃。
6. 有线 `F526` 与 2.4G `F527` 同时在线：仍按 `zaopin:z4d` 合并，
   有线在线条目优先（+200），显示 `USB 有线 · 无线模式`，电量由 `CopyMissing` 补齐。

## 6. 蓝牙耳机：Samsung SPP 只读电量（Buds4 Pro）

> 适用范围：经典蓝牙音频耳机（A2DP/HFP），Windows 未提供
> `DEVPKEY_Device_BatteryLevel`，但设备暴露 Samsung 专有 SPP 服务。
> 本机验证型号：Galaxy Buds4 Pro（Samsung VID `0x0075`，设备 ID `0x0167`）。

### 6.1 发现与连接

- 仅对**当前已连接**的音频设备尝试：扫描线程已通过
  `BLUETOOTH_FIND_DEVICE`（connected-only）得到设备。
- 用 `BluetoothEnumerateInstalledServices()` 检查服务列表，必须包含
  `2e73a4ad-332d-41fc-90e2-16bef06523f2`（Samsung SPP）。没有该服务不发起连接。
- 服务检查结果按地址缓存：命中 10 分钟、未命中 60 秒；设备重连后服务不会变化。
- 用 Winsock RFCOMM 连接：
  `socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM)` →
  `SOCKADDR_BTH{btAddr=<12 位地址>, serviceClassId=2e73a4ad-…}`。
- **只读**：连接后不发送任何设备配置/写入命令；收到状态帧即关闭。不配对、
  不改音量/降噪/触控，不写注册表。
- 连接使用非阻塞 `connect` + `select`，总预算 `2500 ms`（连接 + 等待状态帧）；
  超时按失败处理，不让扫描线程无限等待。

### 6.2 帧格式（Buds4 Pro，新式头）

```text
FD | size_lo size_hi | msg_id | payload... | crc_lo crc_hi | DD
```

- `size = (size_hi << 8 | size_lo) & 0x03FF`，含义是 **msg_id + payload + CRC**
  的字节数（即 `payloadSize = size - 3`）。
- 头部高位是标志位（观察到 `0x4000/0x8000/0xC000`、`0x1000`），解析时必须
  先用 `0x03FF` 取长度，不能把整个 16 位当长度。
- CRC：CRC-16/CCITT，多项式 `0x1021`，初值 `0`，不反射；计算范围是
  `msg_id + payload`，结果与线上小端 CRC 比较（样例：`0xA84D`）。
- 完整帧长度 = `size + 4`（SOM 1 + 头 2 + body size + EOM 1）。
- 解析失败（SOM/EOM/CRC/长度）时丢弃 1 字节后重新同步，不采用其中任何字段。
- 帧内可能出现嵌套帧（如 `DEBUG_EVENT` 的 payload），按外层 `size` 跳过，
  不递归解析。

### 6.3 状态消息与电量字段

`EXTENDED_STATUS_UPDATED`（`msg_id = 97`，主来源）：

| payload 下标 | 字段 | 说明 |
| --- | --- | --- |
| 2 | `BatteryL` | 左耳 0..100 |
| 3 | `BatteryR` | 右耳 0..100 |
| 7 | `BatteryCase` | 充电仓 0..100；0 视为“不可用”，不显示 |
| 43 | `ChargingStatus` | 位掩码：`0x10` 左耳充电、`0x04` 右耳充电、`0x01` 充电仓充电 |

- `payloadSize >= 8` 才读取 L/R/Case；`payloadSize >= 44` 才认为
  `chargingKnown = true`，否则 `chargingKnown = false`，不得把“未知”写成未充电。
- `BatteryL/BatteryR` 超出 0..100（如 `0xFF`）视为无效，保持 `-1`。

`STATUS_UPDATED`（`msg_id = 96`，兼容回退）：

| payload 下标 | 字段 |
| --- | --- |
| 1 | `BatteryL` |
| 2 | `BatteryR` |

该消息没有充电位，`chargingKnown = false`。

### 6.4 单条设备读数的选择规则

设备页/列表只有一个电量值，按以下规则从 L/R 计算：

1. 两侧都有效且 >0：取较小值（先耗完的一侧）。
2. 只有一侧 >0：取该侧（另一侧可能在仓内/未连接，不能显示 0）。
3. 两侧都是 0 且 `chargingKnown && charging`：显示 0（确实在充电且耗尽）。
4. 其余情况：`battery = -1`，不显示电量。

`charging = chargingL || chargingR`；`chargingKnown` 直接沿用状态帧。
充电仓电量只记录在状态结构里，不用于单条设备电量，也不伪造。

### 6.5 轮询、缓存与失败降级

- 成功读数缓存 **60 s**；同一设备 60 s 内复用缓存，不重复开 SPP。
- 失败后 **30 s** 内不重试；失败不覆盖上一次成功读数（继续显示上一次真实值），
  从未成功过则保持 `battery = -1`。
- 5 s 扫描周期只读缓存，因此电量变化最迟约 60 s 反映到 BATTERY/SAMPLE/曲线。
- 设备断开后缓存条目不再使用；重新连接可立即重新查询（缓存按地址保留，
  超过 TTL 自动刷新）。
- 扫描线程内同步执行，单次最多阻塞 2.5 s；不得在 UI 线程发起连接。

### 6.6 验收场景

1. Buds4 Pro 已连接：查询返回 `EXTENDED_STATUS_UPDATED`，左右耳 100%，设备页
   显示电量卡/电量环，`chargingKnown=true`、`charging=false`。
2. 采样帧（真实抓包，CRC `0xD2B4`）：

   ```text
   FD 41 00 61 04 0D 64 64 01 01 12 00 00 04 FF 22 00 00 67 01 67 01
   09 00 04 99 00 04 01 10 00 00 00 00 11 02 00 01 00 00 00 00 00 00
   00 00 00 00 00 00 00 00 00 00 01 01 00 00 00 FF 01 03 00 00 00 00
   00 01 B4 D2 DD
   ```

   → `BatteryL=100`、`BatteryR=100`、`BatteryCase=0`、`charging=false`。
3. CRC 错 1 字节：整帧丢弃，不产生读数。
4. 截断帧：继续等待；超时后按失败处理。
5. 左右耳 0/80：显示 80；0/0 且未充电：不显示电量；0/0 且充电：显示 0。
6. 设备被手机占用导致 SPP 连接失败：保留上次读数，30 s 后重试。
7. Windows 同时提供 `DEVPKEY_Device_BatteryLevel`：优先用 Windows 值，
   不发起 SPP 连接。
8. 不支持该 SPP 的耳机（如 WH-1000XM4）：不发起连接，行为与旧版一致。
