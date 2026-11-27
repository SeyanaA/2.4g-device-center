#include "protocols.h"
#include "samsung_buds.h"

#include <cassert>
#include <iostream>

namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

HidInterface MakeIface(const std::wstring& path, uint16_t vid, uint16_t pid,
                       const std::wstring& product, uint16_t usagePage, uint16_t usage,
                       HidBus bus, const std::wstring& containerId, uint16_t inLen = 8,
                       uint16_t outLen = 8, const std::wstring& manufacturer = L"Acme") {
    HidInterface h;
    h.path = path;
    // 与生产代码保持一致：去掉 \\?\hid# 前缀并按 # 分段，而不是简单截断 &col。
    // 旧测试用截断值，导致 basePath 去重分支从未被真实验证。
    h.basePath = HidPathKey(path);
    h.product = product;
    h.manufacturer = manufacturer;
    h.vid = vid;
    h.pid = pid;
    h.usagePage = usagePage;
    h.usage = usage;
    h.inLen = inLen;
    h.outLen = outLen;
    h.bus = bus;
    h.containerId = containerId;
    return h;
}

Device MakeDevice(Proto p, Conn conn, const std::wstring& name, bool batteryValid) {
    Device d;
    d.id = L"X:test";
    d.name = name;
    d.product = name;
    d.proto = p;
    d.conn = conn;
    d.kind = Kind::Mouse;
    d.online = true;
    d.batteryValid = batteryValid;
    d.battery = batteryValid ? 50 : -1;
    return d;
}

