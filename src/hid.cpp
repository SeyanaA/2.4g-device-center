#include "hid.h"

#include "io_cancel.h"
#include "samsung_buds.h"

#include <winioctl.h>
#ifndef FILE_DEVICE_BATTERY
#define FILE_DEVICE_BATTERY 0x00000029
#endif
#include <batclass.h>
#include <bluetoothapis.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>

namespace {

HANDLE g_timerQueue = nullptr;
std::once_flag g_timerOnce;

void CALLBACK CancelIoCallback(PVOID param, BOOLEAN) {
    CancelSynchronousIo((HANDLE)param);
}

void EnsureTimerQueue() {
    std::call_once(g_timerOnce, [] {
        g_timerQueue = CreateTimerQueue();
    });
}

class SyncWatchdog {
public:
    explicit SyncWatchdog(DWORD ms) {
        EnsureTimerQueue();
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &thread_, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            thread_ = nullptr;
            return;
        }
        if (ms && g_timerQueue) {
            CreateTimerQueueTimer(&timer_, g_timerQueue, CancelIoCallback, thread_, ms, 0,
                                  WT_EXECUTEONLYONCE);
        }
    }
    ~SyncWatchdog() {
        if (timer_) {
            DeleteTimerQueueTimer(g_timerQueue, timer_, INVALID_HANDLE_VALUE);
            timer_ = nullptr;
        }
        if (thread_) CloseHandle(thread_);
    }
    SyncWatchdog(const SyncWatchdog&) = delete;
    SyncWatchdog& operator=(const SyncWatchdog&) = delete;

private:
    HANDLE thread_ = nullptr;
    HANDLE timer_ = nullptr;
};

std::wstring GetRegString(HDEVINFO set, SP_DEVINFO_DATA& did, DWORD prop) {
    wchar_t buf[1024] = {};
    DWORD type = 0;
    DWORD need = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, &did, prop, &type, (PBYTE)buf, sizeof(buf), &need)) {
        if (type == REG_SZ || type == REG_EXPAND_SZ) return buf;
    }
    return {};
}

std::wstring GetRegMultiStringJoined(HDEVINFO set, SP_DEVINFO_DATA& did, DWORD prop) {
    std::vector<wchar_t> buf(4096);
    DWORD type = 0;
    DWORD need = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, &did, prop, &type, (PBYTE)buf.data(),
                                          (DWORD)(buf.size() * sizeof(wchar_t)), &need)) {
        if (type == REG_MULTI_SZ) {
            std::wstring out;
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
                if (!out.empty()) out += L" ";
                out += p;
            }
            return out;
        }
    }
    return {};
}

// {8C7ED206-3F8A-4827-B3AB-AE9E1FAEFC6C}, pid 2 = DEVPKEY_Device_ContainerId。
// MinGW 的 devpkey.h 只声明该属性键（未定义 INITGUID 时没有实体），这里直接
// 给出键值，避免额外的链接依赖。
const DEVPROPKEY kDevPkeyDeviceContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}},
    2};

// {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, pid 2 = DEVPKEY_Device_BatteryLevel。
// 部分蓝牙设备（尤其 LE/HFP 上报电量的型号）会把 0..100 写进设备属性；
// Galaxy Buds4 Pro 在当前 Windows 上没有该属性，读不到时保持 -1。
const DEVPROPKEY kDevPkeyDeviceBatteryLevel = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}},
    2};

// {E0CBF06C-CD8B-4647-BB8A-263B43F0F974}：Bluetooth 设备类。
const GUID kGUIDDevClassBluetooth = {
    0xe0cbf06c, 0xcd8b, 0x4647, {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}};

std::wstring FormatGuidString(const GUID& g) {
    return Fmt(L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               (unsigned long)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
               (unsigned)g.Data4[0], (unsigned)g.Data4[1], (unsigned)g.Data4[2],
               (unsigned)g.Data4[3], (unsigned)g.Data4[4], (unsigned)g.Data4[5],
               (unsigned)g.Data4[6], (unsigned)g.Data4[7]);
}

