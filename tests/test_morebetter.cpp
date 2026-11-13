#include "proto_morebetter.h"

#include <cassert>
#include <iostream>

int main() {
    int pct = -1;
    const uint8_t good[8] = {0xB5, 0x01, 0x01, 0x56, 0x00, 0x00, 0x00, 0x00};
    assert(ParseMoreBetterBattery(good, sizeof(good), pct));
    assert(pct == 86);

    const uint8_t zero[8] = {0xB5, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(ParseMoreBetterBattery(zero, sizeof(zero), pct));
    assert(pct == 0);

    const uint8_t badHeader[8] = {0xB4, 0x01, 0x01, 0x56, 0, 0, 0, 0};
    assert(!ParseMoreBetterBattery(badHeader, sizeof(badHeader), pct));

    const uint8_t badCmd[8] = {0xB5, 0x02, 0x01, 0x56, 0, 0, 0, 0};
    assert(!ParseMoreBetterBattery(badCmd, sizeof(badCmd), pct));

    const uint8_t tooHigh[8] = {0xB5, 0x01, 0x01, 0x65, 0, 0, 0, 0};
    assert(!ParseMoreBetterBattery(tooHigh, sizeof(tooHigh), pct));

    const uint8_t shortFrame[3] = {0xB5, 0x01, 0x01};
    assert(!ParseMoreBetterBattery(shortFrame, sizeof(shortFrame), pct));

    std::cout << "morebetter: 6 tests passed\n";
    return 0;
}