Device* FindByName(std::vector<Device>& devices, const std::wstring& name) {
    for (auto& d : devices) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

void TestLooksWirelessProduct() {
    CHECK(LooksWirelessProduct(L"Logitech G PRO X SUPERLIGHT 2 Wireless"));
    CHECK(LooksWirelessProduct(L"2.4G Dual Mode Mouse"));
    CHECK(LooksWirelessProduct(L"Tri-Mode Keyboard"));
    CHECK(LooksWirelessProduct(L"三模鼠标"));
    CHECK(LooksWirelessProduct(L"LIGHTSPEED"));
    CHECK(!LooksWirelessProduct(L"USB Receiver"));
    CHECK(!LooksWirelessProduct(L"2.4G Dongle"));
    CHECK(!LooksWirelessProduct(L"USB Optical Mouse"));
    CHECK(!LooksWirelessProduct(L""));
}

void TestKnownWirelessIdentity() {
    CHECK(IsKnownWirelessIdentity(L"zaopin:z4d"));
    CHECK(IsKnownWirelessIdentity(L"valkyrie:vk-m3-lite"));
    CHECK(!IsKnownWirelessIdentity(L"logitech:pro-x-superlight"));
    CHECK(!IsKnownWirelessIdentity(L""));
}

void TestFinalizeWiredOnly() {
    std::vector<Device> devices{
        MakeDevice(Proto::GenericHid, Conn::Wired, L"USB Optical Mouse", false)};
    devices[0].chargingKnown = true;
    FinalizeDeviceCapabilities(devices);
    CHECK(!devices[0].wirelessCapable);
    CHECK(!devices[0].batteryApplicable);
    CHECK(!devices[0].chargingKnown);
}

void TestFinalizeWiredWirelessName() {
    std::vector<Device> devices{
        MakeDevice(Proto::GenericHid, Conn::Wired, L"2.4G Dual Mode Mouse", false)};
    devices[0].chargingKnown = true;
    FinalizeDeviceCapabilities(devices);
    CHECK(devices[0].wirelessCapable);
    CHECK(devices[0].batteryApplicable);
    CHECK(devices[0].chargingKnown);
}

void TestFinalizeWirelessNoReading() {
    std::vector<Device> devices{
        MakeDevice(Proto::Logitech, Conn::Wireless24, L"PRO X SUPERLIGHT 2", false)};
    FinalizeDeviceCapabilities(devices);
    CHECK(devices[0].wirelessCapable);
    CHECK(devices[0].batteryApplicable);
}

void TestFinalizeGenericBluetooth() {
    std::vector<Device> devices{
        MakeDevice(Proto::GenericHid, Conn::Bluetooth, L"Bluetooth Mouse", false)};
    devices[0].chargingKnown = true;
    FinalizeDeviceCapabilities(devices);
    CHECK(devices[0].wirelessCapable);
    CHECK(!devices[0].batteryApplicable);
    CHECK(!devices[0].chargingKnown);
}

void TestFinalizeKnownBluetooth() {
    std::vector<Device> devices{
        MakeDevice(Proto::Logitech, Conn::Bluetooth, L"MX Anywhere 3", false)};
    FinalizeDeviceCapabilities(devices);
    CHECK(devices[0].wirelessCapable);
    CHECK(devices[0].batteryApplicable);
}

void TestFinalizeSystemBattery() {
    std::vector<Device> devices{
        MakeDevice(Proto::SystemBattery, Conn::Bluetooth, L"HID-compliant mouse", false)};
    FinalizeDeviceCapabilities(devices);
    CHECK(devices[0].batteryApplicable);
}

// 有线耳机被 Windows 电池服务补出 batteryValid 后，不能因此显示成
// 「USB 有线 · 无线模式」；电量卡仍应可用（batteryApplicable=true）。
void TestFinalizeWiredHeadsetStaysWired() {
    std::vector<Device> devices{
        MakeDevice(Proto::GenericHid, Conn::Wired, L"USB Headset", true)};
    devices[0].kind = Kind::Headset;
    FinalizeDeviceCapabilities(devices);
    CHECK(devices[0].batteryApplicable);
    CHECK(!devices[0].wirelessCapable);
    CHECK(DeviceConnLabel(devices[0]) == L"USB 有线");
}

// UI 与 --once 共用同一连接标签；插线无线设备必须同时出现在两个入口。
void TestDeviceConnLabel() {
    Device wired = MakeDevice(Proto::Compx, Conn::Wired, L"Zaopin Z4D", false);
    wired.wirelessCapable = true;
    CHECK(DeviceConnLabel(wired) == L"USB 有线 · 无线模式");
    wired.wirelessCapable = false;
    CHECK(DeviceConnLabel(wired) == L"USB 有线");

    Device wireless = MakeDevice(Proto::Compx, Conn::Wireless24, L"Zaopin Z4D", true);
    CHECK(DeviceConnLabel(wireless) == L"2.4G 无线");

    Device bt = MakeDevice(Proto::GenericHid, Conn::Bluetooth, L"Bluetooth Mouse", false);
    CHECK(DeviceConnLabel(bt) == L"蓝牙");
}

void TestAddWiredDevices() {
    const std::wstring mouseContainer = L"aaaa1111-2222-3333-4444-555555555555";
    const std::wstring keyboardContainer = L"bbbb1111-2222-3333-4444-555555555555";
    const std::wstring btContainer = L"eeee1111-2222-3333-4444-555555555555";
    const std::wstring existContainer = L"99991111-2222-3333-4444-555555555555";

    std::vector<HidInterface> all;
    // 一只 USB 鼠标：两个鼠标集合 + 一个厂商页集合，共享 containerId。
    all.push_back(MakeIface(L"\\\\?\\hid#vid_1234&pid_0001&mi_01&col01#7&a&0&0000", 0x1234,
                            0x0001, L"Acme Wireless Mouse", 0x0001, 0x0002, HidBus::Usb,
                            mouseContainer));
    all.push_back(MakeIface(L"\\\\?\\hid#vid_1234&pid_0001&mi_01&col02#7&a&0&0000", 0x1234,
                            0x0001, L"Acme Wireless Mouse", 0x0001, 0x0002, HidBus::Usb,
                            mouseContainer));
    all.push_back(MakeIface(L"\\\\?\\hid#vid_1234&pid_0001&mi_01&col03#7&a&0&0000", 0x1234,
                            0x0001, L"Acme Wireless Mouse", 0xFF00, 0x0001, HidBus::Usb,
                            mouseContainer, 64, 64));
    // USB 键盘。
    all.push_back(MakeIface(L"\\\\?\\hid#vid_2345&pid_0002&mi_00&col01#7&b&0&0000", 0x2345,
                            0x0002, L"Acme Keyboard", 0x0001, 0x0006, HidBus::Usb,
                            keyboardContainer));
    // 接收器本身：即使带键盘集合也不能列出。
    all.push_back(MakeIface(L"\\\\?\\hid#vid_3456&pid_0003&mi_00&col01#7&c&0&0000", 0x3456,
                            0x0003, L"USB Receiver", 0x0001, 0x0006, HidBus::Usb,
                            L"cccc1111-0000-0000-0000-000000000000"));
    // 系统虚拟 HID（VID 0x5E2A / HID VHF / Root 路径）。
    all.push_back(MakeIface(L"\\\\?\\hid#vid_5e2a&pid_0004&mi_00&col01#7&d&0&0000", 0x5E2A,
                            0x0004, L"System Mouse", 0x0001, 0x0002, HidBus::Usb,
                            L"dddd1111-0000-0000-0000-000000000000"));
    all.push_back(MakeIface(L"\\\\?\\hid#hid_device_system_vhf&col01#7&d&0&0000", 0x1234,
                            0x0005, L"HID VHF Mouse", 0x0001, 0x0002, HidBus::Unknown,
                            L"dddd2222-0000-0000-0000-000000000000"));
    // 蓝牙鼠标。
    all.push_back(MakeIface(L"\\\\?\\bthhfenum#vid_046d&pid_b025&col01#7&e&0&0000", 0x046D,
                            0xB025, L"Logitech MX Anywhere 3", 0x0001, 0x0002,
                            HidBus::Bluetooth, btContainer));
    // 链路未知：不参与回退。
    all.push_back(MakeIface(L"\\\\?\\root#vid_6666&pid_0777&col01#7&f&0&0000", 0x6666,
                            0x0777, L"Mystery Mouse", 0x0001, 0x0002, HidBus::Unknown,
                            L"ffff1111-0000-0000-0000-000000000000"));
    // 已知协议已经覆盖的同 VID/PID + 同 container：必须跳过。
    all.push_back(MakeIface(L"\\\\?\\hid#vid_5555&pid_0666&mi_01&col01#9&e&0&0000", 0x5555,
                            0x0666, L"Existing Mouse", 0x0001, 0x0002, HidBus::Usb,
                            existContainer));

    std::vector<Device> out;
    Device known;
    known.id = L"L:existing";
    known.name = L"Existing Mouse";
    known.product = known.name;
    known.proto = Proto::Logitech;
    known.conn = Conn::Wired;
    known.kind = Kind::Mouse;
    known.vid = 0x5555;
    known.pid = 0x0666;
    known.online = true;
    known.basePath = all.back().basePath;
    known.ifaces.push_back(all.back());
    out.push_back(known);

    proto::AddWiredHidDevices(all, out);

    CHECK(out.size() == 4);
    Device* mouse = FindByName(out, L"Acme Wireless Mouse");
    Device* keyboard = FindByName(out, L"Acme Keyboard");
    Device* bt = FindByName(out, L"Logitech MX Anywhere 3");
    CHECK(mouse != nullptr);
    CHECK(keyboard != nullptr);
    CHECK(bt != nullptr);
    CHECK(FindByName(out, L"USB Receiver") == nullptr);
    CHECK(FindByName(out, L"HID VHF Mouse") == nullptr);
    CHECK(FindByName(out, L"System Mouse") == nullptr);
    CHECK(FindByName(out, L"Mystery Mouse") == nullptr);

    int knownCount = 0;
    int genericMouseCount = 0;
    for (const auto& d : out) {
        if (d.vid == 0x5555 && d.pid == 0x0666) ++knownCount;
        if (d.vid == 0x1234 && d.pid == 0x0001) ++genericMouseCount;
    }
    CHECK(knownCount == 1);
    CHECK(genericMouseCount == 1);  // 三个集合只产生一个设备

    if (mouse) {
        CHECK(mouse->id == L"G:" + mouseContainer);
        CHECK(mouse->identity.empty());
        CHECK(mouse->proto == Proto::GenericHid);
        CHECK(mouse->conn == Conn::Wired);
        CHECK(mouse->kind == Kind::Mouse);
        CHECK(mouse->online);
        CHECK(!mouse->batteryValid);
        CHECK(mouse->battery == -1);
        CHECK(!mouse->charging);
        CHECK(!mouse->chargingKnown);
        CHECK(mouse->name == L"Acme Wireless Mouse");
        CHECK(mouse->product == L"Acme Wireless Mouse");
        CHECK(mouse->manufacturer == L"Acme");
        CHECK(mouse->path.find(L"col03") != std::wstring::npos);
        CHECK(mouse->ifaces.size() == 3);
        CHECK(!mouse->containerId.empty());
        CHECK(mouse->note == L"无线设备的有线模式 · 未读到电量通道");
    }
    if (keyboard) {
        CHECK(keyboard->kind == Kind::Keyboard);
        CHECK(keyboard->conn == Conn::Wired);
        CHECK(keyboard->proto == Proto::GenericHid);
        CHECK(keyboard->note == L"USB 有线设备 · 不提供电量（无电池）");
    }
    if (bt) {
        CHECK(bt->kind == Kind::Mouse);
        CHECK(bt->conn == Conn::Bluetooth);
        CHECK(bt->proto == Proto::GenericHid);
        CHECK(bt->note == L"蓝牙 HID 设备 · 电量由系统电池服务提供时才显示");
    }

    // 通用回退结果进入能力计算后：无线型号插线保留电量属性，纯有线/通用蓝牙不保留。
    FinalizeDeviceCapabilities(out);
    if (mouse) CHECK(mouse->batteryApplicable);
    if (keyboard) CHECK(!keyboard->batteryApplicable);
    if (bt) CHECK(!bt->batteryApplicable);
}

void TestHeadsetDevices() {
    // 1) Telephony 顶层集合 → 有线话务耳机
    {
        std::vector<HidInterface> ifaces;
        ifaces.push_back(MakeIface(
            L"\\\\?\\hid#vid_046d&pid_0a66&mi_03&col01#7&aaa&0&0000#{4d1e55b2}",
            0x046D, 0x0A66, L"G733 Gaming Headset", 0x000B, 0x0005, HidBus::Usb,
            L"AAAAAAAA-1111-2222-3333-444444444444"));
        std::vector<Device> out;
        proto::AddWiredHidDevices(ifaces, out);
        CHECK(out.size() == 1);
        CHECK(out[0].kind == Kind::Headset);
        CHECK(out[0].conn == Conn::Wired);
        CHECK(out[0].proto == Proto::GenericHid);
    }

    // 2) 只有 Consumer Control，但产品名明确是耳机 → 仍按耳机归类
    {
        std::vector<HidInterface> ifaces;
        ifaces.push_back(MakeIface(
            L"\\\\?\\hid#vid_1038&pid_12b3&col01#7&bbb&0&0000#{4d1e55b2}",
            0x1038, 0x12B3, L"Arctis Nova Headset", 0x000C, 0x0001, HidBus::Usb,
            L"BBBBBBBB-1111-2222-3333-444444444444"));
        std::vector<Device> out;
        proto::AddWiredHidDevices(ifaces, out);
        CHECK(out.size() == 1);
        CHECK(out[0].kind == Kind::Headset);
    }

    // 3) 纯 Consumer Control 且名字不像耳机（音箱）→ 不进入回退列表
    {
        std::vector<HidInterface> ifaces;
        ifaces.push_back(MakeIface(
            L"\\\\?\\hid#vid_1234&pid_5678&col01#7&ccc&0&0000#{4d1e55b2}",
            0x1234, 0x5678, L"Desktop Speaker", 0x000C, 0x0001, HidBus::Usb,
            L"CCCCCCCC-1111-2222-3333-444444444444"));
        std::vector<Device> out;
        proto::AddWiredHidDevices(ifaces, out);
        CHECK(out.empty());
    }

    // 4) 键盘 + Consumer Control（媒体键）→ 仍是键盘，不能误判成耳机
    {
        std::vector<HidInterface> ifaces;
        ifaces.push_back(MakeIface(
            L"\\\\?\\hid#vid_2f68&pid_0042&mi_00&col01#7&ddd&0&0000#{4d1e55b2}",
            0x2F68, 0x0042, L"Multimedia Keyboard", 0x0001, 0x0006, HidBus::Usb,
            L"DDDDDDDD-1111-2222-3333-444444444444"));
        ifaces.push_back(MakeIface(
            L"\\\\?\\hid#vid_2f68&pid_0042&mi_00&col02#7&ddd&0&0001#{4d1e55b2}",
            0x2F68, 0x0042, L"Multimedia Keyboard", 0x000C, 0x0001, HidBus::Usb,
            L"DDDDDDDD-1111-2222-3333-444444444444"));
        std::vector<Device> out;
        proto::AddWiredHidDevices(ifaces, out);
        CHECK(out.size() == 1);
        CHECK(out[0].kind == Kind::Keyboard);
    }

    // 5) 蓝牙耳机：Telephony 集合 + BT 传输
    {
        std::vector<HidInterface> ifaces;
        ifaces.push_back(MakeIface(
            L"\\\\?\\HID#BTHENUM#{00001124-0000-1000-8000-00805f9b34fb}_VID&0002046d_PID&0a66"
            L"&Col01#8&eee&0&0000#{4d1e55b2}",
            0x046D, 0x0A66, L"WH-1000XM5 Headset", 0x000B, 0x0005, HidBus::Bluetooth,
            L"EEEEEEEE-1111-2222-3333-444444444444"));
        std::vector<Device> out;
        proto::AddWiredHidDevices(ifaces, out);
        CHECK(out.size() == 1);
        CHECK(out[0].kind == Kind::Headset);
        CHECK(out[0].conn == Conn::Bluetooth);
    }
}

// 已知协议的 Device.ifaces 在生产路径里是“全部接口”（ifaces=all）。旧实现
// 拿 d.ifaces 去匹配 group.containerId，任何同 VID/PID 的组都会命中自身接口，
// 从而把另一台同型号设备误跳过。这里用两台不同 container 的同型号设备验证：
// 与已知设备同 basePath 的那台跳过，另一台必须保留。
void TestAddWiredKeepsOtherContainer() {
    const std::wstring pathA =
        L"\\\\?\\hid#vid_7777&pid_0888&mi_01&col01#7&aaa&0&0000";
    const std::wstring pathB =
        L"\\\\?\\hid#vid_7777&pid_0888&mi_01&col01#7&bbb&0&0000";
    std::vector<HidInterface> all;
    all.push_back(MakeIface(pathA, 0x7777, 0x0888, L"Acme Mouse A", 0x0001, 0x0002,
                            HidBus::Usb, L"11111111-1111-1111-1111-111111111111"));
    all.push_back(MakeIface(pathB, 0x7777, 0x0888, L"Acme Mouse B", 0x0001, 0x0002,
                            HidBus::Usb, L"22222222-2222-2222-2222-222222222222"));

    Device known;
    known.id = L"L:known";
    known.name = L"Acme Mouse A";
    known.product = known.name;
    known.proto = Proto::Logitech;
    known.conn = Conn::Wired;
    known.kind = Kind::Mouse;
    known.vid = 0x7777;
    known.pid = 0x0888;
    known.online = true;
    known.basePath = all[0].basePath;
    known.ifaces = all;  // 与生产协议一致：已知设备持有全部接口
    std::vector<Device> out{known};

    proto::AddWiredHidDevices(all, out);
    CHECK(out.size() == 2);
    CHECK(FindByName(out, L"Acme Mouse A") == &out[0]);
    CHECK(FindByName(out, L"Acme Mouse B") != nullptr);
}

// 蓝牙音频分类：Class of Device Major=Audio/Video；Minor=Headset/Hands-free/
// Headphones 按耳机，其它音频（音箱）仍列出但不标耳机。
void TestBluetoothAudioClassification() {
    CHECK(BluetoothClassIsAudio(0x244404));   // Galaxy Buds4 Pro
    CHECK(BluetoothClassIsAudio(0x240404));   // WH-1000XM4
    CHECK(!BluetoothClassIsAudio(0x5A020C));  // 手机（Major=Phone）
    CHECK(!BluetoothClassIsAudio(0x000000));

    CHECK(BluetoothAudioLooksLikeHeadset(L"煜 的 Buds4 Pro", 0x244404));
    CHECK(BluetoothAudioLooksLikeHeadset(L"WH-1000XM4", 0x240404));
    CHECK(BluetoothAudioLooksLikeHeadset(L"Galaxy Buds", 0x040400));
    // Major=Audio、Minor=Speaker（5），名称也不含耳机关键词。
    CHECK(!BluetoothAudioLooksLikeHeadset(L"JBL Flip 6", 0x040414));
}

BluetoothAudioDevice MakeBtAudio(const std::wstring& address, const std::wstring& name,
                                 uint32_t cod, bool headset) {
    BluetoothAudioDevice b;
    b.address = address;
    b.name = name;
    b.classOfDevice = cod;
    b.headset = headset;
    return b;
}

// 无 HID、无电池节点的经典蓝牙耳机必须进入列表，且不得伪造电量。
void TestAddBluetoothAudioDevices() {
    std::vector<BluetoothAudioDevice> all{
        MakeBtAudio(L"78C11D6C1B0E", L"煜 的 Buds4 Pro", 0x244404, true),
        MakeBtAudio(L"AC800AF75F9D", L"WH-1000XM4", 0x240404, true),
        MakeBtAudio(L"112233445566", L"JBL Flip 6", 0x040414, false)};

    std::vector<Device> out;
    proto::AddBluetoothAudioDevices(all, out);
    CHECK(out.size() == 3);

    Device* buds = FindByName(out, L"煜 的 Buds4 Pro");
    Device* xm4 = FindByName(out, L"WH-1000XM4");
    Device* speaker = FindByName(out, L"JBL Flip 6");
    CHECK(buds != nullptr);
    CHECK(xm4 != nullptr);
    CHECK(speaker != nullptr);
    if (buds) {
        CHECK(buds->id == L"BT:78C11D6C1B0E");
        CHECK(buds->proto == Proto::BluetoothAudio);
        CHECK(buds->conn == Conn::Bluetooth);
        CHECK(buds->kind == Kind::Headset);
        CHECK(buds->online);
        CHECK(!buds->batteryValid);
        CHECK(buds->battery == -1);
        CHECK(!buds->chargingKnown);
        CHECK(buds->note.find(L"Windows 未提供电量") != std::wstring::npos);
        CHECK(!DeviceSupportsWrites(*buds));
    }
    if (speaker) CHECK(speaker->kind == Kind::Other);

    // 蓝牙音频即使 conn=Bluetooth 也不能进入“等待电量”状态。
    FinalizeDeviceCapabilities(out);
    if (buds) {
        CHECK(buds->wirelessCapable);
        CHECK(!buds->batteryApplicable);
        CHECK(!buds->chargingKnown);
    }
    if (speaker) CHECK(!speaker->batteryApplicable);
}

// 同一物理设备已由 HID 路径列出时，音频条目不能重复；地址 id 也必须稳定去重。
void TestBluetoothAudioDedupe() {
    BluetoothAudioDevice buds =
        MakeBtAudio(L"78C11D6C1B0E", L"煜 的 Buds4 Pro", 0x244404, true);

    {
        std::vector<Device> out;
        Device hid = MakeDevice(Proto::GenericHid, Conn::Bluetooth, L"Buds4 Pro", false);
        hid.id = L"G:some-hid-container";
        out.push_back(hid);
        proto::AddBluetoothAudioDevices({buds}, out);
        CHECK(out.size() == 1);
        CHECK(out[0].id == L"G:some-hid-container");
    }
    {
        std::vector<Device> out;
        Device sameAddress =
            MakeDevice(Proto::BluetoothAudio, Conn::Bluetooth, L"Buds4 Pro", false);
        sameAddress.id = L"BT:78C11D6C1B0E";
        out.push_back(sameAddress);
        proto::AddBluetoothAudioDevices({buds}, out);
        CHECK(out.size() == 1);
    }
}

// Windows 蓝牙设备属性若给出电量（0..100），必须计入 batteryValid/电量事件；
// 没有属性时保持 -1，不能伪造。
void TestBluetoothAudioBattery() {
    BluetoothAudioDevice b =
        MakeBtAudio(L"78C11D6C1B0E", L"煜 的 Buds4 Pro", 0x244404, true);
    b.battery = 42;
    std::vector<Device> out;
    proto::AddBluetoothAudioDevices({b}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].batteryValid);
    CHECK(out[0].battery == 42);
    CHECK(!out[0].chargingKnown);
    CHECK(out[0].note.find(L"Windows 蓝牙属性") != std::wstring::npos);
    FinalizeDeviceCapabilities(out);
    CHECK(out[0].batteryApplicable);  // 有真实读数时进入电量卡/日志/曲线
    CHECK(out[0].wirelessCapable);
}