// 查询设备容器 ID：同一物理设备（鼠标本体、接收器、蓝牙链路）共享该 GUID，
// 通用回退用它把多个 HID 集合合并成一个设备。失败时返回空串。
std::wstring GetContainerId(HDEVINFO set, SP_DEVINFO_DATA& did) {
    GUID guid{};
    DEVPROPTYPE type = 0;
    DWORD need = 0;
    if (!SetupDiGetDevicePropertyW(set, &did, &kDevPkeyDeviceContainerId, &type, (PBYTE)&guid,
                                   sizeof(guid), &need, 0)) {
        return {};
    }
    if (type != DEVPROP_TYPE_GUID) return {};
    return FormatGuidString(guid);
}

bool IsInterestingBattery(const std::wstring& friendly, const std::wstring& desc,
                          const std::wstring& hardware, const std::wstring& enumerator) {
    std::wstring name = friendly.empty() ? desc : friendly;
    std::wstring all = name + L" " + hardware + L" " + enumerator;
    const wchar_t* exclude[] = {L"laptop", L"笔记本", L"main battery", L"主电池", L"platform"};
    for (auto e : exclude) {
        if (ContainsNoCase(all, e)) return false;
    }
    const wchar_t* include[] = {L"mouse", L"keyboard", L"headset", L"gamepad", L"controller",
                                L"HID", L"mouse", L"keyboard", L"鼠标", L"键盘", L"耳机",
                                L"手柄", L"无线", L"蓝牙", L"bluetooth"};
    for (auto e : include) {
        if (ContainsNoCase(all, e)) return true;
    }
    return false;
}

// {72631E54-78A4-11D0-BCF7-00AA00B7B32A}
const GUID kGUIDDevClassBattery = {
    0x72631e54, 0x78a4, 0x11d0, {0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a}};

}  // namespace

bool ParseVidPid(const std::wstring& path, uint16_t& vid, uint16_t& pid) {
    vid = pid = 0;
    const std::wstring lower = Lower(path);
    // USB 路径用 vid_/pid_；蓝牙 HID 的实例 ID 常见 vid&/pid&（8 位十六进制，
    // 例如 ..._vid&0001000f_pid&...）。只认 vid_ 会把蓝牙设备判成 0000:0000，
    // 导致通用回退按空 VID/PID 合并同型号设备（见蓝牙父链/去重路径）。
    auto parse = [&lower](const wchar_t* marker, const wchar_t* alt, uint16_t& out) {
        size_t pos = lower.find(marker);
        size_t skip = 4;
        if (pos == std::wstring::npos) {
            pos = lower.find(alt);
            skip = 4;
        }
        if (pos == std::wstring::npos) return false;
        const size_t start = pos + skip;
        size_t end = start;
        auto isHex = [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                   (c >= L'A' && c <= L'F');
        };
        while (end < lower.size() && isHex(lower[end])) ++end;
        if (end - start < 4) return false;
        // 8 位形式取低 16 位（Windows 的 VID& 是 32 位复合值）。
        const size_t use = (end - start) > 4 ? end - 4 : start;
        out = (uint16_t)wcstoul(lower.substr(use, 4).c_str(), nullptr, 16);
        return true;
    };
    const bool hasVid = parse(L"vid_", L"vid&", vid);
    const bool hasPid = parse(L"pid_", L"pid&", pid);
    return hasVid && hasPid && vid != 0;
}

std::wstring HidPathKey(const std::wstring& path) {
    std::wstring lower = Lower(path);
    size_t first = lower.find(L'#');
    size_t second = first == std::wstring::npos ? std::wstring::npos : lower.find(L'#', first + 1);
    size_t third = second == std::wstring::npos ? std::wstring::npos : lower.find(L'#', second + 1);
    if (first == std::wstring::npos) return lower;
    std::wstring iface = lower.substr(first + 1, (second == std::wstring::npos ? lower.size() : second) - first - 1);
    size_t col = iface.find(L"&col");
    if (col != std::wstring::npos) iface = iface.substr(0, col);
    std::wstring instance;
    if (second != std::wstring::npos) {
        instance = lower.substr(second + 1, (third == std::wstring::npos ? lower.size() : third) - second - 1);
    }
    return iface + L"#" + instance;
}

