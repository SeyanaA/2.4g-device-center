#include "protocols.h"

const wchar_t* ProtoName(Proto p) {
    switch (p) {
        case Proto::Logitech: return L"Logitech HID++";
        case Proto::Asus: return L"ASUS ROG HID";
        case Proto::Razer: return L"Razer HID";
        case Proto::Rapoo: return L"Rapoo 2.4G";
        case Proto::Compx: return L"Compx 公版";
        case Proto::Nordic54: return L"Nordic 54";
        case Proto::Aurora: return L"Aurora / mouse.xyz";
        case Proto::Cherry: return L"Cherry HID";
        case Proto::Lofree: return L"Lofree WebHID";
        case Proto::Valkyrie: return L"Valkyrie HID";
        case Proto::MoreBetter: return L"More&Better 2.4G";
        case Proto::GenericHid: return L"USB / 蓝牙 HID（通用）";
        case Proto::BluetoothAudio: return L"蓝牙音频";
        case Proto::SystemBattery: return L"Windows HID 电池";
        default: return L"未知";
    }
}

const wchar_t* ConnName(Conn c) {
    switch (c) {
        case Conn::Wireless24: return L"2.4G 无线";
        case Conn::Wired: return L"USB 有线";
        case Conn::Bluetooth: return L"蓝牙";
        default: return L"未知连接";
    }
}

const wchar_t* KindName(Kind k) {
    switch (k) {
        case Kind::Mouse: return L"鼠标";
        case Kind::Keyboard: return L"键盘";
        case Kind::Headset: return L"耳机";
        case Kind::Other: return L"设备";
        default: return L"设备";
    }
}

std::wstring DeviceConnLabel(const Device& d) {
    // 物理链路是 USB，但设备本身是无线型号时，必须与真正的有线设备区分开。
    if (d.conn == Conn::Wired && d.wirelessCapable) return L"USB 有线 · 无线模式";
    return ConnName(d.conn);
}

std::wstring FriendlyProtocolName(const Device& d) {
    if (d.proto == Proto::GenericHid) {
        // 通用回退按连接方式细分名称，避免 UI 上显示带两种链路的括号说明。
        return d.conn == Conn::Bluetooth ? L"蓝牙 HID（通用）" : L"USB HID（通用）";
    }
    if (d.proto == Proto::Valkyrie) return L"Valkyrie VK M3";
    if (d.proto == Proto::MoreBetter) return L"More&Better 2.4G";
    if (d.proto == Proto::Compx) {
        if (d.vid == 0x3554) return L"Compx / Zaopin 公版";
        if (d.vid == 0x373B) return L"Compx / ATK-VXE 公版";
        if (d.vid == 0x3367) return L"Compx / Endgame 公版";
        if (d.vid == 0x33E4) return L"Compx / G-Wolves 公版";
        if (d.vid == 0x25A7) return L"Compx / Redragon 公版";
    }
    return ProtoName(d.proto);
}

bool DeviceSupportsWrites(const Device& d) {
    return d.dpi.canWrite || d.polling.canWrite || d.debounce.canWrite || d.angle.canWrite;
}

namespace proto {

bool LooksWirelessProduct(const std::wstring& text) {
    const std::wstring lower = Lower(text);
    if (lower.empty()) return false;
    // 接收器/Dongle 本身不是无线型号：即使名字里带 2.4G，也不能据此判定设备
    // 具备无线能力，否则接收器会被当成“无线设备插线”。
    const wchar_t* exclude[] = {L"receiver", L"dongle", L"接收器"};
    for (const wchar_t* e : exclude) {
        if (lower.find(e) != std::wstring::npos) return false;
    }
    const wchar_t* keywords[] = {L"wireless", L"2.4g",    L"2.4 g",   L"dual mode",
                                 L"dual-mode", L"tri-mode", L"三模",  L"双模",
                                 L"无线",      L"蓝牙",    L"bluetooth", L"lightspeed"};
    for (const wchar_t* k : keywords) {
        if (lower.find(k) != std::wstring::npos) return true;
    }
    return false;
}

bool IsKnownWirelessIdentity(const std::wstring& identity) {
    const std::wstring id = Lower(identity);
    return id.rfind(L"zaopin:", 0) == 0 || id.rfind(L"valkyrie:", 0) == 0;
}

void FinalizeDeviceCapabilities(std::vector<Device>& devices) {
    for (auto& d : devices) {
        // 判定优先级见 docs/SPEC-transport-wired-bluetooth.md §1.3：
        // 不能仅凭“当前走 USB”就判定为有线设备。
        // 有线耳机例外：Windows 电池服务能给它补电量，但“有电量”不代表它是无线型号，
        // 否则会把 USB 有线耳机标成「USB 有线 · 无线模式」。
        const bool wiredHeadset = d.conn == Conn::Wired && d.kind == Kind::Headset;
        d.wirelessCapable = d.conn != Conn::Wired ||
                            (!wiredHeadset && d.batteryValid) ||
                            IsKnownWirelessIdentity(d.identity) ||
                            LooksWirelessProduct(d.name + L" " + d.product);
        d.batteryApplicable = d.batteryValid || d.proto == Proto::SystemBattery ||
                              (d.proto != Proto::GenericHid &&
                               d.proto != Proto::BluetoothAudio && d.conn != Conn::Wired) ||
                              (d.conn == Conn::Wired && d.wirelessCapable);
        // 不适用电池的设备不保留充电状态，避免 UI/日志显示“未知充电”之类的残留。
        if (!d.batteryApplicable) d.chargingKnown = false;
    }
}

}  // namespace proto