std::vector<uint8_t> HexToBytes(const char* text) {
    std::vector<uint8_t> out;
    int high = -1;
    for (const char* p = text; *p; ++p) {
        int value = -1;
        if (*p >= '0' && *p <= '9') value = *p - '0';
        else if (*p >= 'a' && *p <= 'f') value = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') value = *p - 'A' + 10;
        else continue;
        if (high < 0) {
            high = value;
        } else {
            out.push_back((uint8_t)((high << 4) | value));
            high = -1;
        }
    }
    return out;
}

std::vector<uint8_t> MakeBudsFrame(uint8_t msgId, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body;
    body.push_back(msgId);
    body.insert(body.end(), payload.begin(), payload.end());
    const uint16_t crc = SamsungBudsCrc16(body.data(), body.size());
    body.push_back((uint8_t)(crc & 0xFF));
    body.push_back((uint8_t)((crc >> 8) & 0xFF));

    std::vector<uint8_t> frame;
    frame.push_back(0xFD);
    frame.push_back((uint8_t)(body.size() & 0xFF));
    frame.push_back((uint8_t)((body.size() >> 8) & 0x03FF));
    frame.insert(frame.end(), body.begin(), body.end());
    frame.push_back(0xDD);
    return frame;
}

std::vector<uint8_t> MakeExtendedPayload(int left, int right, int casePct, uint8_t charging,
                                         bool withChargingByte = true) {
    std::vector<uint8_t> payload(withChargingByte ? 62 : 42, 0);
    payload[0] = 4;
    payload[1] = 0x0D;
    payload[2] = (uint8_t)left;
    payload[3] = (uint8_t)right;
    payload[7] = (uint8_t)casePct;
    if (withChargingByte) payload[43] = charging;
    return payload;
}

