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
    bool batteryValid = false;
    int millivolts = 0;

    DpiInfo dpi;
    PollingInfo polling;
    DebounceInfo debounce;
    AngleInfo angle;

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

void AddSystemBatteries(std::vector<Device>& out);

}  // namespace proto

std::wstring FriendlyProtocolName(const Device& d);
bool DeviceSupportsWrites(const Device& d);
