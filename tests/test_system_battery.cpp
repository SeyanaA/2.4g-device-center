#include "protocols.h"

#include <cassert>
#include <iostream>

namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

SystemBattery Bat(const std::wstring& name, int percent) {
    SystemBattery b;
    b.id = L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0002046d";
    b.name = name;
    b.percent = percent;
    b.charging = true;
    b.millivolts = 3900;
    b.capacity = 60;
    b.fullCapacity = 100;
    return b;
}

Device Dev(const std::wstring& name, Conn conn = Conn::Bluetooth,
           Kind kind = Kind::Mouse) {
    Device d;
    d.id = L"D:" + name;
    d.name = name;
    d.product = name;
    d.conn = conn;
    d.kind = kind;
    d.proto = Proto::GenericHid;
    d.online = true;
    d.wirelessCapable = conn != Conn::Wired;
    return d;
}

// 精确同名且已有协议电量：系统电池是同源读数，不能覆盖。
void TestExactDuplicateKeepsProtocolReading() {
    Device d = Dev(L"MX Anywhere 3S");
    d.battery = 80;
    d.batteryValid = true;
    std::vector<Device> out{d};
    proto::MergeSystemBatteries({Bat(L"MX Anywhere 3S", 55)}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].battery == 80);
    CHECK(out[0].batteryValid);
}

// 精确同名无电量：补上系统电池并标记来源。
void TestExactMatchFillsReading() {
    Device d = Dev(L"MX Anywhere 3S");
    std::vector<Device> out{d};
    proto::MergeSystemBatteries({Bat(L"MX Anywhere 3S", 55)}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].battery == 55);
    CHECK(out[0].batteryValid);
    CHECK(out[0].charging);
    CHECK(out[0].note.find(L"Windows") != std::wstring::npos);
}

// “Mouse” 已有电量时，不能因此吞掉 “Mouse Pro” 的系统电池。
void TestExactNameBeatsPartialName() {
    Device base = Dev(L"Mouse");
    base.battery = 90;
    base.batteryValid = true;
    Device pro = Dev(L"Mouse Pro");
    std::vector<Device> out{base, pro};
    proto::MergeSystemBatteries({Bat(L"Mouse Pro", 44)}, out);
    CHECK(out.size() == 2);
    CHECK(out[0].battery == 90);
    CHECK(out[1].battery == 44);
    CHECK(out[1].batteryValid);
}

// 唯一候选但系统没有百分比：不能新建一条重复的 SystemBattery。
void TestUniqueCandidateWithoutPercentDoesNotDuplicate() {
    std::vector<Device> out{Dev(L"Headset X", Conn::Bluetooth, Kind::Headset)};
    proto::MergeSystemBatteries({Bat(L"Headset X", -1)}, out);
    CHECK(out.size() == 1);
    CHECK(!out[0].batteryValid);
}

// 多个互含候选：歧义，不补也不新增。
void TestAmbiguousPartialCandidatesAreSkipped() {
    std::vector<Device> out{Dev(L"Acme Mouse One"), Dev(L"Acme Mouse Two")};
    proto::MergeSystemBatteries({Bat(L"Acme Mouse", 50)}, out);
    CHECK(out.size() == 2);
    CHECK(!out[0].batteryValid);
    CHECK(!out[1].batteryValid);
}

// 多个精确同名候选同样按歧义处理。
void TestAmbiguousExactCandidatesAreSkipped() {
    std::vector<Device> out{Dev(L"Twin Mouse"), Dev(L"Twin Mouse")};
    proto::MergeSystemBatteries({Bat(L"Twin Mouse", 50)}, out);
    CHECK(out.size() == 2);
    CHECK(!out[0].batteryValid);
    CHECK(!out[1].batteryValid);
}

// 无同名设备：创建独立 SystemBattery 条目，并按实例 ID 判蓝牙/耳机类型。
void TestIndependentBatteryEntry() {
    std::vector<Device> out;
    proto::MergeSystemBatteries({Bat(L"Bluetooth Headphones", 66)}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].proto == Proto::SystemBattery);
    CHECK(out[0].conn == Conn::Bluetooth);
    CHECK(out[0].kind == Kind::Headset);
    CHECK(out[0].battery == 66);
    CHECK(out[0].batteryValid);
}

// 无百分比且无同名设备：不创建幽灵条目。
void TestNoGhostEntryWithoutPercent() {
    std::vector<Device> out;
    proto::MergeSystemBatteries({Bat(L"Unnamed Battery", -1)}, out);
    CHECK(out.empty());
}

// 离线设备不吸收系统电池，也不能因此冒出重复条目。
void TestOfflineDeviceIsNotFilled() {
    Device d = Dev(L"MX Anywhere 3S");
    d.online = false;
    std::vector<Device> out{d};
    proto::MergeSystemBatteries({Bat(L"MX Anywhere 3S", 55)}, out);
    CHECK(out.size() == 1);
    CHECK(!out[0].batteryValid);
}

// 有线耳机允许系统电池补电量（否则会另起重复条目）。
void TestWiredHeadsetAbsorbsBattery() {
    Device d = Dev(L"USB Headset", Conn::Wired, Kind::Headset);
    d.wirelessCapable = false;
    std::vector<Device> out{d};
    proto::MergeSystemBatteries({Bat(L"USB Headset", 77)}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].battery == 77);
    CHECK(out[0].batteryValid);
}

// 普通有线设备不吸收系统电池，也不新建同名条目。
void TestWiredMouseDoesNotAbsorbBattery() {
    Device d = Dev(L"USB Mouse", Conn::Wired, Kind::Mouse);
    d.wirelessCapable = false;
    std::vector<Device> out{d};
    proto::MergeSystemBatteries({Bat(L"USB Mouse", 77)}, out);
    CHECK(out.size() == 1);
    CHECK(!out[0].batteryValid);
}

// 无线型号插线（conn=Wired、wirelessCapable=true）也必须吸收系统电池：
// Finalize 已把 batteryApplicable 置 true，若不吸收会出现电量卡存在但
// 系统电池另起条目或被同名逻辑吞掉。
void TestWiredWirelessAbsorbsBattery() {
    Device d = Dev(L"2.4G Dual Mode Mouse", Conn::Wired, Kind::Mouse);
    d.wirelessCapable = true;
    d.batteryApplicable = true;
    std::vector<Device> out{d};
    proto::MergeSystemBatteries({Bat(L"2.4G Dual Mode Mouse", 64)}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].battery == 64);
    CHECK(out[0].batteryValid);
    CHECK(out[0].chargingKnown);
}

}  // namespace

int main() {
    TestExactDuplicateKeepsProtocolReading();
    TestExactMatchFillsReading();
    TestExactNameBeatsPartialName();
    TestUniqueCandidateWithoutPercentDoesNotDuplicate();
    TestAmbiguousPartialCandidatesAreSkipped();
    TestAmbiguousExactCandidatesAreSkipped();
    TestIndependentBatteryEntry();
    TestNoGhostEntryWithoutPercent();
    TestOfflineDeviceIsNotFilled();
    TestWiredHeadsetAbsorbsBattery();
    TestWiredMouseDoesNotAbsorbBattery();
    TestWiredWirelessAbsorbsBattery();
    std::cout << "system_battery: 12 tests, " << g_checks << " checks passed\n";
    return 0;
}
