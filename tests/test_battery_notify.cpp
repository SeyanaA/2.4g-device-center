#include "battery_notify.h"

#include <cassert>
#include <iostream>

namespace {

Device MakeDevice(const std::wstring& id, int battery, bool charging, bool online = true) {
    Device d;
    d.id = id;
    d.identity = id;
    d.name = L"Test Mouse";
    d.kind = Kind::Mouse;
    d.proto = Proto::Compx;
    d.online = online;
    d.battery = battery;
    d.batteryValid = battery >= 0;
    d.charging = charging;
    return d;
}

std::vector<Device> One(const std::wstring& id, int battery, bool charging, bool online = true) {
    return {MakeDevice(id, battery, charging, online)};
}

void TestLowOnce() {
    BatteryNotifier n;
    auto r = n.Update(One(L"d1", 15, false));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Low);
    assert(r[0].warning);
    assert(r[0].title == L"电量偏低");
    assert(r[0].text == L"Test Mouse 剩余 15%，请及时充电。");
    assert(n.Update(One(L"d1", 15, false)).empty());
}

void TestCriticalAfterLow() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 20, false)).size() == 1);
    auto r = n.Update(One(L"d1", 8, false));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Critical);
    assert(r[0].title == L"电量严重不足");
    assert(r[0].text == L"Test Mouse 仅剩 8%，请立即充电。");
    assert(n.Update(One(L"d1", 8, false)).empty());
}

void TestLowResetAbove25() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 15, false)).size() == 1);
    assert(n.Update(One(L"d1", 24, false)).empty());
    assert(n.Update(One(L"d1", 15, false)).empty());
    assert(n.Update(One(L"d1", 26, false)).empty());
    assert(n.Update(One(L"d1", 15, false)).size() == 1);
}

void TestChargingRearmsLow() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 8, false)).size() == 1);
    assert(n.Update(One(L"d1", 8, true)).empty());
    auto r = n.Update(One(L"d1", 8, false));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Critical);
}

void TestFullWhileCharging() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 99, true)).empty());
    auto r = n.Update(One(L"d1", 100, true));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Charged);
    assert(!r[0].warning);
    assert(r[0].title == L"充电完成");
    assert(r[0].text == L"Test Mouse 已充满（100%）。");
    assert(n.Update(One(L"d1", 100, true)).empty());
}

void TestFullAfterUnplug() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 99, true)).empty());
    auto r = n.Update(One(L"d1", 99, false));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Charged);
    assert(n.Update(One(L"d1", 99, false)).empty());
}

void TestUnplugAt80IsNotFull() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 80, true)).empty());
    assert(n.Update(One(L"d1", 80, false)).empty());
}

void TestStartupFullDoesNotNotify() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 100, true)).empty());
    assert(n.Update(One(L"d1", 100, true)).empty());
    assert(n.Update(One(L"d1", 100, false)).empty());
}

void TestTwoDevicesIndependent() {
    BatteryNotifier n;
    auto r = n.Update({MakeDevice(L"d1", 15, false), MakeDevice(L"d2", 8, false)});
    assert(r.size() == 2);
    assert(n.Update({MakeDevice(L"d1", 15, false), MakeDevice(L"d2", 8, false)}).empty());
    auto r2 = n.Update({MakeDevice(L"d1", 15, true), MakeDevice(L"d2", 8, false)});
    assert(r2.empty());
}

void TestOfflineIgnored() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 5, false, false)).empty());
    auto r = n.Update(One(L"d1", 5, false, true));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Critical);
}

void TestFullRearmAfterDischarge() {
    BatteryNotifier n;
    assert(n.Update(One(L"d1", 100, true)).empty());  // startup full: suppressed
    assert(n.Update(One(L"d1", 100, false)).empty());
    assert(n.Update(One(L"d1", 89, false)).empty());  // re-arm below 90
    assert(n.Update(One(L"d1", 95, true)).empty());
    auto r = n.Update(One(L"d1", 100, true));
    assert(r.size() == 1);
    assert(r[0].kind == BatteryNotifyKind::Charged);
}

}  // namespace

int main() {
    TestLowOnce();
    TestCriticalAfterLow();
    TestLowResetAbove25();
    TestChargingRearmsLow();
    TestFullWhileCharging();
    TestFullAfterUnplug();
    TestUnplugAt80IsNotFull();
    TestStartupFullDoesNotNotify();
    TestTwoDevicesIndependent();
    TestOfflineIgnored();
    TestFullRearmAfterDischarge();
    std::cout << "battery_notify: 11 tests passed\n";
    return 0;
}
