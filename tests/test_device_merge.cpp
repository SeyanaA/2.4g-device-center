#include "device_merge.h"

#include <cassert>
#include <iostream>

namespace {

Device MakeDevice(const std::wstring& id, const std::wstring& identity, const std::wstring& name,
                  bool online, int battery, bool charging, Conn conn) {
    Device d;
    d.id = id;
    d.identity = identity;
    d.name = name;
    d.product = name;
    d.kind = Kind::Mouse;
    d.proto = Proto::Compx;
    d.vid = 0x3554;
    d.online = online;
    d.conn = conn;
    if (battery >= 0) {
        d.battery = battery;
        d.batteryValid = true;
    }
    d.charging = charging;
    return d;
}

Device Wireless(int battery = 35, bool online = true) {
    return MakeDevice(L"C:vid_3554&pid_f527&mi_01#7&aaa&0&0000", L"zaopin:z4d", L"Zaopin Z4D",
                      online, battery, false, Conn::Wireless24);
}

Device Wired(int battery = 50, bool online = true, bool charging = true) {
    return MakeDevice(L"C:vid_3554&pid_f526&mi_01#7&bbb&0&0000", L"zaopin:z4d",
                      L"2.4G Dual Mode Mouse", online, battery, charging, Conn::Wired);
}

void TestDedupePrefersLiveChargingTransport() {
    std::vector<Device> devices{Wireless(), Wired()};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 1);
    assert(devices[0].id == Wired().id);
    assert(devices[0].name == L"Zaopin Z4D");
    assert(devices[0].conn == Conn::Wired);
    assert(devices[0].battery == 50);
    assert(devices[0].charging);
}

void TestDedupePrefersWiredWhenBothCharging() {
    // Both links are healthy and charging. A live wired link means the cable is
    // physically connected, so the wired entry must win for every identity; the
    // receiver may still report a stale wireless reading (84 vs 85). This is the
    // state that renders as "USB 有线 · 无线模式" and keeps the battery card.
    Device wireless = Wireless(84, true);
    wireless.charging = true;
    Device wired = Wired(85, true, true);
    std::vector<Device> devices{wireless, wired};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 1);
    assert(devices[0].id == Wired().id);
    assert(devices[0].conn == Conn::Wired);
    assert(devices[0].battery == 85);
    assert(devices[0].charging);
}

void TestDedupeOrderDoesNotMatter() {
    std::vector<Device> devices{Wired(), Wireless()};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 1);
    assert(devices[0].id == Wired().id);
    assert(devices[0].battery == 50);
}

void TestDedupeFillsMissingCapabilities() {
    Device wireless = Wireless();
    wireless.dpi.valid = true;
    wireless.dpi.x = 3100;
    wireless.dpi.y = 3100;

    std::vector<Device> devices{wireless, Wired()};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 1);
    assert(devices[0].id == Wired().id);
    assert(devices[0].dpi.valid);
    assert(devices[0].dpi.x == 3100);
}

void TestSnapshotDropsReplacedTransport() {
    std::vector<Device> old{Wireless(35)};
    std::vector<Device> fresh{Wired(50)};
    std::vector<Device> merged = MergeDeviceSnapshots(old, fresh);
    assert(merged.size() == 1);
    assert(merged[0].id == Wired().id);
    assert(merged[0].battery == 50);
    assert(merged[0].charging);
    assert(!merged[0].stale);
}

void TestSnapshotKeepsLastBatteryWhenWiredHasNoReading() {
    Device wired = Wired(-1, true, false);
    std::vector<Device> old{Wireless(35)};
    std::vector<Device> fresh{wired};
    std::vector<Device> merged = MergeDeviceSnapshots(old, fresh);
    assert(merged.size() == 1);
    assert(merged[0].id == wired.id);
    assert(merged[0].batteryValid);
    assert(merged[0].battery == 35);
    assert(merged[0].stale);
}

