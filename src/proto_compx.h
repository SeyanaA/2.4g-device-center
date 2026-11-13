#pragma once

#include "common.h"

// Compx 0xFF02 帧解析：纯函数，不碰 HID 句柄，便于单元测试。
// 背景见 docs/SPEC-battery-reading.md。

// 在缓冲区任意偏移寻找 cmd 的应答帧，并验证校验和。
// 设备会把多帧拼在一次 ReadFile 里返回（实测 51 字节 = 3 帧），
// 所以不能只看 buf[0]。找到时 out 为规范化的 17 字节帧（含 report ID）。
bool FindCompxFrame(const uint8_t* buf, int len, uint8_t cmd, uint8_t out[17]);

// 解析 0x04 电量帧：08 04 00 00 00 02 <pct> <chg> <mV hi> <mV lo> ...
bool ParseCompxBatteryFrame(const uint8_t* frame, int len, int& pct, bool& charging, int& mv);

// 0x17 只在部分 VXE/ATK 型号上携带电量；Zaopin Z4D 的 0x17 是固定占位帧。
bool CompxIsBatteryFallbackVid(uint16_t vid);

// 解析 0x17 回退帧；拒绝占位帧、校验和错误和越界值。
bool ParseCompxFallbackFrame(uint16_t vid, const uint8_t* frame, int len, int& pct);
