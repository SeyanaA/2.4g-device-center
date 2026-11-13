#include "proto_morebetter.h"

#include <hidsdi.h>

#include <map>

namespace {

constexpr uint16_t kMoreBetterVid = 0x1EA7;
constexpr uint16_t kMoreBetterPid = 0x0064;

std::map<std::wstring, int> g_lastPercent;

bool ReadBatteryFromHandle(HANDLE h, int& percent) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;

        uint8_t rx[8] = {};
        DWORD got = 0;
        BOOL readOk = ReadFile(h, rx, sizeof(rx), &got, &ov);
        bool pending = !readOk && GetLastError() == ERROR_IO_PENDING;

        uint8_t tx[8] = {0xB5, 0x01, 0, 0, 0, 0, 0, 0};
        BOOL txOk = HidD_SetOutputReport(h, tx, sizeof(tx));

        DWORD wait = WaitForSingleObject(ov.hEvent, 900);
        // 固件在超时后仍可能持有 IRP；先取消并冲刷，再关句柄。
        CancelIoEx(h, &ov);
        DWORD got2 = 0;
        if (!GetOverlappedResult(h, &ov, &got2, TRUE)) got2 = got;
        CloseHandle(ov.hEvent);

        if (wait == WAIT_OBJECT_0 && txOk && got2 >= 4 &&
            ParseMoreBetterBattery(rx, got2, percent)) {
            return true;
        }
        if (readOk && txOk && got >= 4 && ParseMoreBetterBattery(rx, got, percent)) {
            return true;
        }
        if (!pending) {
            // 非异步完成但也没有数据，稍后重试。
        }
        Sleep(120);
    }
    return false;
}

}  // namespace

bool ParseMoreBetterBattery(const uint8_t* buf, size_t len, int& percent) {
    if (!buf || len < 4) return false;
    if (buf[0] != 0xB5 || buf[1] != 0x01) return false;
    if (buf[3] > 100) return false;
    percent = buf[3];
    return true;
}

namespace proto {

void ReadMoreBetter(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    // 1EA7:0064 是常见廉价 2.4G 鼠标方案；Col01 厂商页 0xFFB5、8 字节
    // 请求/应答。固件忽略中断 OUT 的 WriteFile，必须用 HidD_SetOutputReport。
    std::map<std::wstring, HidInterface> groups;
    for (const auto& h : all) {
        if (h.vid != kMoreBetterVid || h.pid != kMoreBetterPid) continue;
        if (h.usagePage != 0xFFB5 || h.usage != 0x0001) continue;
        if (h.inLen < 8 || h.outLen < 8) continue;
        groups.emplace(h.basePath, h);
    }

    for (const auto& [base, iface] : groups) {
        int percent = -1;
        bool ok = false;

        HANDLE h = CreateFileW(iface.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ok = ReadBatteryFromHandle(h, percent);
            CloseHandle(h);
        }

        Device d;
        d.id = L"M:" + base;
        d.proto = Proto::MoreBetter;
        d.vid = iface.vid;
        d.pid = iface.pid;
        d.basePath = iface.basePath;
        d.path = iface.path;
        d.ifaces = all;
        d.name = iface.product.empty() ? L"2.4G 公版鼠标" : iface.product;
        d.product = iface.product;
        d.manufacturer = L"More&Better / SHARKOON";
        d.kind = Kind::Mouse;
        d.conn = Conn::Wireless24;
        d.online = ok;
        d.batteryValid = ok;
        d.battery = ok ? percent : -1;
        // 2.4G 链路没有充电标志；USB-C 只供电不传输数据，无法从主机侧判断充电。
        d.chargingKnown = false;

        if (ok) {
            // 该 2.4G 链路没有充电标志位；按开源实现用“连续上升/满电”推断。
            bool charging = percent >= 100;
            auto it = g_lastPercent.find(base);
            if (it != g_lastPercent.end() && percent > it->second) charging = true;
            g_lastPercent[base] = percent;
            d.charging = charging;
            d.note = Fmt(L"%s · 电量只读 · 充电状态不可读（电量计可能冻结）",
                         d.name.c_str());
        } else {
            d.note = L"接收器在线，鼠标休眠或未连接";
        }

        out.push_back(std::move(d));
    }
}

}  // namespace proto
