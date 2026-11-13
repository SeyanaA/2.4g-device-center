#include "proto_compx.h"

#include <cassert>
#include <iostream>

namespace {

// 实测 0x04 应答：95%、未充电、4076 mV。
const uint8_t kReal95[17] = {0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x5F,
                             0x00, 0x0F, 0xEC, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0xED};

// 实测 0x03 状态帧，用于拼批量缓冲。
const uint8_t kStatus03[17] = {0x08, 0x03, 0x00, 0x00, 0x00, 0x01, 0x01,
                               0x48, 0x51, 0xC5, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0xEA};

// Z4D 的 0x17 固定占位帧：[5] 恒为 0x0A。
const uint8_t kPlaceholder17[17] = {0x08, 0x17, 0x00, 0x00, 0x00, 0x0A, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x2C};

}  // namespace

int main() {
    // 1. 真实 0x04 帧解析为 95% / 未充电 / 4076 mV。
    {
        int pct = -1, mv = 0;
        bool chg = true;
        assert(ParseCompxBatteryFrame(kReal95, sizeof(kReal95), pct, chg, mv));
        assert(pct == 95 && !chg && mv == 4076);
    }

    // 2. 充电中 100%：08 04 00 00 00 02 64 01 0F C6 ...
    {
        uint8_t f[17] = {0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x64,
                         0x01, 0x0F, 0xC6, 0, 0, 0, 0, 0, 0, 0x0D};
        int pct = -1, mv = 0;
        bool chg = false;
        assert(ParseCompxBatteryFrame(f, 17, pct, chg, mv));
        assert(pct == 100 && chg && mv == 4038);
    }

    // 3. 校验和错误必须丢弃。
    {
        uint8_t f[17];
        for (int i = 0; i < 17; ++i) f[i] = kReal95[i];
        f[16] ^= 0x01;
        int pct = -1, mv = 0;
        bool chg = false;
        assert(!ParseCompxBatteryFrame(f, 17, pct, chg, mv));
    }

    // 4. 百分比越界（101）必须丢弃。
    {
        uint8_t f[17] = {0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x65,
                         0x00, 0x0F, 0xEC, 0, 0, 0, 0, 0, 0, 0xE7};
        int pct = -1, mv = 0;
        bool chg = false;
        assert(!ParseCompxBatteryFrame(f, 17, pct, chg, mv));
    }

    // 5. 全零帧没有信息量，按无读数处理。
    {
        uint8_t f[17] = {0x08, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00,
                         0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x47};
        int pct = -1, mv = 0;
        bool chg = false;
        assert(!ParseCompxBatteryFrame(f, 17, pct, chg, mv));
    }

    // 6. 批量回帧：0x03 + 0x04 拼在同一个缓冲区，仍能找到 0x04。
    {
        uint8_t buf[34];
        for (int i = 0; i < 17; ++i) buf[i] = kStatus03[i];
        for (int i = 0; i < 17; ++i) buf[17 + i] = kReal95[i];
        uint8_t out[17];
        assert(FindCompxFrame(buf, 34, 0x04, out));
        assert(out[6] == 0x5F);
    }

    // 7. 三帧批量（实测 51 字节）取最后一帧的 0x04。
    {
        uint8_t buf[51];
        for (int i = 0; i < 17; ++i) buf[i] = kStatus03[i];
        for (int i = 0; i < 17; ++i) buf[34 + i] = kReal95[i];
        // 中间放一帧 0x12，确保扫描不会只看固定偏移。
        uint8_t fw[17] = {0x08, 0x12, 0x00, 0x00, 0x00, 0x02, 0x01,
                          0x22, 0, 0, 0, 0, 0, 0, 0, 0, 0x16};
        for (int i = 0; i < 17; ++i) buf[17 + i] = fw[i];
        uint8_t out[17];
        assert(FindCompxFrame(buf, 51, 0x04, out));
        assert(out[6] == 0x5F);
    }

    // 8. 省略 report ID 的 16 字节应答也能解析。
    {
        uint8_t stripped[16];
        for (int i = 0; i < 16; ++i) stripped[i] = kReal95[i + 1];
        uint8_t out[17];
        assert(FindCompxFrame(stripped, 16, 0x04, out));
        int pct = -1, mv = 0;
        bool chg = false;
        assert(ParseCompxBatteryFrame(out, 17, pct, chg, mv));
        assert(pct == 95 && mv == 4076);
    }

    // 9. Z4D 的 0x17 是占位帧：VID 3554 不开放回退，20% 不再出现。
    {
        assert(!CompxIsBatteryFallbackVid(0x3554));
        int pct = -1;
        assert(!ParseCompxFallbackFrame(0x3554, kPlaceholder17, 17, pct));
    }

    // 10. 即使 VID 允许（373B），占位帧也必须拒绝。
    {
        assert(CompxIsBatteryFallbackVid(0x373B));
        int pct = -1;
        assert(!ParseCompxFallbackFrame(0x373B, kPlaceholder17, 17, pct));
        // 真实 VXE 回退帧：frame[5]=0x32 -> 100%。
        uint8_t f[17] = {0x08, 0x17, 0x00, 0x00, 0x00, 0x32, 0x00,
                         0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x04};
        assert(ParseCompxFallbackFrame(0x373B, f, 17, pct));
        assert(pct == 100);
    }

    // 11. 半帧/垃圾数据不产生读数。
    {
        uint8_t out[17];
        assert(!FindCompxFrame(kReal95, 6, 0x04, out));
    }

    std::cout << "compx: 11 tests passed\n";
    return 0;
}