// 真实抓包帧：Buds4 Pro 左右耳 100%，充电仓 0，未充电。CRC = 0xD2B4。
void TestSamsungBudsFrameParse() {
    const auto frame = HexToBytes(
        "FD410061040D6464010112000004FF2200006701670109000499000401"
        "10000000001102000100000000000000000000000000000101000000FF"
        "0103000000000001B4D2DD");
    CHECK(frame.size() == 69);

    size_t consumed = 0;
    SamsungBudsStatus status;
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size(), consumed, status) ==
          SamsungFrameParse::Status);
    CHECK(consumed == frame.size());
    CHECK(status.batteryLeft == 100);
    CHECK(status.batteryRight == 100);
    CHECK(status.batteryCase == -1);  // payload[7] == 0 视为不可用
    CHECK(status.chargingKnown);
    CHECK(!status.chargingLeft);
    CHECK(!status.chargingRight);
    CHECK(SamsungBudsDeviceBattery(status) == 100);

    // 截断帧必须继续等待，不能产生读数。
    SamsungBudsStatus truncated;
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size() - 1, consumed, truncated) ==
          SamsungFrameParse::NeedMore);
    CHECK(consumed == 0);

    // CRC 错 1 字节：整帧丢弃。
    auto badCrc = frame;
    badCrc[badCrc.size() - 2] ^= 0x01;
    CHECK(ParseSamsungBudsFrame(badCrc.data(), badCrc.size(), consumed, truncated) ==
          SamsungFrameParse::Skipped);
    CHECK(consumed == 1);

    // EOM 错误：整帧丢弃。
    auto badEom = frame;
    badEom.back() = 0x00;
    CHECK(ParseSamsungBudsFrame(badEom.data(), badEom.size(), consumed, truncated) ==
          SamsungFrameParse::Skipped);

    // 非电量帧：整帧跳过后继续。
    const auto other = MakeBudsFrame(0x40, {0x01, 0x02, 0x03});
    CHECK(ParseSamsungBudsFrame(other.data(), other.size(), consumed, truncated) ==
          SamsungFrameParse::Other);
    CHECK(consumed == other.size());

    // 前导垃圾字节：逐字节重同步，最终仍能读到状态帧。
    std::vector<uint8_t> stream{0x00, 0x11, 0x22};
    stream.insert(stream.end(), frame.begin(), frame.end());
    size_t offset = 0;
    bool found = false;
    while (offset < stream.size()) {
        SamsungBudsStatus parsed;
        const auto result = ParseSamsungBudsFrame(stream.data() + offset,
                                                  stream.size() - offset, consumed, parsed);
        if (result == SamsungFrameParse::NeedMore) break;
        if (result == SamsungFrameParse::Status) {
            found = true;
            CHECK(parsed.batteryLeft == 100);
            break;
        }
        offset += consumed ? consumed : 1;
    }
    CHECK(found);
}