std::wstring DeviceKindFromText(const std::wstring& text) {
    if (ContainsNoCase(text, L"mouse") || ContainsNoCase(text, L"鼠标")) return L"mouse";
    if (ContainsNoCase(text, L"keyboard") || ContainsNoCase(text, L"键盘")) return L"keyboard";
    if (ContainsNoCase(text, L"headset") || ContainsNoCase(text, L"耳机")) return L"headset";
    return L"device";
}

bool PathLooksBluetooth(const std::wstring& path) {
    const std::wstring lower = Lower(path);
    return lower.find(L"{00001124-0000-1000-8000-00805f9b34fb}") != std::wstring::npos ||
           lower.find(L"{00001812-0000-1000-8000-00805f9b34fb}") != std::wstring::npos ||
           lower.find(L"bthhfenum") != std::wstring::npos ||
           lower.find(L"bthledevice") != std::wstring::npos ||
           lower.find(L"bthle") != std::wstring::npos ||
           lower.find(L"bthenum") != std::wstring::npos;
}

bool InstanceIdLooksBluetooth(const std::wstring& instanceId) {
    const std::wstring lower = Lower(instanceId);
    return lower.find(L"bthenum") != std::wstring::npos ||
           lower.find(L"bthle") != std::wstring::npos ||
           lower.find(L"bthhfenum") != std::wstring::npos ||
           // 蓝牙 HID 节点的父设备可能是 BTHUSB/BTHPORT；只看 BTHENUM 会漏判
           // “设备节点本身就是 BTHENUM、父节点是 BTHUSB”的机型。
           lower.find(L"bthusb") != std::wstring::npos ||
           lower.find(L"bthport") != std::wstring::npos;
}

namespace {

// 部分蓝牙 HID 的接口路径只有 HID#VID_...，枚举器链仍是 BTHENUM/BTHLE；
// 仅按路径会把它们误判成 USB（验收项 3）。逐级查父设备实例 ID 兜底。
// 先检查起始节点再上溯：某些设备的 devInst 本身就是 BTHENUM 节点，只查父链
// 会看到 BTHUSB 而漏判。深度放宽到 16，单级 ID 查询失败继续上溯而不是
// 立即否定（CR_BUFFER_SMALL 等失败不代表链路不是蓝牙）。
bool HasBluetoothAncestor(DEVINST devInst) {
    DEVINST cur = devInst;
    for (int depth = 0; depth < 16; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (cur && CM_Get_Device_IDW(cur, id, ARRAYSIZE(id), 0) == CR_SUCCESS &&
            InstanceIdLooksBluetooth(id)) {
            return true;
        }
        DEVINST parent = 0;
        if (!cur || CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS || !parent) return false;
        cur = parent;
    }
    return false;
}

// 从 BTHENUM\DEV_<12 hex>\... 中取出蓝牙地址（小写）；不是 DEV 节点时返回空。
std::wstring BluetoothAddressFromInstanceId(const std::wstring& instanceId) {
    const std::wstring lower = Lower(instanceId);
    size_t pos = lower.find(L"\\dev_");
    if (pos == std::wstring::npos) return {};
    const size_t start = pos + 5;
    size_t end = lower.find(L'\\', start);
    if (end == std::wstring::npos) end = lower.size();
    if (end - start != 12) return {};
    std::wstring addr = lower.substr(start, 12);
    for (wchar_t c : addr) {
        if (!iswxdigit(c)) return {};
    }
    return addr;
}

// 读取 Windows 暴露在蓝牙设备节点上的电量属性（DEVPKEY_Device_BatteryLevel）。
// 这是“有就计入、没有就不造”的可选源：Galaxy Buds4 Pro 当前没有该属性，
// 因此不会产生电量事件；支持上报的型号会进入 BATTERY/SAMPLE 日志与曲线。
std::map<std::wstring, int> ReadBluetoothBatteryLevels() {
    std::map<std::wstring, int> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kGUIDDevClassBluetooth, nullptr, nullptr,
                                        DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i) {
        // B01：逐设备检查取消，Stop() 不必等整轮属性读取。
        if (iocancel::Requested()) break;
        SP_DEVINFO_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInfo(set, i, &did)) break;
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(did.DevInst, id, ARRAYSIZE(id), 0) != CR_SUCCESS) continue;
        const std::wstring addr = BluetoothAddressFromInstanceId(id);
        if (addr.empty()) continue;

        DEVPROPTYPE type = 0;
        BYTE buf[64] = {};
        ULONG need = sizeof(buf);
        if (CM_Get_DevNode_PropertyW(did.DevInst, &kDevPkeyDeviceBatteryLevel, &type, buf,
                                     &need, 0) != CR_SUCCESS) {
            continue;
        }
        int pct = -1;
        if (type == DEVPROP_TYPE_BYTE) {
            pct = buf[0];
        } else if (type == DEVPROP_TYPE_UINT32 || type == DEVPROP_TYPE_INT32) {
            uint32_t v = 0;
            memcpy(&v, buf, sizeof(v));
            pct = (int)v;
        } else if (type == DEVPROP_TYPE_STRING) {
            pct = (int)wcstol((const wchar_t*)buf, nullptr, 10);
        }
        if (pct >= 0 && pct <= 100) out[addr] = pct;
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

}  // namespace

