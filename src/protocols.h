#pragma once

#include <string>
#include <vector>

#include "hid.h"

enum class Proto {
    None = 0,
    Logitech,
    Asus,
    Razer,
    Rapoo,
    Compx,
    Nordic54,
    Aurora,
    Cherry,
    Lofree,
    Valkyrie,
    MoreBetter,
    GenericHid,
    BluetoothAudio,
    SystemBattery,
};

enum class Conn { Unknown = 0, Wireless24, Wired, Bluetooth };
enum class Kind { Unknown = 0, Mouse, Keyboard, Headset, Other };

const wchar_t* ProtoName(Proto p);
const wchar_t* ConnName(Conn c);
const wchar_t* KindName(Kind k);

struct DpiInfo {
    bool valid = false;
    bool currentKnown = true;
    bool canWrite = false;
    int x = 0;
    int y = 0;
    int lod = 0;
    int min = 100;
    int max = 30000;
    int step = 50;
    std::vector<int> stages;
    int activeStage = -1;
    std::wstring note;
};

struct PollingInfo {
    bool valid = false;
    bool canWrite = false;
    int currentHz = 0;
    std::vector<int> options;
};

struct DebounceInfo {
    bool valid = false;
    bool canWrite = false;
    int currentMs = 0;
    std::vector<int> options;
};

struct AngleInfo {
    bool valid = false;
    bool canWrite = false;
    bool on = false;
};

struct Device {
    // Transport-specific id used for selection and write routing.
    std::wstring id;
    // Stable key shared by transports of the same physical device (for example
    // the 2.4G receiver and the USB charging cable of one mouse). Empty means
    // the device is only known by id.
    std::wstring identity;
    std::wstring name;
    std::wstring product;
    std::wstring manufacturer;
    uint16_t vid = 0;
    uint16_t pid = 0;
    Proto proto = Proto::None;
    Conn conn = Conn::Unknown;
    Kind kind = Kind::Unknown;

    bool online = false;
    bool stale = false;
    std::wstring note;

    int battery = -1;
    bool charging = false;
    // False when the protocol has no charging flag (e.g. More&Better 2.4G).
    bool chargingKnown = true;
    bool batteryValid = false;
    int millivolts = 0;

    // 物理型号具备无线/电池能力（与当前链路无关）；由 FinalizeDeviceCapabilities 计算。
    bool wirelessCapable = false;
    // UI/日志/通知是否把“电量”视为该设备属性；为 false 时不显示电量卡、不写电量事件。
    bool batteryApplicable = false;

    DpiInfo dpi;
    PollingInfo polling;
    DebounceInfo debounce;
    AngleInfo angle;

    // 物理设备容器 id（USB/蓝牙拓扑）；已知协议可能为空，通用回退会填。
    std::wstring containerId;
    // Protocol-specific routing information.
    std::vector<HidInterface> ifaces;
    std::wstring path;
    std::wstring basePath;
    int lgIndex = 0;
    int batteryScale = 1;
    int statusReportId = 0;
};

struct ApplyRequest {
    std::wstring deviceId;
    bool setDpi = false;
    int dpiX = 0;
    int dpiY = 0;
    int stageIndex = -1;
    bool selectStage = false;
    bool setPolling = false;
    int pollingHz = 0;
    bool setDebounce = false;
    int debounceMs = 0;
    bool setAngle = false;
    bool angleOn = false;
};

// 连接显示名：UI 与 --once 共用。USB 链路但型号具备无线能力时显示
// 「USB 有线 · 无线模式」，避免 CLI 复核把插线无线设备误判成纯有线。
std::wstring DeviceConnLabel(const Device& d);

namespace proto {

void ReadLogitech(const std::vector<HidInterface>& all, std::vector<Device>& out);
bool ApplyLogitech(const Device& dev, const ApplyRequest& req, std::wstring& err);

void ReadAsus(const std::vector<HidInterface>& all, std::vector<Device>& out);
bool ApplyAsus(const Device& dev, const ApplyRequest& req, std::wstring& err);

void ReadRazer(const std::vector<HidInterface>& all, std::vector<Device>& out);
bool ApplyRazer(const Device& dev, const ApplyRequest& req, std::wstring& err);

void ReadRapoo(const std::vector<HidInterface>& all, std::vector<Device>& out);
bool ApplyRapoo(const Device& dev, const ApplyRequest& req, std::wstring& err);

void ReadGeneric(const std::vector<HidInterface>& all, std::vector<Device>& out);
bool ApplyGeneric(const Device& dev, const ApplyRequest& req, std::wstring& err);

void ReadValkyrie(const std::vector<HidInterface>& all, std::vector<Device>& out);
void ReadMoreBetter(const std::vector<HidInterface>& all, std::vector<Device>& out);

void AddSystemBatteries(std::vector<Device>& out);

// 把 Windows 电池读数归并到设备列表（AddSystemBatteries 的纯逻辑部分，便于单测）：
// 精确同名优先，其次唯一互含名称；多候选歧义时宁可不补；无有效百分比不新建条目。
void MergeSystemBatteries(const std::vector<SystemBattery>& batteries, std::vector<Device>& out);

// USB / 蓝牙通用 HID 回退（未知厂商协议只读列出）。
void AddWiredHidDevices(const std::vector<HidInterface>& all, std::vector<Device>& out);

// 已连接的蓝牙音频设备（经典 A2DP/HFP 或 LE Audio）只读列入。
// 这类设备通常没有 HID 接口，也没有 Windows 电池节点；不要伪造电量。
void AddBluetoothAudioDevices(const std::vector<BluetoothAudioDevice>& all,
                              std::vector<Device>& out);

// 产品名/型号名是否包含无线关键词（用于识别“无线设备插线”的型号）。
bool LooksWirelessProduct(const std::wstring& text);
// identity 是否为已知无线机型（zaopin:/valkyrie: 前缀）。
bool IsKnownWirelessIdentity(const std::wstring& identity);
// 扫描线程发布快照前调用一次，计算 wirelessCapable / batteryApplicable。
void FinalizeDeviceCapabilities(std::vector<Device>& devices);

}  // namespace proto

// 兼容既有自由函数调用方式；实现与 proto:: 中的声明相同。
using proto::FinalizeDeviceCapabilities;
using proto::IsKnownWirelessIdentity;
using proto::LooksWirelessProduct;

std::wstring FriendlyProtocolName(const Device& d);
bool DeviceSupportsWrites(const Device& d);
