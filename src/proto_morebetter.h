#pragma once

#include <cstdint>
#include <vector>

#include "protocols.h"

// More&Better / SHARKOON 1EA7:0064 8-byte vendor protocol:
// TX B5 01 00 00 00 00 00 00 -> RX B5 01 01 <pct> ...
bool ParseMoreBetterBattery(const uint8_t* buf, size_t len, int& percent);
void ReadMoreBetter(const std::vector<HidInterface>& all, std::vector<Device>& out);
