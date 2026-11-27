#include "protocols.h"

#include <initializer_list>
#include <map>
#include <utility>

namespace {

bool IsMouseUsage(const HidInterface& h) {
    return h.usagePage == 0x0001 && h.usage == 0x0002;
}

bool IsKeyboardUsage(const HidInterface& h) {
    return h.usagePage == 0x0001 && h.usage == 0x0006;
}

bool ContainsAnyNoCase(const std::wstring& text, std::initializer_list<const wchar_t*> needles) {
    for (const wchar_t* n : needles) {
        if (ContainsNoCase(text, n)) return true;
    }
    return false;
}

// 耳机顶层集合：Telephony 页（话务耳机）或 Consumer 页的 Headphone(0x05)。
// 注意：Consumer 0x06 是 Graphic Equalizer，不是 Headphone；
// 也不能只看 Consumer Control(0x0C/0x01)——键盘的媒体键也用这个集合。
bool IsHeadsetUsage(const HidInterface& h) {
    if (h.usagePage == 0x000B) return true;  // Telephony
    if (h.usagePage == 0x000C && h.usage == 0x0005) return true;
    return false;
}

bool LooksLikeHeadsetName(const std::wstring& text) {
    return ContainsAnyNoCase(text, {L"headset", L"headphone", L"earphone", L"earbud",
                                    L"耳机", L"耳麦", L"头戴"});
}

struct WiredGroup {
    std::wstring key;
    std::wstring containerId;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::wstring product;
    std::wstring manufacturer;
    std::vector<const HidInterface*> ifaces;
    bool hasMouse = false;
    bool hasKeyboard = false;
    bool hasHeadset = false;
    bool hasConsumer = false;
    bool virtualPath = false;
};

// 同一物理设备的多个 HID 集合（col01/col02/...）共享 containerId；没有
// containerId 时退化为 vid:pid:小写 product，仍然能把集合合并成一个条目。
std::wstring GroupKey(const HidInterface& h) {
    // Windows 在取不到 ContainerId 时会给出全 0/全 F 的占位值，不能当成真实容器。
    const bool placeholder = h.containerId.empty() ||
                             h.containerId.rfind(L"00000000-0000-0000-", 0) == 0;
    if (!placeholder) return h.containerId;
    return Fmt(L"%04x:%04x:%s", (unsigned)h.vid, (unsigned)h.pid, Lower(h.product).c_str());
}

// 代表性接口优先选厂商页（usagePage >= 0xFF00）中报文最长的那个，便于后续
// 读取/写入路由；没有厂商页时退回鼠标/键盘顶层集合。
const HidInterface* PickRepresentative(const WiredGroup& g) {
    const HidInterface* best = nullptr;
    int bestScore = -1;
    for (const HidInterface* h : g.ifaces) {
        if (h->usagePage < 0xFF00) continue;
        const int score = (int)h->outLen + (int)h->inLen;
        if (!best || score > bestScore) {
            best = h;
            bestScore = score;
        }
    }
    if (best) return best;
    for (const HidInterface* h : g.ifaces) {
        if (IsMouseUsage(*h)) return h;
    }
    for (const HidInterface* h : g.ifaces) {
        if (IsKeyboardUsage(*h)) return h;
    }
    return g.ifaces.empty() ? nullptr : g.ifaces.front();
}

HidBus ResolveBus(const WiredGroup& g, const HidInterface& rep) {
    if (rep.bus != HidBus::Unknown) return rep.bus;
    for (const HidInterface* h : g.ifaces) {
        if (h->bus != HidBus::Unknown) return h->bus;
    }
    return HidBus::Unknown;
}

// 与已知协议去重：同 vid+pid 且（container 相同 / basePath 相同 / 名称互相包含）
// 视为同一物理设备，通用回退不再重复列出。
bool SharesPhysicalDevice(const Device& d, const WiredGroup& g, const HidInterface& rep,
                          const std::wstring& name) {
    if (d.vid != g.vid || d.pid != g.pid) return false;
    const bool groupContainer =
        !g.containerId.empty() && g.containerId.rfind(L"00000000-0000-0000-", 0) != 0;
    const bool devContainer = !d.containerId.empty();
    // 已知协议的 Device.containerId 通常为空；不能拿 d.ifaces 里的容器来比对，
    // 因为已知协议把 d.ifaces 设成“本次枚举的全部接口”，组自身的接口必然在
    // 其中，会让任意同 VID/PID 的真实设备都被误判为已存在。
    // 只有双方都有真实容器时才用容器做最终裁决（不同容器 = 不同物理设备）。
    if (!d.containerId.empty() && groupContainer) {
        return _wcsicmp(d.containerId.c_str(), g.containerId.c_str()) == 0;
    }
    if (!d.basePath.empty()) {
        if (d.basePath == rep.basePath) return true;
        for (const HidInterface* h : g.ifaces) {
            if (!h->basePath.empty() && h->basePath == d.basePath) return true;
        }
    }
    // 没有容器/路径可比较时才退回名称；仅在双方都缺少容器时使用，避免误合并同型号。
    if (!devContainer && !groupContainer && !d.name.empty() && !name.empty() &&
        (ContainsNoCase(d.name, name.c_str()) || ContainsNoCase(name, d.name.c_str()))) {
        return true;
    }
    return false;
}

}  // namespace

