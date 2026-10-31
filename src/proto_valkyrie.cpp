#include "proto_valkyrie.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace {

constexpr uint8_t kCmdIdentity = 0x10;
constexpr uint8_t kCmdPolling = 0x12;
constexpr uint8_t kCmdDpi = 0x13;

bool IsValkyrieId(uint16_t vid, uint16_t pid) {
    if (vid == 0x248A &&
        (pid == 0x5D2E || pid == 0x5C2E || pid == 0x5E2E || pid == 0x5C2F)) {
        return true;
    }
    return vid == 0x249A && pid == 0x5C2F;
}

bool IsWiredPid(uint16_t pid) { return pid == 0x5D2E || pid == 0x5C2E || pid == 0x5E2E; }

uint8_t PayloadChecksum(const uint8_t* p) {
    uint8_t sum = 0;
    for (int i = 4; i <= 30; ++i) sum += p[i];
    return sum;
}

bool ValidReply(const uint8_t* p, uint8_t cmd, uint8_t header3) {
    if (p[0] != cmd || p[1] != 0 || p[2] != 1 || p[3] != header3) return false;
    return p[31] == PayloadChecksum(p);
}

void DrainInput(HidHandle& in) {
    // 2.4G 接收器会延迟/重复返回上一轮报文；发新命令前先清空队列，
    // 否则会把旧的 0x10 电量帧当成新读数，出现 90%->87% 的跳变。
    uint8_t junk[128] = {};
    for (int i = 0; i < 16; ++i) {
        if (in.readRaw(junk, sizeof(junk), 1) <= 0) break;
    }
}

bool SendCommand(HidHandle& out, HidHandle& in, uint8_t cmd, uint8_t payload[32],
                 DWORD timeout = 700) {
    DrainInput(in);
    // Native HID uses a leading zero report ID; the codec payload starts at
    // tx[1]. This is the 33-byte frame used by the vendor driver.
    uint8_t tx[33] = {};
    tx[0] = 0;
    tx[1] = cmd;
    if (!out.sendRaw(tx, sizeof(tx), 500)) return false;

    uint8_t rx[64] = {};
    int n = in.readMatching(rx, sizeof(rx), [&](const uint8_t* b, int len) {
        if (len >= 33 && b[0] == 0 && b[1] == cmd) return true;
        return len >= 32 && b[0] == cmd;
    }, timeout);
    if (n <= 0) return false;

    const uint8_t* p = nullptr;
    if (n >= 33 && rx[0] == 0 && rx[1] == cmd) {
        p = rx + 1;
    } else if (n >= 32 && rx[0] == cmd) {
        p = rx;
    }
    if (!p) return false;
    memcpy(payload, p, 32);
    return true;
}

int DecodeDpiRaw(int raw, uint8_t sensor) {
    if (sensor != 0x11 || raw <= 0) return 0;
    if (raw < 200) return raw * 50;
    if (raw <= 220) return (raw - 100) * 100;
    if (raw <= 232) return (raw - 208) * 1000;
    return 0;
}

}  // namespace

bool ParseValkyrieIdentity(const uint8_t* payload, size_t len, ValkyrieIdentity& out) {
    if (!payload || len != 32) return false;
    if (payload[0] != kCmdIdentity || payload[1] != 0 || payload[2] != 1 ||
        payload[3] != 0x0B) {
        return false;
    }
    if (payload[31] != PayloadChecksum(payload)) return false;
    if (payload[13] > 100) return false;

    for (int i = 4; i < 8; ++i) {
        if (payload[i] < 0x20 || payload[i] > 0x7E) return false;
    }

    out.modelCode.assign(reinterpret_cast<const char*>(payload + 4), 4);
    out.firmware = (uint16_t)(payload[8] | (payload[9] << 8));
    out.sensor = payload[10];
    out.sensorVariant = payload[11];
    out.batteryStatus = payload[12];
    out.batteryPercent = payload[13];
    out.connectionStatus = payload[14];
    return true;
}

std::wstring ValkyrieModelName(const std::string& modelCode) {
    if (modelCode == "M220") return L"Valkyrie VK M3 Lite";
    if (modelCode == "E023") return L"Valkyrie VK M3";
    return {};
}

