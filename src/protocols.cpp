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

std::wstring FriendlyProtocolName(const Device& d) {
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
