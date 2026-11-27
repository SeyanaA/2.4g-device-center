#define WIN32_LEAN_AND_MEAN
#include "samsung_buds.h"

#include "io_cancel.h"

#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr uint8_t kSom = 0xFD;
constexpr uint8_t kEom = 0xDD;
constexpr uint16_t kMsgStatusUpdated = 96;
constexpr uint16_t kMsgExtendedStatusUpdated = 97;
constexpr size_t kMaxPayloadGuard = 64 * 1024;

// Galaxy Buds4 Pro 暴露的 Samsung 专有 SPP 服务。
const GUID kSamsungSppService = {0x2e73a4ad, 0x332d, 0x41fc,
                                 {0x90, 0xe2, 0x16, 0xbe, 0xf0, 0x65, 0x23, 0xf2}};

bool PctValid(int v) { return v >= 0 && v <= 100; }

std::wstring NormalizeAddress(const std::wstring& in) {
    std::wstring out;
    out.reserve(12);
    for (wchar_t c : in) {
        if (iswxdigit(c)) out.push_back((wchar_t)towupper(c));
    }
    return out;
}

bool ParseAddress(const std::wstring& in, BTH_ADDR& out) {
    const std::wstring hex = NormalizeAddress(in);
    if (hex.size() != 12) return false;
    wchar_t* end = nullptr;
    const unsigned long long value = wcstoull(hex.c_str(), &end, 16);
    if (!end || *end != L'\0') return false;
    out = (BTH_ADDR)value;
    return true;
}

std::once_flag g_wsaOnce;
bool g_wsaReady = false;

bool EnsureWinsock() {
    std::call_once(g_wsaOnce, [] {
        WSADATA data{};
        g_wsaReady = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return g_wsaReady;
}

// 只读查询 SDP/注册表里该设备安装的服务，判断是否包含 Samsung SPP。
bool DetectSamsungSpp(const std::wstring& addressHex) {
    BTH_ADDR address = 0;
    if (!ParseAddress(addressHex, address)) return false;

    BLUETOOTH_FIND_RADIO_PARAMS params{};
    params.dwSize = sizeof(params);
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) return false;

    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    for (int i = 0; i < 6; ++i) {
        info.Address.rgBytes[i] = (BYTE)(address >> (8 * i));
    }

    DWORD count = 0;
    DWORD rc = BluetoothEnumerateInstalledServices(radio, &info, &count, nullptr);
    bool present = false;
    if ((rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA) && count > 0) {
        std::vector<GUID> guids(count);
        DWORD written = count;
        rc = BluetoothEnumerateInstalledServices(radio, &info, &written, guids.data());
        if (rc == ERROR_SUCCESS) {
            for (DWORD i = 0; i < written; ++i) {
                if (IsEqualGUID(guids[i], kSamsungSppService)) {
                    present = true;
                    break;
                }
            }
        }
    }

    BluetoothFindRadioClose(find);
    if (radio) CloseHandle(radio);
    return present;
}

std::mutex g_serviceMutex;
struct ServiceCacheEntry {
    bool present = false;
    ULONGLONG checked = 0;
};
std::map<std::wstring, ServiceCacheEntry> g_serviceCache;

std::mutex g_statusMutex;
struct StatusCacheEntry {
    SamsungBudsStatus status;
    ULONGLONG lastOk = 0;
    ULONGLONG lastFail = 0;
    bool haveOk = false;
};
std::map<std::wstring, StatusCacheEntry> g_statusCache;

}  // namespace

uint16_t SamsungBudsCrc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

