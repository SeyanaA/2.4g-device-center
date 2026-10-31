#include "battery_notify.h"

#include "device_merge.h"

namespace {

constexpr int kLowPercent = 20;
constexpr int kCriticalPercent = 10;
constexpr int kLowResetPercent = 25;
constexpr int kFullChargingPercent = 100;
constexpr int kFullUnplugPercent = 99;
constexpr int kFullResetPercent = 90;

std::wstring KeyFor(const Device& d) { return DeviceIdentity(d); }

BatteryNotification Make(BatteryNotifyKind kind, const Device& d) {
    BatteryNotification n;
    n.kind = kind;
    n.key = KeyFor(d);
    n.warning = kind != BatteryNotifyKind::Charged;
    const wchar_t* name = d.name.empty() ? L"设备" : d.name.c_str();
    switch (kind) {
        case BatteryNotifyKind::Low:
            n.title = L"电量偏低";
            n.text = Fmt(L"%s 剩余 %d%%，请及时充电。", name, d.battery);
            break;
        case BatteryNotifyKind::Critical:
            n.title = L"电量严重不足";
            n.text = Fmt(L"%s 仅剩 %d%%，请立即充电。", name, d.battery);
            break;
        case BatteryNotifyKind::Charged:
            n.title = L"充电完成";
            n.text = d.battery >= kFullChargingPercent
                         ? Fmt(L"%s 已充满（%d%%）。", name, d.battery)
                         : Fmt(L"%s 电量 %d%%，充电已结束。", name, d.battery);
            break;
    }
    return n;
}

}  // namespace

void BatteryNotifier::Reset() { states_.clear(); }

std::vector<BatteryNotification> BatteryNotifier::Update(const std::vector<Device>& devices) {
    std::vector<BatteryNotification> out;

    for (const auto& d : devices) {
        if (!d.online || !d.batteryValid || d.battery < 0) continue;

        const std::wstring key = KeyFor(d);
        auto [it, inserted] = states_.try_emplace(key);
        State& s = it->second;

        if (inserted || !s.seen) {
            s.seen = true;
            s.charging = d.charging;
            s.battery = d.battery;
            s.lowNotified = false;
            s.criticalNotified = false;
            // 首次看到设备时只提醒已经发生的低电；如果它启动时已经插着电并显示
            // 满电，把本充电周期视为已通知过，避免程序启动后补弹“充电完成”。
            s.fullNotified = d.charging && d.battery >= kFullChargingPercent;
            if (!d.charging) {
                if (d.battery <= kCriticalPercent) {
                    out.push_back(Make(BatteryNotifyKind::Critical, d));
                    s.criticalNotified = true;
                    s.lowNotified = true;
                } else if (d.battery <= kLowPercent) {
                    out.push_back(Make(BatteryNotifyKind::Low, d));
                    s.lowNotified = true;
                }
            }
            continue;
        }

        const bool wasCharging = s.charging;

        // 新一次充电开始：重新武装充满检测；低电提醒也重新允许，
        // 这样拔电后如果仍然低电会再次提醒。
        if (!wasCharging && d.charging) {
            if (d.battery < kFullUnplugPercent) s.fullNotified = false;
            s.lowNotified = false;
            s.criticalNotified = false;
        }

        // 两种“充满”都覆盖：有的固件停在 100% 且继续报充电，
        // 有的固件充满后把充电位从 1 变成 0。
        if (!s.fullNotified) {
            const bool fullWhileCharging = d.charging && d.battery >= kFullChargingPercent;
            const bool fullAfterUnplug =
                wasCharging && !d.charging && d.battery >= kFullUnplugPercent;
            if (fullWhileCharging || fullAfterUnplug) {
                out.push_back(Make(BatteryNotifyKind::Charged, d));
                s.fullNotified = true;
            }
        }

        if (d.charging) {
            // 充电中不报低电；同时清除本放电周期的低电标记。
            s.lowNotified = false;
            s.criticalNotified = false;
        } else {
            // 电量回到 25% 以上才算脱离低电区间，避免在 20% 附近反复提醒。
            if (d.battery > kLowResetPercent) {
                s.lowNotified = false;
                s.criticalNotified = false;
            }
            // 充满后放电到 90% 以下，下一轮充电可再次报“充电完成”。
            if (s.fullNotified && d.battery < kFullResetPercent) {
                s.fullNotified = false;
            }
            if (d.battery <= kCriticalPercent) {
                if (!s.criticalNotified) {
                    out.push_back(Make(BatteryNotifyKind::Critical, d));
                    s.criticalNotified = true;
                    s.lowNotified = true;
                }
            } else if (d.battery <= kLowPercent) {
                if (!s.lowNotified) {
                    out.push_back(Make(BatteryNotifyKind::Low, d));
                    s.lowNotified = true;
                }
            }
        }

        s.charging = d.charging;
        s.battery = d.battery;
    }

    return out;
}