void TestSamsungBudsBatterySelection() {
    SamsungBudsStatus both;
    both.batteryLeft = 100;
    both.batteryRight = 100;
    CHECK(SamsungBudsDeviceBattery(both) == 100);

    SamsungBudsStatus oneEmpty;
    oneEmpty.batteryLeft = 0;
    oneEmpty.batteryRight = 80;
    CHECK(SamsungBudsDeviceBattery(oneEmpty) == 80);

    SamsungBudsStatus oneEmptyRight;
    oneEmptyRight.batteryLeft = 80;
    oneEmptyRight.batteryRight = 0;
    CHECK(SamsungBudsDeviceBattery(oneEmptyRight) == 80);

    SamsungBudsStatus bothEmpty;
    bothEmpty.batteryLeft = 0;
    bothEmpty.batteryRight = 0;
    CHECK(SamsungBudsDeviceBattery(bothEmpty) == -1);

    bothEmpty.chargingKnown = true;
    bothEmpty.chargingLeft = true;
    CHECK(SamsungBudsDeviceBattery(bothEmpty) == 0);

    SamsungBudsStatus single;
    single.batteryLeft = -1;
    single.batteryRight = 55;
    CHECK(SamsungBudsDeviceBattery(single) == 55);

    SamsungBudsStatus invalid;
    invalid.batteryLeft = 101;
    invalid.batteryRight = 50;
    CHECK(SamsungBudsDeviceBattery(invalid) == 50);
}

