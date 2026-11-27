#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Samsung Galaxy Buds 私有 SPP 的只读电量查询。
//
// 依据 GalaxyBudsClient 的 Buds4ProDeviceSpec / ExtendedStatusUpdateDecoder：
// SOM 0xFD、EOM 0xDD、2 字节小端头（低 10 位为长度）、CRC-16/CCITT。
// 本模块只读取设备主动上报的状态帧，不配对、不写设备设置。
// 协议与降级规则见 docs/SPEC-battery-reading.md §6。

struct SamsungBudsStatus {
    int batteryLeft = -1;
    int batteryRight = -1;
    // 充电仓电量；0 视为“不可用”，不参与单条设备电量。
    int batteryCase = -1;
    bool chargingLeft = false;
    bool chargingRight = false;
    bool chargingCase = false;
    // 只有状态帧确实带有充电位时才为 true；未知不能写成“未充电”。
    bool chargingKnown = false;
};

enum class SamsungFrameParse {
    NeedMore,  // 数据不足，继续读取
    Skipped,   // 当前字节不是合法帧起点；consumed 为应跳过的字节数
    Status,    // 已解析出电量状态
    Other,     // 合法帧，但不是电量状态
};

// CRC-16/CCITT：poly 0x1021、init 0、不反射。计算范围是 msg_id + payload。
uint16_t SamsungBudsCrc16(const uint8_t* data, size_t len);

// 解析 data 起始处的一帧。consumed 返回本帧总字节数（Skipped 时通常为 1）。
SamsungFrameParse ParseSamsungBudsFrame(const uint8_t* data, size_t len,
                                        size_t& consumed, SamsungBudsStatus& out);

// 左/右耳电量折算成单条设备电量；两侧都为 0 且未充电时返回 -1。
int SamsungBudsDeviceBattery(const SamsungBudsStatus& status);

// 设备当前是否安装了 Samsung SPP 服务（2e73a4ad-…）。
// 只读枚举，结果按地址缓存：命中 10 分钟，未命中 60 秒。
bool SamsungBudsServiceAvailable(const std::wstring& addressHex);

// 只读查询一次：RFCOMM 连接 -> 等待状态帧 -> 立即关闭。
bool QuerySamsungBudsStatus(const std::wstring& addressHex, SamsungBudsStatus& out,
                            uint32_t timeoutMs = 2500);

// 带缓存的查询入口：成功读数复用 60 秒；失败后 30 秒内不重试，
// 且不覆盖上一次成功读数。
bool GetSamsungBudsStatusCached(const std::wstring& addressHex, SamsungBudsStatus& out);
