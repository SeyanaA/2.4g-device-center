#pragma once

#include <functional>

#include "common.h"

// 物理链路类型：通用回退只区分 USB 有线与蓝牙，Unknown 不参与回退。
enum class HidBus { Unknown = 0, Usb, Bluetooth };

struct HidInterface {
    std::wstring path;
    std::wstring basePath;      // path before &col (all collections of one interface)
    std::wstring product;
    std::wstring manufacturer;
    // 枚举器给出的链路类型（bthhfenum/bthledevice/bthenum 视为蓝牙，hid#vid_ 视为 USB）。
    HidBus bus = HidBus::Unknown;
    // DEVPKEY_Device_ContainerId；取不到时为空。同一物理设备的多个接口共享该值。
    std::wstring containerId;
    uint16_t vid = 0;
    uint16_t pid = 0;
    int interfaceNumber = -1;
    int collection = -1;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint16_t inLen = 0;
    uint16_t outLen = 0;
    uint16_t featLen = 0;
    bool canOpenRW = false;
};

struct SystemBattery {
    std::wstring id;
    std::wstring name;
    int percent = -1;
    bool charging = false;
    int millivolts = 0;
    uint32_t capacity = 0;
    uint32_t fullCapacity = 0;
};

// 已连接的蓝牙音频设备（经典 A2DP/HFP 或 LE Audio）。这类设备通常没有 HID
// 接口和 Windows Battery 节点，必须单独枚举才能出现在设备列表里。
enum class BluetoothBatterySource {
    None,             // 没有可读电量
    WindowsProperty,  // DEVPKEY_Device_BatteryLevel
    SamsungSpp,       // Samsung 耳机私有 SPP（只读）
};

struct BluetoothAudioDevice {
    std::wstring address;  // 12 位十六进制，来自 BLUETOOTH_DEVICE_INFO.Address
    std::wstring name;
    uint32_t classOfDevice = 0;
    // true = 耳机/耳麦/头戴；false = 其它音频设备（如蓝牙音箱）。
    bool headset = true;
    // 电量来源按优先级选择；读不到时保持 -1，不得伪造。
    int battery = -1;
    bool charging = false;
    // 只有来源明确给出充电位时才为 true。
    bool chargingKnown = false;
    BluetoothBatterySource batterySource = BluetoothBatterySource::None;
};

std::vector<HidInterface> EnumerateHidInterfaces();
std::vector<SystemBattery> EnumerateSystemBatteries();
std::vector<BluetoothAudioDevice> EnumerateBluetoothAudioDevices();

// 纯逻辑（可单测）：Class of Device 的 Major Device Class 是否为 Audio/Video。
bool BluetoothClassIsAudio(uint32_t classOfDevice);
// 纯逻辑（可单测）：音频设备是否按耳机归类（Minor Class 或名称关键词）。
bool BluetoothAudioLooksLikeHeadset(const std::wstring& name, uint32_t classOfDevice);

bool ParseVidPid(const std::wstring& path, uint16_t& vid, uint16_t& pid);
std::wstring HidPathKey(const std::wstring& path);
std::wstring DeviceKindFromText(const std::wstring& text);

// 蓝牙 HID 判定（纯字符串，便于单元测试）。
// 路径：HID service UUID、bthhfenum/bthledevice/bthle/bthenum；
// 实例 ID：BTHENUM/BTHLE/BTHHFENUM（用于父链兜底）。
bool PathLooksBluetooth(const std::wstring& path);
bool InstanceIdLooksBluetooth(const std::wstring& instanceId);

class HidHandle {
public:
    HidHandle() = default;
    ~HidHandle();
    HidHandle(const HidHandle&) = delete;
    HidHandle& operator=(const HidHandle&) = delete;
    HidHandle(HidHandle&& other) noexcept;
    HidHandle& operator=(HidHandle&& other) noexcept;

    bool Open(const HidInterface& info, DWORD access = GENERIC_READ | GENERIC_WRITE,
              bool overlapped = true);
    bool OpenPath(const std::wstring& path, DWORD access, bool overlapped);
    void Close();

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE handle() const { return handle_; }
    const HidInterface& info() const { return info_; }
    bool overlapped() const { return overlapped_; }

    bool sendRaw(const uint8_t* data, size_t len, DWORD timeoutMs);
    bool writeReport(uint8_t reportId, const uint8_t* data, size_t len, DWORD timeoutMs);
    int readRaw(uint8_t* buf, size_t len, DWORD timeoutMs);
    int readMatching(uint8_t* buf, size_t len,
                     const std::function<bool(const uint8_t*, int)>& match, DWORD timeoutMs);
    bool getFeature(uint8_t reportId, uint8_t* data, size_t len, DWORD timeoutMs);
    bool setFeature(uint8_t reportId, const uint8_t* data, size_t len, DWORD timeoutMs);

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HidInterface info_{};
    bool overlapped_ = false;
};