SamsungFrameParse ParseSamsungBudsFrame(const uint8_t* data, size_t len,
                                        size_t& consumed, SamsungBudsStatus& out) {
    consumed = 0;
    if (!data || len == 0) return SamsungFrameParse::NeedMore;
    if (data[0] != kSom) {
        consumed = 1;
        return SamsungFrameParse::Skipped;
    }
    if (len < 4) return SamsungFrameParse::NeedMore;

    // 头部高字节带标志位（0x4000/0x8000/0xC000 等），长度只取低 10 位。
    const size_t size = ((size_t)data[1] | ((size_t)data[2] << 8)) & 0x03FFu;
    if (size < 3) {
        consumed = 1;
        return SamsungFrameParse::Skipped;
    }
    const size_t total = size + 4;
    if (len < total) return SamsungFrameParse::NeedMore;
    if (data[total - 1] != kEom) {
        consumed = 1;
        return SamsungFrameParse::Skipped;
    }

    const uint8_t* body = data + 3;  // msg_id + payload + crc
    const uint16_t wireCrc =
        (uint16_t)((uint16_t)body[size - 2] | ((uint16_t)body[size - 1] << 8));
    if (SamsungBudsCrc16(body, size - 2) != wireCrc) {
        consumed = 1;
        return SamsungFrameParse::Skipped;
    }

    const uint16_t msgId = body[0];
    const uint8_t* payload = body + 1;
    const size_t payloadSize = size - 3;
    consumed = total;

    if (msgId == kMsgExtendedStatusUpdated) {
        if (payloadSize < 8) return SamsungFrameParse::Other;
        SamsungBudsStatus status;
        status.batteryLeft = PctValid(payload[2]) ? payload[2] : -1;
        status.batteryRight = PctValid(payload[3]) ? payload[3] : -1;
        status.batteryCase = (payload[7] > 0 && PctValid(payload[7])) ? payload[7] : -1;
        if (payloadSize >= 44) {
            const uint8_t flags = payload[43];
            status.chargingKnown = true;
            status.chargingLeft = (flags & 0x10) != 0;
            status.chargingRight = (flags & 0x04) != 0;
            status.chargingCase = (flags & 0x01) != 0;
        }
        out = status;
        return SamsungFrameParse::Status;
    }

    if (msgId == kMsgStatusUpdated) {
        if (payloadSize < 3) return SamsungFrameParse::Other;
        SamsungBudsStatus status;
        status.batteryLeft = PctValid(payload[1]) ? payload[1] : -1;
        status.batteryRight = PctValid(payload[2]) ? payload[2] : -1;
        out = status;
        return SamsungFrameParse::Status;
    }

    return SamsungFrameParse::Other;
}

int SamsungBudsDeviceBattery(const SamsungBudsStatus& status) {
    const int left = (status.batteryLeft >= 0 && status.batteryLeft <= 100)
                         ? status.batteryLeft
                         : -1;
    const int right = (status.batteryRight >= 0 && status.batteryRight <= 100)
                          ? status.batteryRight
                          : -1;
    // 另一侧可能是 0（未佩戴/在仓内），不能把 0 当成整机电量。
    if (left > 0 && right > 0) return std::min(left, right);
    if (left > 0) return left;
    if (right > 0) return right;
    const bool charging =
        status.chargingKnown && (status.chargingLeft || status.chargingRight);
    if (charging && (left == 0 || right == 0)) return 0;
    return -1;
}

bool SamsungBudsServiceAvailable(const std::wstring& addressHex) {
    const std::wstring key = NormalizeAddress(addressHex);
    if (key.size() != 12) return false;

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_serviceMutex);
        auto it = g_serviceCache.find(key);
        if (it != g_serviceCache.end()) {
            const ULONGLONG ttl = it->second.present ? 10 * 60 * 1000ULL : 60 * 1000ULL;
            if (now - it->second.checked < ttl) return it->second.present;
        }
    }

    const bool present = DetectSamsungSpp(key);
    {
        std::lock_guard<std::mutex> lock(g_serviceMutex);
        ServiceCacheEntry entry;
        entry.present = present;
        entry.checked = GetTickCount64();
        g_serviceCache[key] = entry;
    }
    return present;
}