namespace proto {

void ReadValkyrie(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    // The vendor control channel is interface 2, usage 0x0001:0x0000,
    // 32-byte payload + report ID. Both wired (248A:5D2E) and the 2.4G
    // receiver (249A:5C2F) expose the same channel and identity reply.
    std::map<std::wstring, HidInterface> groups;
    for (const auto& h : all) {
        if (!IsValkyrieId(h.vid, h.pid)) continue;
        if (h.usagePage != 0x0001 || h.usage != 0x0000) continue;
        if (h.inLen < 32 || h.outLen < 32) continue;
        groups.emplace(h.basePath, h);
    }

    for (const auto& [base, iface] : groups) {
        HidHandle outH, inH;
        if (!outH.Open(iface, GENERIC_READ | GENERIC_WRITE, false) ||
            !inH.Open(iface, GENERIC_READ, true)) {
            continue;
        }

        uint8_t identity[32] = {};
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (attempt) Sleep(80);
            ok = SendCommand(outH, inH, kCmdIdentity, identity);
        }
        ValkyrieIdentity id;
        if (!ok || !ParseValkyrieIdentity(identity, sizeof(identity), id)) continue;

        // 249A:5C2F / 248A:5D2E 也被其它代工方案使用；只接受已知型号码，
        // 或产品名明确带 VK/Valkyrie 的设备，避免把别的鼠标误标成瓦尔基里。
        std::wstring modelName = ValkyrieModelName(id.modelCode);
        if (modelName.empty() && !ContainsNoCase(iface.product, L"VK") &&
            !ContainsNoCase(iface.product, L"Valkyrie")) {
            continue;
        }

        Device d;
        d.id = L"V:" + base;
        d.identity = id.modelCode == "M220"
                         ? L"valkyrie:vk-m3-lite"
                         : (id.modelCode == "E023" ? L"valkyrie:vk-m3" : L"");
        d.proto = Proto::Valkyrie;
        d.vid = iface.vid;
        d.pid = iface.pid;
        d.basePath = iface.basePath;
        d.path = iface.path;
        d.ifaces = all;
        d.name = modelName;
        if (d.name.empty()) {
            d.name = iface.product.empty() || ContainsNoCase(iface.product, L"receiver")
                         ? L"Valkyrie 无线鼠标"
                         : iface.product;
        }
        d.product = iface.product;
        d.manufacturer = L"Valkyrie / XCTECH";
        d.kind = Kind::Mouse;
        d.conn = IsWiredPid(iface.pid) ? Conn::Wired : Conn::Wireless24;
        d.online = true;
        d.battery = id.batteryPercent;
        d.batteryValid = true;
        // 固件标志位 0 在接收器链路为 0、有线链路为 1；按“充电/有线”处理。
        d.charging = (id.batteryStatus & 0x01) != 0;
        d.note = Fmt(L"%s · 固件 %04X · 设备原始电量/轮询率只读", d.name.c_str(), id.firmware);

        uint8_t poll[32] = {};
        if (SendCommand(outH, inH, kCmdPolling, poll) && ValidReply(poll, kCmdPolling, 0x01)) {
            const int code = poll[4];
            int hz = 0;
            if (code == 1) hz = 1000;
            else if (code == 2) hz = 500;
            else if (code == 4) hz = 250;
            else if (code == 8) hz = 125;
            if (hz) {
                d.polling.valid = true;
                d.polling.canWrite = false;
                d.polling.currentHz = hz;
                d.polling.options = {125, 250, 500, 1000};
            }
        }

        uint8_t dpiReply[32] = {};
        if (SendCommand(outH, inH, kCmdDpi, dpiReply) &&
            ValidReply(dpiReply, kCmdDpi, 0x19)) {
            const int count = dpiReply[4] & 0x0F;
            int active = (dpiReply[4] >> 4) & 0x0F;
            if (count >= 1 && count <= 6) {
                std::vector<int> xs, ys;
                for (int i = 0; i < count; ++i) {
                    int rawX = dpiReply[5 + i * 4] | (dpiReply[6 + i * 4] << 8);
                    int rawY = dpiReply[7 + i * 4] | (dpiReply[8 + i * 4] << 8);
                    int x = DecodeDpiRaw(rawX, id.sensor);
                    int y = DecodeDpiRaw(rawY, id.sensor);
                    if (x > 0) {
                        xs.push_back(x);
                        ys.push_back(y > 0 ? y : x);
                    }
                }
                if (!xs.empty()) {
                    if (active < 0 || active >= (int)xs.size()) active = 0;
                    d.dpi.valid = true;
                    d.dpi.canWrite = false;
                    d.dpi.currentKnown = true;
                    d.dpi.stages = xs;
                    d.dpi.activeStage = active;
                    d.dpi.x = xs[active];
                    d.dpi.y = ys[active];
                    d.dpi.min = *std::min_element(xs.begin(), xs.end());
                    d.dpi.max = *std::max_element(xs.begin(), xs.end());
                    d.dpi.step = 50;
                    d.dpi.note = L"Valkyrie VK M3：DPI/轮询率只读";
                }
            }
        }

        out.push_back(std::move(d));
    }
}

}  // namespace proto