std::vector<HidInterface> EnumerateHidInterfaces() {
    std::vector<HidInterface> out;
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i) {
        // B01：枚举本身（每个接口要 CreateFile + 读属性/字符串）在 HID 接口多的
        // 机器上能占几百 ms；逐接口检查取消，Stop() 不必等整轮枚举。
        if (iocancel::Requested()) break;
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &did)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> storage(need + 8, 0);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)storage.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, &need, &devInfo)) continue;

        HidInterface hi;
        hi.path = detail->DevicePath;
        if (hi.path.empty()) continue;

        // 链路类型按设备接口路径判定；蓝牙 HID 路径通常带 HID service UUID，
        // 也可能是 bth* 枚举器。必须先判蓝牙，再判 USB（BT 路径也可能含 HID#VID_）。
        // 路径看不出时再查父链实例 ID，覆盖“BT 设备却走 USB 风格路径”的机型。
        const std::wstring lowerPath = Lower(hi.path);
        const bool btPath = PathLooksBluetooth(hi.path) || HasBluetoothAncestor(devInfo.DevInst);
        if (btPath) {
            hi.bus = HidBus::Bluetooth;
        } else if (lowerPath.find(L"hid#vid_") != std::wstring::npos) {
            hi.bus = HidBus::Usb;
        }
        hi.containerId = GetContainerId(set, devInfo);

        HANDLE h = CreateFileW(hi.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(hi.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            hi.vid = attr.VendorID;
            hi.pid = attr.ProductID;
        }
        if (!hi.vid) ParseVidPid(hi.path, hi.vid, hi.pid);

        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                hi.usagePage = caps.UsagePage;
                hi.usage = caps.Usage;
                hi.inLen = caps.InputReportByteLength;
                hi.outLen = caps.OutputReportByteLength;
                hi.featLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }

        wchar_t str[512] = {};
        if (HidD_GetProductString(h, str, sizeof(str))) hi.product = Trim(str);
        str[0] = 0;
        if (HidD_GetManufacturerString(h, str, sizeof(str))) hi.manufacturer = Trim(str);
        if (hi.product.empty()) {
            hi.product = GetRegString(set, devInfo, SPDRP_DEVICEDESC);
        }
        if (hi.product.empty()) hi.product = L"未知 HID 设备";
        CloseHandle(h);

        std::wstring lower = Lower(hi.path);
        size_t mi = lower.find(L"&mi_");
        if (mi != std::wstring::npos && mi + 6 <= lower.size()) {
            hi.interfaceNumber = (int)wcstoul(lower.substr(mi + 4, 2).c_str(), nullptr, 16);
        }
        size_t col = lower.find(L"&col");
        if (col != std::wstring::npos && col + 6 <= lower.size()) {
            hi.collection = (int)wcstoul(lower.substr(col + 4, 2).c_str(), nullptr, 16);
        }
        hi.basePath = HidPathKey(hi.path);

        // 只保留一次额外的写入权限探测：canOpenRW 仅用于 --diag 展示。
        // 原实现还额外用 GENERIC_READ 打开一次（canOpenRead 无任何调用方），
        // 每个接口每轮扫描多一次系统调用，已删除。
        HANDLE hr = CreateFileW(hi.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
        if (hr != INVALID_HANDLE_VALUE) {
            hi.canOpenRW = true;
            CloseHandle(hr);
        }

        out.push_back(std::move(hi));
    }
    SetupDiDestroyDeviceInfoList(set);

    std::sort(out.begin(), out.end(), [](const HidInterface& a, const HidInterface& b) {
        if (a.vid != b.vid) return a.vid < b.vid;
        if (a.pid != b.pid) return a.pid < b.pid;
        return a.path < b.path;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const HidInterface& a, const HidInterface& b) {
                              return a.path == b.path;
                          }),
              out.end());
    return out;
}