void TestSamsungBudsChargingFlags() {
    size_t consumed = 0;

    SamsungBudsStatus left;
    auto frame = MakeBudsFrame(97, MakeExtendedPayload(50, 40, 90, 0x10));
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size(), consumed, left) ==
          SamsungFrameParse::Status);
    CHECK(left.chargingKnown);
    CHECK(left.chargingLeft);
    CHECK(!left.chargingRight);
    CHECK(!left.chargingCase);

    SamsungBudsStatus right;
    frame = MakeBudsFrame(97, MakeExtendedPayload(50, 40, 90, 0x04));
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size(), consumed, right) ==
          SamsungFrameParse::Status);
    CHECK(right.chargingRight);
    CHECK(!right.chargingLeft);

    SamsungBudsStatus caseOnly;
    frame = MakeBudsFrame(97, MakeExtendedPayload(50, 40, 90, 0x01));
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size(), consumed, caseOnly) ==
          SamsungFrameParse::Status);
    CHECK(caseOnly.chargingCase);
    CHECK(!caseOnly.chargingLeft);

    // 没有充电位时必须是“未知”，不能当成未充电。
    SamsungBudsStatus noFlags;
    frame = MakeBudsFrame(97, MakeExtendedPayload(50, 40, 90, 0, false));
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size(), consumed, noFlags) ==
          SamsungFrameParse::Status);
    CHECK(!noFlags.chargingKnown);
}

