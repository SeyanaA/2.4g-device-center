#include "hid.h"

#include <winioctl.h>
#ifndef FILE_DEVICE_BATTERY
#define FILE_DEVICE_BATTERY 0x00000029
#endif
#include <batclass.h>
#include <devguid.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <cstring>
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
    std::wstring lower = Lower(path);
    size_t vi = lower.find(L"vid_");
    size_t pi = lower.find(L"pid_");
    if (vi == std::wstring::npos || pi == std::wstring::npos) return false;
    if (vi + 8 > lower.size() || pi + 8 > lower.size()) return false;
    vid = (uint16_t)wcstoul(lower.substr(vi + 4, 4).c_str(), nullptr, 16);
    pid = (uint16_t)wcstoul(lower.substr(pi + 4, 4).c_str(), nullptr, 16);
    return vid != 0;
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

std::vector<HidInterface> EnumerateHidInterfaces() {
    std::vector<HidInterface> out;
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i) {
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

        HANDLE hr = CreateFileW(hi.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
        if (hr != INVALID_HANDLE_VALUE) {
            hi.canOpenRW = true;
            CloseHandle(hr);
        }
        hr = CreateFileW(hi.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, 0, nullptr);
        if (hr != INVALID_HANDLE_VALUE) {
            hi.canOpenRead = true;
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
            h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
        }
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
    if (OpenPath(info.path, access, overlapped)) return true;
    if (access != 0) {
        if (OpenPath(info.path, 0, overlapped)) return true;
    }
    return false;
}

bool HidHandle::OpenPath(const std::wstring& path, DWORD access, bool overlapped) {
    Close();
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
            DWORD wr = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(handle_, &ov);
                WaitForSingleObject(ov.hEvent, 150);
                CloseHandle(ov.hEvent);
                return false;
            }
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
    if (overlapped_) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return -1;
        DWORD got = 0;
        BOOL ok = ReadFile(handle_, buf, (DWORD)len, nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wr = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (wr != WAIT_OBJECT_0) {
                CancelIoEx(handle_, &ov);
                WaitForSingleObject(ov.hEvent, 150);
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
        DWORD left = (DWORD)(end - GetTickCount64());
        DWORD slice = left > 80 ? 80 : left;
        int n = readRaw(buf, len, slice);
        if (n > 0 && match(buf, n)) return n;
    }
    return -1;
}

bool HidHandle::getFeature(uint8_t reportId, uint8_t* data, size_t len, DWORD timeoutMs) {
    if (!valid()) return false;
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