std::vector<SystemBattery> EnumerateSystemBatteries() {
    std::vector<SystemBattery> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kGUIDDevClassBattery, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i) {
        // B01：电池 IOCTL 单次最长 600 ms（SyncWatchdog），逐设备检查取消。
        if (iocancel::Requested()) break;
        SP_DEVICE_INTERFACE_DATA did{};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &kGUIDDevClassBattery, i, &did)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> storage(need + 8, 0);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)storage.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, &need, &devInfo)) continue;

        std::wstring friendly = GetRegString(set, devInfo, SPDRP_FRIENDLYNAME);
        std::wstring desc = GetRegString(set, devInfo, SPDRP_DEVICEDESC);
        std::wstring hardware = GetRegMultiStringJoined(set, devInfo, SPDRP_HARDWAREID);
        std::wstring enumerator = GetRegString(set, devInfo, SPDRP_ENUMERATOR_NAME);
        if (!IsInterestingBattery(friendly, desc, hardware, enumerator)) continue;

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            // 部分电池驱动拒绝写访问但允许只读；只用 RW 打开会静默漏掉整个设备。
            h = CreateFileW(detail->DevicePath, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
        }
        // 原实现这里又用完全相同的访问参数重试一次，失败结果必然相同；
        // 删掉这次无意义调用，改为上面的只读回退。
        if (h == INVALID_HANDLE_VALUE) continue;

        ULONG tag = 0;
        ULONG wait = 0;
        DWORD bytes = 0;
        {
            SyncWatchdog wd(600);
            if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &tag,
                                 sizeof(tag), &bytes, nullptr)) {
                CloseHandle(h);
                continue;
            }
        }
        if (tag == 0) {
            CloseHandle(h);
            continue;
        }

        BATTERY_QUERY_INFORMATION qi{};
        qi.BatteryTag = tag;
        qi.InformationLevel = BatteryInformation;
        BATTERY_INFORMATION bi{};
        {
            SyncWatchdog wd(600);
            if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &qi, sizeof(qi), &bi,
                                 sizeof(bi), &bytes, nullptr)) {
                CloseHandle(h);
                continue;
            }
        }

        BATTERY_WAIT_STATUS ws{};
        ws.BatteryTag = tag;
        BATTERY_STATUS st{};
        {
            SyncWatchdog wd(600);
            if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof(ws), &st, sizeof(st),
                                 &bytes, nullptr)) {
                CloseHandle(h);
                continue;
            }
        }
        CloseHandle(h);

        SystemBattery sb;
        sb.name = friendly.empty() ? desc : friendly;
        if (sb.name.empty()) sb.name = L"系统电池设备";
        sb.charging = (st.PowerState & BATTERY_CHARGING) != 0;
        sb.capacity = st.Capacity;
        sb.fullCapacity = bi.FullChargedCapacity ? bi.FullChargedCapacity : bi.DesignedCapacity;
        sb.millivolts = (int)st.Voltage;
        if (sb.fullCapacity > 0 && st.Capacity <= sb.fullCapacity) {
            sb.percent = (int)((uint64_t)st.Capacity * 100 / sb.fullCapacity);
        }
        wchar_t instance[MAX_PATH] = {};
        if (SetupDiGetDeviceInstanceIdW(set, &devInfo, instance, ARRAYSIZE(instance), nullptr)) {
            sb.id = instance;
        } else {
            sb.id = detail->DevicePath;
        }
        out.push_back(std::move(sb));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool BluetoothClassIsAudio(uint32_t classOfDevice) {
    // Class of Device: bit 8-12 是 Major Device Class，0x04 = Audio/Video。
    return ((classOfDevice >> 8) & 0x1Fu) == 0x04u;
}