void TestSnapshotSwitchBackToWireless() {
    std::vector<Device> old{Wired(50, true, true)};
    std::vector<Device> fresh{Wireless(35)};
    std::vector<Device> merged = MergeDeviceSnapshots(old, fresh);
    assert(merged.size() == 1);
    assert(merged[0].id == Wireless().id);
    assert(merged[0].battery == 35);
}

void TestSnapshotReplacesBothOldEntries() {
    // Exact transition from the report: the UI still had the wireless entry
    // (35 %, battery power) and an older wired entry (50 %) while the mouse was
    // now charging through USB. The fresh scan must collapse everything into
    // one live wired entry with the current percentage.
    std::vector<Device> old{Wireless(35, true), Wired(50, false, false)};
    std::vector<Device> fresh{Wired(70, true, true)};
    std::vector<Device> merged = MergeDeviceSnapshots(old, fresh);
    assert(merged.size() == 1);
    assert(merged[0].id == Wired().id);
    assert(merged[0].name == L"Zaopin Z4D");
    assert(merged[0].battery == 70);
    assert(merged[0].charging);
    assert(!merged[0].stale);
}

void TestOfflineDevicesAreDropped() {
    Device offline = Wireless(35, false);
    assert(MergeDeviceSnapshots({}, {offline}).empty());

    std::vector<Device> old{Wireless(35, true)};
    assert(MergeDeviceSnapshots(old, {}).empty());
}

void TestUnrelatedDevicesStaySeparate() {
    Device other = MakeDevice(L"C:vid_373b&pid_1085&mi_01#7&ccc&0&0000", L"", L"VXE R1 SE+",
                              true, 71, false, Conn::Wireless24);
    std::vector<Device> devices{Wireless(), Wired(), other};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 2);
}

// 有线条目胜出时，CopyMissing 必须把“充电状态已知”一起补上；否则合并后
// batteryApplicable=true 但 chargingKnown=false，UI/日志永远显示充电未知。
void TestWiredMergeKeepsChargingKnown() {
    Device wireless = Wireless(35, true);
    wireless.charging = true;
    wireless.chargingKnown = true;
    Device wired = Wired(-1, true, false);
    wired.chargingKnown = false;
    std::vector<Device> devices{wired, wireless};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 1);
    assert(devices[0].id == wired.id);
    assert(devices[0].batteryValid);
    assert(devices[0].battery == 35);
    assert(devices[0].charging);
    assert(devices[0].chargingKnown);
}

// 旧快照补电量时同样要补 chargingKnown。
void TestSnapshotStaleBatteryKeepsChargingKnown() {
    Device oldWireless = Wireless(35, true);
    oldWireless.charging = true;
    oldWireless.chargingKnown = true;
    Device wired = Wired(-1, true, false);
    wired.chargingKnown = false;
    std::vector<Device> merged = MergeDeviceSnapshots({oldWireless}, {wired});
    assert(merged.size() == 1);
    assert(merged[0].batteryValid);
    assert(merged[0].battery == 35);
    assert(merged[0].chargingKnown);
    assert(merged[0].stale);
}

}  // namespace

int main() {
    TestDedupePrefersLiveChargingTransport();
    TestDedupePrefersWiredWhenBothCharging();
    TestDedupeOrderDoesNotMatter();
    TestDedupeFillsMissingCapabilities();
    TestSnapshotDropsReplacedTransport();
    TestSnapshotKeepsLastBatteryWhenWiredHasNoReading();
    TestSnapshotSwitchBackToWireless();
    TestSnapshotReplacesBothOldEntries();
    TestOfflineDevicesAreDropped();
    TestUnrelatedDevicesStaySeparate();
    TestWiredMergeKeepsChargingKnown();
    TestSnapshotStaleBatteryKeepsChargingKnown();
    std::cout << "device_merge: 12 tests passed\n";
    return 0;
}
