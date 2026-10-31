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

void TestDedupePrefersWirelessWhenBothCharging() {
    // Both links are healthy and charging. The official driver reads the 2.4G
    // link, and the two links can differ by one point (84 vs 85), so keep the
    // 2.4G entry to stay consistent with the vendor software.
    Device wireless = Wireless(84, true);
    wireless.charging = true;
    Device wired = Wired(85, true, true);
    std::vector<Device> devices{wireless, wired};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 1);
    assert(devices[0].id == Wireless().id);
    assert(devices[0].conn == Conn::Wireless24);
    assert(devices[0].battery == 84);
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

void TestUnrelatedDevicesStaySeparate() {
    Device other = MakeDevice(L"C:vid_373b&pid_1085&mi_01#7&ccc&0&0000", L"", L"VXE R1 SE+",
                              true, 71, false, Conn::Wireless24);
    std::vector<Device> devices{Wireless(), Wired(), other};
    DedupeDevicesByIdentity(devices);
    assert(devices.size() == 2);
}

}  // namespace

int main() {
    TestDedupePrefersLiveChargingTransport();
    TestDedupePrefersWirelessWhenBothCharging();
    TestDedupeOrderDoesNotMatter();
    TestDedupeFillsMissingCapabilities();
    TestSnapshotDropsReplacedTransport();
    TestSnapshotKeepsLastBatteryWhenWiredHasNoReading();
    TestSnapshotSwitchBackToWireless();
    TestSnapshotReplacesBothOldEntries();
    TestUnrelatedDevicesStaySeparate();
    std::cout << "device_merge: 9 tests passed\n";
    return 0;
}