namespace proto {

void AddWiredHidDevices(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    std::map<std::wstring, WiredGroup> groups;
    for (const auto& h : all) {
        const std::wstring key = GroupKey(h);
        auto [it, inserted] = groups.try_emplace(key);
        WiredGroup& g = it->second;
        if (inserted) {
            g.key = key;
            g.containerId = h.containerId;
            g.vid = h.vid;
            g.pid = h.pid;
        }
        // 某些接口的 HID 属性读取失败时 vid/pid 为 0，用组内第一个有效值补上。
        if ((g.vid == 0 || g.pid == 0) && h.vid != 0) {
            g.vid = h.vid;
            g.pid = h.pid;
        }
        if (g.product.empty() && !h.product.empty()) g.product = h.product;
        if (g.manufacturer.empty() && !h.manufacturer.empty()) g.manufacturer = h.manufacturer;
        if (IsMouseUsage(h)) g.hasMouse = true;
        if (IsKeyboardUsage(h)) g.hasKeyboard = true;
        if (IsHeadsetUsage(h)) g.hasHeadset = true;
        if (h.usagePage == 0x000C) g.hasConsumer = true;
        if (ContainsNoCase(h.path, L"hid_device_system_vhf")) g.virtualPath = true;
        g.ifaces.push_back(&h);
    }

    // 很多耳机只有 Consumer Control 集合、没有 Telephony 集合，靠产品名确认，
    // 避免把键盘的媒体键集合误判成耳机。
    for (auto& [key, g] : groups) {
        (void)key;
        if (!g.hasHeadset && g.hasConsumer && LooksLikeHeadsetName(g.product)) {
            g.hasHeadset = true;
        }
    }

    for (const auto& [key, g] : groups) {
        (void)key;
        // 鼠标/键盘/耳机都进入回退；纯多媒体/消费者控制（音箱、单个媒体键）仍不列出。
        if (!g.hasMouse && !g.hasKeyboard && !g.hasHeadset) continue;
        // 排除系统虚拟 HID（HID VHF / Root / Virtual）与接收器/Dongle 本身。
        if (g.vid == 0x5E2A || g.virtualPath) continue;
        if (ContainsAnyNoCase(g.product, {L"hid vhf", L"virtual", L"虚拟", L"root"})) continue;
        if (ContainsAnyNoCase(g.product, {L"receiver", L"dongle", L"接收器"})) continue;
        // 复合“键盘+鼠标”且产品名只写 Wireless 的通常是接收器，不是设备本体。
        if (g.hasMouse && g.hasKeyboard && ContainsNoCase(g.product, L"wireless")) continue;

        const HidInterface* rep = PickRepresentative(g);
        if (!rep) continue;
        const HidBus bus = ResolveBus(g, *rep);
        if (bus == HidBus::Unknown) continue;
        const Conn conn = bus == HidBus::Bluetooth ? Conn::Bluetooth : Conn::Wired;

        std::wstring name = g.product;
        if (name.empty()) {
            name = g.manufacturer +
                   (g.hasMouse ? L"鼠标" : (g.hasKeyboard ? L"键盘" : L"耳机"));
        }

        bool existing = false;
        for (const auto& d : out) {
            if (SharesPhysicalDevice(d, g, *rep, name)) {
                existing = true;
                break;
            }
        }
        if (existing) continue;

        Device d;
        d.id = L"G:" + g.key;
        // identity 留空：未知设备不做跨传输合并，已知机型由原协议设置 identity。
        d.name = name;
        d.product = g.product;
        d.manufacturer = g.manufacturer;
        d.proto = Proto::GenericHid;
        d.conn = conn;
        // 鼠标 > 键盘 > 耳机：耳机也可能暴露消费者/话务集合，只有两者都没有时才按耳机归类。
        d.kind = g.hasMouse ? Kind::Mouse : (g.hasKeyboard ? Kind::Keyboard : Kind::Headset);
        // 键盘常额外暴露一个鼠标集合（多媒体/滚轮）；产品名没有明确写鼠标时按键盘归类。
        if (g.hasKeyboard) {
            const bool productMouse = ContainsNoCase(g.product, L"mouse") ||
                                      ContainsNoCase(g.product, L"鼠标");
            if (!g.hasMouse || !productMouse) d.kind = Kind::Keyboard;
        }
        d.vid = g.vid;
        d.pid = g.pid;
        d.online = true;
        d.battery = -1;
        d.batteryValid = false;
        d.charging = false;
        d.chargingKnown = false;
        d.path = rep->path;
        d.basePath = rep->basePath;
        const bool realContainer =
            !g.containerId.empty() && g.containerId.rfind(L"00000000-0000-0000-", 0) != 0;
        d.containerId = realContainer ? g.containerId : std::wstring();
        d.ifaces.clear();
        d.ifaces.reserve(g.ifaces.size());
        for (const HidInterface* h : g.ifaces) d.ifaces.push_back(*h);
        if (conn == Conn::Bluetooth) {
            d.note = L"蓝牙 HID 设备 · 电量由系统电池服务提供时才显示";
        } else if (LooksWirelessProduct(name)) {
            d.note = L"无线设备的有线模式 · 未读到电量通道";
        } else {
            d.note = L"USB 有线设备 · 不提供电量（无电池）";
        }
        out.push_back(std::move(d));
    }
}

void AddBluetoothAudioDevices(const std::vector<BluetoothAudioDevice>& all,
                              std::vector<Device>& out) {
    for (const auto& b : all) {
        // 同一物理设备已由 HID 路径列出时不要重复：
        // 音频链路没有 HID 集合，用稳定地址 id 或名称互含兜底。
        const std::wstring id = L"BT:" + b.address;
        bool existing = false;
        for (const auto& d : out) {
            if (_wcsicmp(d.id.c_str(), id.c_str()) == 0) {
                existing = true;
                break;
            }
            if (d.name.size() >= 3 && b.name.size() >= 3 &&
                (ContainsNoCase(d.name, b.name.c_str()) ||
                 ContainsNoCase(b.name, d.name.c_str()))) {
                existing = true;
                break;
            }
        }
        if (existing) continue;

        Device d;
        d.id = id;
        d.name = b.name;
        d.product = b.name;
        d.manufacturer = L"蓝牙音频";
        d.proto = Proto::BluetoothAudio;
        d.conn = Conn::Bluetooth;
        d.kind = b.headset ? Kind::Headset : Kind::Other;
        d.online = true;
        d.battery = b.battery;
        d.batteryValid = b.battery >= 0 && b.battery <= 100;
        d.charging = false;
        d.chargingKnown = false;
        // 经典蓝牙音频没有 HID/电池节点；没有真实读数时不要编造。
        d.note = L"蓝牙音频设备 · Windows 未提供电量读数（无 HID/电池节点）";
        if (d.batteryValid) {
            if (b.batterySource == BluetoothBatterySource::SamsungSpp) {
                // 电量和充电位都来自耳机自己的状态帧；没有充电位时保持未知。
                d.note = L"蓝牙音频设备 · 电量来自耳机私有协议（只读）";
                d.chargingKnown = b.chargingKnown;
                d.charging = b.chargingKnown && b.charging;
            } else {
                d.note = L"蓝牙音频设备 · 电量来自 Windows 蓝牙属性";
            }
        }
        out.push_back(std::move(d));
    }
}

}  // namespace proto