bool BluetoothAudioLooksLikeHeadset(const std::wstring& name, uint32_t classOfDevice) {
    // Audio/Video 的 Minor Device Class：1=Wearable Headset，2=Hands-free，
    // 6=Headphones。部分耳机只报 0（未分类），再用名称关键词兜底。
    const uint32_t minor = (classOfDevice >> 2) & 0x3Fu;
    if (minor == 1 || minor == 2 || minor == 6) return true;
    const wchar_t* keywords[] = {L"headset", L"headphone", L"earphone", L"earbud",
                                 L"buds",    L"airpods",   L"耳机",     L"耳麦",
                                 L"头戴"};
    for (const wchar_t* k : keywords) {
        if (ContainsNoCase(name, k)) return true;
    }
    return false;
}

std::vector<BluetoothAudioDevice> EnumerateBluetoothAudioDevices() {
    std::vector<BluetoothAudioDevice> out;
    const std::map<std::wstring, int> batteries = ReadBluetoothBatteryLevels();

    // 只枚举“当前已连接”的设备：已配对但未连接的耳机不应出现在在线列表里。
    // 不发起 inquiry、不配对、不写设备设置。电量优先取 Windows 属性；没有属性时
    // 仅对暴露 Samsung SPP 的耳机做一次只读状态查询（见 samsung_buds.h）。
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp{};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = FALSE;
    sp.fReturnRemembered = FALSE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;
    sp.cTimeoutMultiplier = 1;
    sp.hRadio = nullptr;

    BLUETOOTH_DEVICE_INFO bi{};
    bi.dwSize = sizeof(bi);
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&sp, &bi);
    // 没有蓝牙适配器、服务被禁用或权限不足时返回空；不能影响 HID/2.4G 扫描。
    if (!find) return out;

    do {
        // B01：每个耳机可能要建一次 RFCOMM（Samsung SPP 预算 2500 ms），
        // 逐设备检查取消。
        if (iocancel::Requested()) break;
        bi.dwSize = sizeof(bi);
        if (!bi.fConnected) continue;
        if (!BluetoothClassIsAudio(bi.ulClassofDevice)) continue;

        BluetoothAudioDevice dev;
        // Address 是 6 字节小端存储，按人类可读顺序拼成 12 位十六进制。
        dev.address = Fmt(L"%02X%02X%02X%02X%02X%02X", (unsigned)bi.Address.rgBytes[5],
                          (unsigned)bi.Address.rgBytes[4], (unsigned)bi.Address.rgBytes[3],
                          (unsigned)bi.Address.rgBytes[2], (unsigned)bi.Address.rgBytes[1],
                          (unsigned)bi.Address.rgBytes[0]);
        dev.name = Trim(bi.szName);
        if (dev.name.empty()) dev.name = L"蓝牙音频设备";
        dev.classOfDevice = bi.ulClassofDevice;
        dev.headset = BluetoothAudioLooksLikeHeadset(dev.name, dev.classOfDevice);
        auto bit = batteries.find(Lower(dev.address));
        if (bit != batteries.end()) {
            dev.battery = bit->second;
            dev.batterySource = BluetoothBatterySource::WindowsProperty;
        } else if (SamsungBudsServiceAvailable(dev.address)) {
            // Windows 没有暴露电量属性时才走厂商 SPP；只读取设备主动上报的
            // 状态帧，不配对、不写入。缓存避免每 5 秒重复建链。
            SamsungBudsStatus status;
            if (GetSamsungBudsStatusCached(dev.address, status)) {
                const int percent = SamsungBudsDeviceBattery(status);
                if (percent >= 0) {
                    dev.battery = percent;
                    dev.batterySource = BluetoothBatterySource::SamsungSpp;
                    dev.chargingKnown = status.chargingKnown;
                    dev.charging = status.chargingKnown &&
                                   (status.chargingLeft || status.chargingRight);
                }
            }
        }

        bool duplicate = false;
        for (const auto& existing : out) {
            if (_wcsicmp(existing.address.c_str(), dev.address.c_str()) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) out.push_back(std::move(dev));
    } while (BluetoothFindNextDevice(find, &bi));

    BluetoothFindDeviceClose(find);
    std::sort(out.begin(), out.end(), [](const BluetoothAudioDevice& a,
                                         const BluetoothAudioDevice& b) {
        return Lower(a.name) < Lower(b.name);
    });
    return out;
}

