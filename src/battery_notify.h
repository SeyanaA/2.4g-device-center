#pragma once

#include <map>
#include <string>
#include <vector>

#include "protocols.h"

enum class BatteryNotifyKind {
    Low,
    Critical,
    Charged,
};

struct BatteryNotification {
    BatteryNotifyKind kind = BatteryNotifyKind::Low;
    std::wstring key;
    std::wstring title;
    std::wstring text;
    bool warning = true;
};

// Per-device low/critical/full battery state machine. It only produces events
// on transitions, so the UI can forward every returned item to the system
// notification area without spamming the user.
class BatteryNotifier {
public:
    std::vector<BatteryNotification> Update(const std::vector<Device>& devices);
    void Reset();

private:
    struct State {
        bool seen = false;
        bool charging = false;
        int battery = -1;
        bool lowNotified = false;
        bool criticalNotified = false;
        bool fullNotified = false;
    };

    std::map<std::wstring, State> states_;
};