bool QuerySamsungBudsStatus(const std::wstring& addressHex, SamsungBudsStatus& out,
                            uint32_t timeoutMs) {
    out = SamsungBudsStatus{};
    BTH_ADDR address = 0;
    if (!EnsureWinsock() || !ParseAddress(addressHex, address)) return false;

    SOCKET socketHandle = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (socketHandle == INVALID_SOCKET) return false;

    SOCKADDR_BTH target{};
    target.addressFamily = AF_BTH;
    target.btAddr = address;
    target.serviceClassId = kSamsungSppService;
    target.port = 0;

    const uint32_t budget = timeoutMs < 500 ? 500 : timeoutMs;
    const ULONGLONG deadline = GetTickCount64() + budget;

    u_long nonBlocking = 1;
    ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
    int rc = connect(socketHandle, (const SOCKADDR*)&target, sizeof(target));
    bool connected = rc == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(socketHandle, &writeSet);
        // B01：连接预算最长 2500 ms，按 200 ms 切片等待，Stop() 时立刻放弃。
        while (!iocancel::Requested()) {
            const ULONGLONG remaining = deadline > GetTickCount64()
                                            ? deadline - GetTickCount64()
                                            : 0;
            if (remaining == 0) break;
            timeval tv{};
            const ULONGLONG slice = remaining > 200 ? 200 : remaining;
            tv.tv_sec = (long)(slice / 1000);
            tv.tv_usec = (long)((slice % 1000) * 1000);
            const int selected = select(0, nullptr, &writeSet, nullptr, &tv);
            if (selected > 0) {
                int socketError = 0;
                int socketErrorLen = sizeof(socketError);
                if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR, (char*)&socketError,
                               &socketErrorLen) == 0 &&
                    socketError == 0) {
                    connected = true;
                }
                break;
            }
            if (selected == 0) continue;  // 切片超时：继续等剩余预算
            break;                        // select 出错
        }
    }
    if (!connected) {
        closesocket(socketHandle);
        return false;
    }

    nonBlocking = 0;
    ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
    DWORD recvTimeout = 300;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout,
               sizeof(recvTimeout));

    std::vector<uint8_t> buffer;
    buffer.reserve(2048);
    uint8_t chunk[2048];
    bool ok = false;
    while (GetTickCount64() < deadline) {
        // B01：Manager::Stop() 时不再继续读；recv 的 SO_RCVTIMEO 是 300 ms，
        // 所以最坏也只多等一个切片就退出这条 2500 ms 预算的查询。
        if (iocancel::Requested()) break;
        size_t offset = 0;
        while (offset < buffer.size()) {
            size_t consumed = 0;
            SamsungBudsStatus status;
            const SamsungFrameParse result = ParseSamsungBudsFrame(
                buffer.data() + offset, buffer.size() - offset, consumed, status);
            if (result == SamsungFrameParse::NeedMore) break;
            if (result == SamsungFrameParse::Status) {
                out = status;
                ok = true;
                break;
            }
            offset += consumed ? consumed : 1;
        }
        if (ok) break;
        if (offset > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + (ptrdiff_t)offset);
        }
        if (buffer.size() > kMaxPayloadGuard) break;

        const int received = recv(socketHandle, (char*)chunk, sizeof(chunk), 0);
        if (received > 0) {
            buffer.insert(buffer.end(), chunk, chunk + received);
        } else if (received == 0) {
            break;
        } else {
            const int error = WSAGetLastError();
            if (error != WSAETIMEDOUT) break;
        }
    }

    closesocket(socketHandle);
    return ok;
}

bool GetSamsungBudsStatusCached(const std::wstring& addressHex, SamsungBudsStatus& out) {
    const std::wstring key = NormalizeAddress(addressHex);
    if (key.size() != 12) return false;

    constexpr ULONGLONG kOkTtlMs = 60 * 1000ULL;
    constexpr ULONGLONG kFailBackoffMs = 30 * 1000ULL;
    const ULONGLONG now = GetTickCount64();

    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        auto it = g_statusCache.find(key);
        if (it != g_statusCache.end()) {
            if (it->second.haveOk && now - it->second.lastOk < kOkTtlMs) {
                out = it->second.status;
                return true;
            }
            if (it->second.lastFail != 0 && now - it->second.lastFail < kFailBackoffMs) {
                if (it->second.haveOk) {
                    out = it->second.status;
                    return true;
                }
                return false;
            }
        }
    }

    SamsungBudsStatus fresh;
    const bool ok = QuerySamsungBudsStatus(key, fresh);

    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        StatusCacheEntry& entry = g_statusCache[key];
        if (ok) {
            entry.status = fresh;
            entry.lastOk = GetTickCount64();
            entry.haveOk = true;
            out = fresh;
            return true;
        }
        entry.lastFail = GetTickCount64();
        if (entry.haveOk) {
            out = entry.status;
            return true;
        }
    }
    return false;
}