HidHandle::~HidHandle() { Close(); }

HidHandle::HidHandle(HidHandle&& other) noexcept {
    handle_ = other.handle_;
    info_ = std::move(other.info_);
    overlapped_ = other.overlapped_;
    other.handle_ = INVALID_HANDLE_VALUE;
}

HidHandle& HidHandle::operator=(HidHandle&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        info_ = std::move(other.info_);
        overlapped_ = other.overlapped_;
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
}

void HidHandle::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    overlapped_ = false;
}

bool HidHandle::Open(const HidInterface& info, DWORD access, bool overlapped) {
    info_ = info;
    // OpenPath 内部已包含 access=0 的只查询回退；这里不再重复尝试一次
    // 完全相同的调用。
    return OpenPath(info.path, access, overlapped);
}

bool HidHandle::OpenPath(const std::wstring& path, DWORD access, bool overlapped) {
    Close();
    // B01：取消后不再打开新句柄。协议对每个接口都会尝试打开，23 个接口逐个
    // CreateFileW 在一次扫描里能占几百 ms；这个检查让取消后的遍历近乎零成本。
    if (iocancel::Requested()) return false;
    DWORD flags = overlapped ? FILE_FLAG_OVERLAPPED : 0;
    handle_ = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, flags, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE && access != 0) {
        handle_ = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, flags, nullptr);
    }
    overlapped_ = overlapped;
    return handle_ != INVALID_HANDLE_VALUE;
}

bool HidHandle::sendRaw(const uint8_t* data, size_t len, DWORD timeoutMs) {
    if (!valid()) return false;
    // B01：Manager::Stop() 置位后不再发起新 I/O，工作线程在一个操作内就能退出。
    if (iocancel::Requested()) return false;
    std::vector<uint8_t> padded;
    if (info_.outLen > 0) {
        if (len > info_.outLen) return false;
        if (len < info_.outLen) {
            padded.assign(info_.outLen, 0);
            memcpy(padded.data(), data, len);
            data = padded.data();
            len = info_.outLen;
        }
    }
    if (overlapped_) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD written = 0;
        BOOL ok = WriteFile(handle_, data, (DWORD)len, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            // B01：驱动返回 PENDING 时 CancelSynchronousIo 无效，必须让
            // Manager::Stop() 通过 CancelIoEx 打断这次等待（否则要等满 timeout）。
            iocancel::BeginIo(handle_);
            DWORD wr = WaitForSingleObject(ov.hEvent, timeoutMs);
            iocancel::EndIo(handle_);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(handle_, &ov);
                // CancelIoEx 只“发起取消”，不保证 IRP 已经完成。若在完成前
                // 释放栈上的 OVERLAPPED（以及调用方/ padded 缓冲区），内核稍后
                // 仍会写入已释放内存，形成 UAF/内存破坏。这里必须等到完成
                // 再关闭事件并返回；驱动异常时宁可多等也不提前释放。
                DWORD ignored = 0;
                GetOverlappedResult(handle_, &ov, &ignored, TRUE);
                CloseHandle(ov.hEvent);
                return false;
            }
            ok = GetOverlappedResult(handle_, &ov, &written, FALSE);
        } else if (ok) {
            // 同步完成时 WriteFile 的 lpNumberOfBytesWritten 为 null，必须从 OVERLAPPED 取长度。
            ok = GetOverlappedResult(handle_, &ov, &written, FALSE);
        }
        CloseHandle(ov.hEvent);
        return ok && written > 0;
    }
    DWORD written = 0;
    SyncWatchdog wd(timeoutMs);
    BOOL ok = WriteFile(handle_, data, (DWORD)len, &written, nullptr);
    if (!ok) return false;
    return written > 0;
}