void TestSamsungBudsStatusUpdatedFallback() {
    const auto frame = MakeBudsFrame(96, {0x01, 55, 44, 0x00, 0x00, 0x00});
    size_t consumed = 0;
    SamsungBudsStatus status;
    CHECK(ParseSamsungBudsFrame(frame.data(), frame.size(), consumed, status) ==
          SamsungFrameParse::Status);
    CHECK(status.batteryLeft == 55);
    CHECK(status.batteryRight == 44);
    CHECK(status.batteryCase == -1);
    CHECK(!status.chargingKnown);
    CHECK(SamsungBudsDeviceBattery(status) == 44);
}

void TestSamsungBudsSppDeviceFields() {
    BluetoothAudioDevice b =
        MakeBtAudio(L"78C11D6C1B0E", L"煜 的 Buds4 Pro", 0x244404, true);
    b.battery = 73;
    b.batterySource = BluetoothBatterySource::SamsungSpp;
    b.chargingKnown = true;
    b.charging = false;
    std::vector<Device> out;
    proto::AddBluetoothAudioDevices({b}, out);
    CHECK(out.size() == 1);
    CHECK(out[0].batteryValid);
    CHECK(out[0].battery == 73);
    CHECK(out[0].chargingKnown);
    CHECK(!out[0].charging);
    CHECK(out[0].note.find(L"私有协议") != std::wstring::npos);
    FinalizeDeviceCapabilities(out);
    CHECK(out[0].batteryApplicable);
}

}  // namespace

int main() {
    TestLooksWirelessProduct();
    TestKnownWirelessIdentity();
    TestFinalizeWiredOnly();
    TestFinalizeWiredWirelessName();
    TestFinalizeWirelessNoReading();
    TestFinalizeGenericBluetooth();
    TestFinalizeKnownBluetooth();
    TestFinalizeSystemBattery();
    TestFinalizeWiredHeadsetStaysWired();
    TestDeviceConnLabel();
    TestAddWiredDevices();
    TestAddWiredKeepsOtherContainer();
    TestHeadsetDevices();
    TestBluetoothAudioClassification();
    TestAddBluetoothAudioDevices();
    TestBluetoothAudioDedupe();
    TestBluetoothAudioBattery();
    TestSamsungBudsFrameParse();
    TestSamsungBudsBatterySelection();
    TestSamsungBudsChargingFlags();
    TestSamsungBudsStatusUpdatedFallback();
    TestSamsungBudsSppDeviceFields();
    std::cout << "wired_devices: 22 tests, " << g_checks << " checks passed\n";
    return 0;
}
