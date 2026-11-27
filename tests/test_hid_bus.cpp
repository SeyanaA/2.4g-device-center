#include "hid.h"

#include <cassert>
#include <iostream>

namespace {

// 蓝牙 HID 的路径判定：HID service UUID 与 bth* 枚举器都必须命中，
// 普通 USB HID 路径不能误判（否则 --diag/扫描会把有线设备标成蓝牙）。
void TestBluetoothPaths() {
    assert(PathLooksBluetooth(
        L"\\\\?\\hid#{00001124-0000-1000-8000-00805f9b34fb}_vid&0001000f_pid&0000"));
    assert(PathLooksBluetooth(
        L"\\\\?\\hid#{00001812-0000-1000-8000-00805f9b34fb}_dev_vid&0000046d"));
    assert(PathLooksBluetooth(L"\\\\?\\HID#BTHLEDevice#{00001124-0000-1000-8000-00805f9b34fb}"));
    assert(PathLooksBluetooth(L"\\\\?\\HID#BTHENUM#7&1234&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"));
    assert(PathLooksBluetooth(L"\\\\?\\HID#BTHHFENUM#BthHFEnum#..."));
    assert(PathLooksBluetooth(L"\\\\?\\hid#bthle#..."));
    assert(!PathLooksBluetooth(
        L"\\\\?\\hid#vid_046d&pid_c548&mi_01&col01#7&2a622c47&0&0000"
        L"#{4d1e55b2-f16f-11cf-88cb-001111000030}"));
    assert(!PathLooksBluetooth(L""));
}

// 父链兜底使用设备实例 ID：BTHENUM/BTHLE/BTHHFENUM 命中，USB 实例不命中。
void TestBluetoothInstanceIds() {
    assert(InstanceIdLooksBluetooth(L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001000F"));
    assert(InstanceIdLooksBluetooth(L"BTHLE\\DEV_001122334455\\7&1a2b3c&0&0000"));
    assert(InstanceIdLooksBluetooth(L"BTHHFENUM\\BthHFEnum\\7&1a2b3c&0&0000"));
    assert(InstanceIdLooksBluetooth(L"bthenum\\lowercase"));
    assert(!InstanceIdLooksBluetooth(L"USB\\VID_046D&PID_C548\\6&1a2b3c&0&0001"));
    assert(!InstanceIdLooksBluetooth(L"HID\\VID_1EA7&PID_0064\\7&565c6c0&0&0000"));
    assert(!InstanceIdLooksBluetooth(L""));
}

// 路径里同时出现 USB 与蓝牙特征时必须先判蓝牙（BTHENUM 路径常带 HID#VID_）。
void TestBluetoothWinsOverUsbPath() {
    const std::wstring path =
        L"\\\\?\\HID#VID_046D&PID_B013&BTHENUM#7&1a2b3c&0&0000"
        L"#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    assert(PathLooksBluetooth(path));
}

// 蓝牙实例 ID 的父链可能是 BTHUSB/BTHPORT；也必须识别。
void TestBluetoothParentEnumerators() {
    assert(InstanceIdLooksBluetooth(L"BTHUSB\\VID_8087&PID_0026\\5&1a2b3c&0&1"));
    assert(InstanceIdLooksBluetooth(L"BTHPORT\\DEV_001122334455\\7&1a2b3c&0&0000"));
}

// 蓝牙 HID 的实例 ID 常写成 vid&/pid&（8 位十六进制）；USB 的 vid_/pid_
// 也要继续支持。旧实现只找 vid_，会把蓝牙设备判成 0000:0000。
void TestParseVidPidForms() {
    uint16_t vid = 0, pid = 0;
    assert(ParseVidPid(L"\\\\?\\hid#vid_046d&pid_c548&mi_01&col01#7&a&0&0000", vid, pid));
    assert(vid == 0x046D && pid == 0xC548);
    assert(ParseVidPid(L"\\\\?\\hid#{00001124-0000-1000-8000-00805f9b34fb}_vid&0001000f_pid&0000",
                       vid, pid));
    assert(vid == 0x000F && pid == 0x0000);
    assert(!ParseVidPid(L"\\\\?\\hid#bthle#no-vid-here", vid, pid));
}

}  // namespace

int main() {
    TestBluetoothPaths();
    TestBluetoothInstanceIds();
    TestBluetoothWinsOverUsbPath();
    TestBluetoothParentEnumerators();
    TestParseVidPidForms();
    std::cout << "hid_bus: 5 tests passed" << std::endl;
    return 0;
}
