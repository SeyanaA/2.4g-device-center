#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocols.h"

struct ValkyrieIdentity {
    std::string modelCode;
    uint16_t firmware = 0;
    uint8_t sensor = 0;
    uint8_t sensorVariant = 0;
    uint8_t batteryStatus = 0;
    int batteryPercent = -1;
    uint8_t connectionStatus = 0;
};

// payload is the 32-byte codec payload without the transport report ID.
bool ParseValkyrieIdentity(const uint8_t* payload, size_t len, ValkyrieIdentity& out);
std::wstring ValkyrieModelName(const std::string& modelCode);