bool HidHandle::writeReport(uint8_t reportId, const uint8_t* data, size_t len, DWORD timeoutMs) {
    size_t outLen = info_.outLen ? info_.outLen : len + 1;
    std::vector<uint8_t> buf;
    if (reportId != 0) {
        size_t size = std::max(outLen, len + 1);
        buf.assign(size, 0);
        buf[0] = reportId;
        memcpy(buf.data() + 1, data, std::min(len, size - 1));
        outLen = size;
    } else if (outLen >= len + 1) {
        buf.assign(outLen, 0);
        buf[0] = 0;
        memcpy(buf.data() + 1, data, len);
    } else {
        buf.assign(len, 0);
        memcpy(buf.data(), data, len);
        outLen = len;
    }
    if (sendRaw(buf.data(), outLen, timeoutMs)) return true;

    // Some vendor collections only accept a control-transfer output report.
    if (!overlapped_) {
        SyncWatchdog wd(timeoutMs);
        if (HidD_SetOutputReport(handle_, buf.data(), (ULONG)outLen)) return true;
    }
    return false;
}

int HidHandle::readRaw(uint8_t* buf, size_t len, DWORD timeoutMs) {
    if (!valid()) return -1;
    if (iocancel::Requested()) return -1;
    if (overlapped_) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return -1;
        DWORD got = 0;
        BOOL ok = ReadFile(handle_, buf, (DWORD)len, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            // 同 sendRaw：overlapped 等待只能靠 CancelIoEx 打断。
            iocancel::BeginIo(handle_);
            DWORD wr = WaitForSingleObject(ov.hEvent, timeoutMs);
            iocancel::EndIo(handle_);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(handle_, &ov);
                // 同 sendRaw：取消后必须等到 IRP 真正完成，才能释放调用方的
                // buf 和栈上的 OVERLAPPED。readMatching 每 80 ms 切片一次，
                // 这条路径在设备无响应时很常见，提前释放会造成内核写已释放内存。
                DWORD ignored = 0;
                GetOverlappedResult(handle_, &ov, &ignored, TRUE);
                CloseHandle(ov.hEvent);
                return -1;
            }
            ok = GetOverlappedResult(handle_, &ov, &got, FALSE);
        } else if (ok) {
            GetOverlappedResult(handle_, &ov, &got, FALSE);
        }
        CloseHandle(ov.hEvent);
        return ok ? (int)got : -1;
    }
    DWORD got = 0;
    SyncWatchdog wd(timeoutMs);
    BOOL ok = ReadFile(handle_, buf, (DWORD)len, &got, nullptr);
    return ok ? (int)got : -1;
}

int HidHandle::readMatching(uint8_t* buf, size_t len,
                            const std::function<bool(const uint8_t*, int)>& match,
                            DWORD timeoutMs) {
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < end) {
        // B01：每 80 ms 切片检查一次取消，阻塞等待最多延长一个切片。
        if (iocancel::Requested()) return -1;
        DWORD left = (DWORD)(end - GetTickCount64());
        DWORD slice = left > 80 ? 80 : left;
        int n = readRaw(buf, len, slice);
        if (n > 0 && match(buf, n)) return n;
    }
    return -1;
}

bool HidHandle::getFeature(uint8_t reportId, uint8_t* data, size_t len, DWORD timeoutMs) {
    if (!valid()) return false;
    if (iocancel::Requested()) return false;
    size_t size = info_.featLen ? info_.featLen : len + 1;
    std::vector<uint8_t> buf(size, 0);
    if (reportId == 0 && size == len) {
        memcpy(buf.data(), data, len);
    } else {
        buf[0] = reportId;
        if (size > 1) memcpy(buf.data() + 1, data, std::min(len, size - 1));
    }
    SyncWatchdog wd(timeoutMs);
    if (!HidD_GetFeature(handle_, buf.data(), (ULONG)size)) return false;
    size_t copy = std::min(len, size);
    memcpy(data, buf.data(), copy);
    return true;
}

bool HidHandle::setFeature(uint8_t reportId, const uint8_t* data, size_t len, DWORD timeoutMs) {
    if (!valid()) return false;
    if (iocancel::Requested()) return false;
    size_t size = info_.featLen ? info_.featLen : len + 1;
    std::vector<uint8_t> buf(size, 0);
    if (reportId == 0 && size == len) {
        memcpy(buf.data(), data, len);
    } else {
        buf[0] = reportId;
        if (size > 1) memcpy(buf.data() + 1, data, std::min(len, size - 1));
    }
    SyncWatchdog wd(timeoutMs);
    return HidD_SetFeature(handle_, buf.data(), (ULONG)size) != FALSE;
}
