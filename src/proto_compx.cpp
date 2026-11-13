#include "proto_compx.h"

#include <cstring>

namespace {

bool ChecksumOk(const uint8_t* f17) {
    uint8_t sum = 0;
    for (int i = 0; i < 16; ++i) sum = (uint8_t)(sum + f17[i]);
    return (uint8_t)(sum + f17[16]) == 0x55;
}

// Z4D 的 0x17 应答：08 17 00 00 00 0A 00 ... 00
// 第 5 字节是帧头里的常量 0x0A（长度/类型），不是电量；按 [5]*2 解析
// 会得到固定 20%，这正是旧版本误报的来源。
bool IsPlaceholderFallback(const uint8_t* f) {
    if (f[5] != 0x0A) return false;
    for (int i = 6; i < 16; ++i) {
        if (f[i] != 0) return false;
    }
    return true;
}

}  // namespace

bool FindCompxFrame(const uint8_t* buf, int len, uint8_t cmd, uint8_t out[17]) {
    if (!buf || len <= 0) return false;
    for (int off = 0; off + 16 <= len; ++off) {
        // 常见形态：report ID + cmd，共 17 字节。
        if (off + 17 <= len && buf[off] == 0x08 && buf[off + 1] == cmd &&
            ChecksumOk(buf + off)) {
            memcpy(out, buf + off, 17);
            return true;
        }
        // 少数固件省略 report ID：cmd 在首字节，共 16 字节；补回 0x08 后
        // 校验和仍按原始 17 字节计算。
        if (buf[off] == cmd) {
            uint8_t f[17] = {};
            f[0] = 0x08;
            memcpy(f + 1, buf + off, 16);
            if (ChecksumOk(f)) {
                memcpy(out, f, 17);
                return true;
            }
        }
    }
    return false;
}

bool ParseCompxBatteryFrame(const uint8_t* frame, int len, int& pct, bool& charging, int& mv) {
    if (!frame || len < 17) return false;
    if (frame[0] != 0x08 || frame[1] != 0x04) return false;
    if (!ChecksumOk(frame)) return false;
    int p = frame[6];
    bool c = frame[7] != 0;
    int v = (frame[8] << 8) | frame[9];
    // 0% 且无充电位、无电压的帧没有信息量，按无读数处理。
    if (p == 0 && !c && v == 0) return false;
    if (p > 100) return false;
    pct = p;
    charging = c;
    mv = v;
    return true;
}

bool CompxIsBatteryFallbackVid(uint16_t vid) {
    // 只保留协议笔记里明确记录的 VXE/ATK 老型号；其它型号一律不猜。
    return vid == 0x373B;
}

bool ParseCompxFallbackFrame(uint16_t vid, const uint8_t* frame, int len, int& pct) {
    if (!frame || len < 17) return false;
    if (!CompxIsBatteryFallbackVid(vid)) return false;
    if (frame[0] != 0x08 || frame[1] != 0x17) return false;
    if (!ChecksumOk(frame)) return false;
    if (IsPlaceholderFallback(frame)) return false;
    int p = frame[5] * 2;
    if (p <= 0 || p > 100) return false;
    pct = p;
    return true;
}
