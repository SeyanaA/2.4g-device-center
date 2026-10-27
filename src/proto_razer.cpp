#include "protocols.h"

#include <cstring>
#include <map>

namespace {

constexpr uint16_t kRazerVid = 0x1532;

int RazerPollHzStd(uint8_t code) {
    switch (code) {
        case 0x01: return 1000;
        case 0x02: return 500;
        case 0x08: return 125;
        default: return 0;
    }
}

int RazerPollCodeStd(int hz) {
    switch (hz) {
        case 1000: return 0x01;
        case 500: return 0x02;
        case 125: return 0x08;
        default: return 0;
    }
}

int RazerPollHzExt(uint8_t code) {
    switch (code) {
        case 0x01: return 8000;
        case 0x02: return 4000;
        case 0x04: return 2000;
        case 0x08: return 1000;
        case 0x10: return 500;
        case 0x40: return 125;
        default: return 0;
    }
}

int RazerPollCodeExt(int hz) {
    switch (hz) {
        case 8000: return 0x01;
        case 4000: return 0x02;
        case 2000: return 0x04;
        case 1000: return 0x08;
        case 500: return 0x10;
        case 125: return 0x40;
        default: return 0;
    }
}

struct RazerLink {
    HidHandle h;
    bool open = false;
    uint8_t txCache[256] = {};
    bool txKnown[256] = {};

    bool Open(const HidInterface& iface) {
        open = h.Open(iface, GENERIC_READ | GENERIC_WRITE, false);
        return open;
    }

    void Build(uint8_t tx, uint8_t cls, uint8_t id, uint8_t size, const uint8_t* args,
               size_t argLen, uint8_t report[90]) {
        memset(report, 0, 90);
        report[0] = 0;
        report[1] = tx;
        report[5] = size;
        report[6] = cls;
        report[7] = id;
        if (args && argLen) memcpy(report + 8, args, std::min<size_t>(argLen, 80));
        uint8_t crc = 0;
        for (int i = 2; i < 88; ++i) crc ^= report[i];
        report[88] = crc;
    }

    bool Exchange(uint8_t tx, uint8_t cls, uint8_t id, uint8_t size, const uint8_t* args,
                  size_t argLen, uint8_t out[90], DWORD timeout = 650) {
        if (!open) return false;
        uint8_t report[90];
        Build(tx, cls, id, size, args, argLen, report);
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (!h.setFeature(0, report, sizeof(report), timeout)) return false;
            Sleep(attempt == 0 ? 55 : 35);
            size_t rlen = h.info().featLen ? h.info().featLen : 91;
            if (rlen < 90) return false;
            uint8_t raw[128] = {};
            if (!h.getFeature(0, raw, rlen, timeout)) continue;
            int off = -1;
            if (rlen >= 91 && raw[1 + 6] == cls && raw[1 + 7] == id) off = 1;
            else if (raw[6] == cls && raw[7] == id) off = 0;
            if (off < 0) continue;
            if (rlen < (size_t)off + 90) continue;
            const uint8_t* r = raw + off;
            if (r[0] == 0x00 || r[0] == 0x01) {
                Sleep(35);
                continue;
            }
            if (r[0] != 0x02) return false;
            memcpy(out, r, 90);
            return true;
        }
        return false;
    }

    bool ProbeClassTx(uint8_t cls, uint8_t tx) {
        uint8_t out[90];
        uint8_t arg = 1;
        bool ok = false;
        if (cls == 0x00) {
            ok = Exchange(tx, 0x00, 0x81, 0x02, nullptr, 0, out, 450);
        } else if (cls == 0x07) {
            ok = Exchange(tx, 0x07, 0x80, 0x02, nullptr, 0, out, 450);
        } else if (cls == 0x04) {
            ok = Exchange(tx, 0x04, 0x85, 0x07, &arg, 1, out, 450);
        }
        return ok;
    }

    uint8_t TxForClass(uint8_t cls) {
        if (txKnown[cls]) return txCache[cls];
        static const uint8_t candidates[] = {0xFF, 0x1F, 0x3F, 0x08, 0x00};
        for (uint8_t tx : candidates) {
            if (ProbeClassTx(cls, tx)) {
                txCache[cls] = tx;
                txKnown[cls] = true;
                return tx;
            }
        }
        uint8_t fallback = txKnown[0x00] ? txCache[0x00] : 0xFF;
        txCache[cls] = fallback;
        txKnown[cls] = true;
        return fallback;
    }

    bool Cmd(uint8_t cls, uint8_t id, uint8_t size, const uint8_t* args, size_t argLen,
             uint8_t out[90]) {
        return Exchange(TxForClass(cls), cls, id, size, args, argLen, out);
    }
};

struct Group {
    std::wstring base;
    uint16_t pid = 0;
    std::wstring product;
    std::vector<HidInterface> ifaces;
};

std::vector<Group> BuildGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, Group> map;
    for (const auto& h : all) {
        if (h.vid != kRazerVid) continue;
        if (h.featLen < 90) continue;
        Group& g = map[h.basePath];
        g.base = h.basePath;
        g.pid = h.pid;
        if (g.product.empty()) g.product = h.product;
        g.ifaces.push_back(h);
    }
    std::vector<Group> out;
    for (auto& [k, g] : map) out.push_back(g);
    return out;
}

std::wstring RazerName(const Group& g) {
    if (!g.product.empty() && !ContainsNoCase(g.product, L"razer mouse")) return g.product;
    switch (g.pid) {
        case 0x007B: return L"Razer Viper Ultimate (接收器)";
        case 0x007A: return L"Razer Viper Ultimate";
        case 0x00A6: return L"Razer Viper V2 Pro (接收器)";
        case 0x00A5: return L"Razer Viper V2 Pro";
        case 0x009F: return L"Razer Viper Mini SE (接收器)";
        case 0x009E: return L"Razer Viper Mini SE";
        default: return L"Razer 设备";
    }
}

bool RazerReadBattery(RazerLink& link, int& pct, bool& charging) {
    uint8_t out[90] = {};
    pct = -1;
    charging = false;
    if (link.Cmd(0x07, 0x80, 0x02, nullptr, 0, out)) {
        int raw = out[9];
        if (raw > 0) {
            pct = (raw * 100 + 127) / 255;
            if (pct > 100) pct = 100;
        }
    }
    uint8_t charge[90] = {};
    if (link.Cmd(0x07, 0x84, 0x02, nullptr, 0, charge)) {
        charging = charge[9] != 0;
    }
    return pct > 0 || charging;
}

bool RazerReadDpi(RazerLink& link, DpiInfo& dpi) {
    uint8_t arg = 1;
    uint8_t out[90] = {};
    if (!link.Cmd(0x04, 0x85, 0x07, &arg, 1, out)) return false;
    int x = (out[9] << 8) | out[10];
    int y = (out[11] << 8) | out[12];
    if (x < 100 || x > 100000) return false;
    dpi.valid = true;
    dpi.canWrite = true;
    dpi.currentKnown = true;
    dpi.x = x;
    dpi.y = y >= 100 ? y : x;
    dpi.min = 100;
    dpi.max = 45000;
    dpi.step = 50;
    dpi.activeStage = -1;
    return true;
}

bool RazerReadPolling(RazerLink& link, PollingInfo& info) {
    uint8_t out[90] = {};
    if (link.Cmd(0x00, 0xC0, 0x01, nullptr, 0, out)) {
        int hz = RazerPollHzExt(out[8]);
        if (hz > 0) {
            info.valid = true;
            info.canWrite = true;
            info.currentHz = hz;
            info.options = {125, 500, 1000, 2000, 4000, 8000};
            return true;
        }
    }
    if (link.Cmd(0x00, 0x85, 0x01, nullptr, 0, out)) {
        int hz = RazerPollHzStd(out[8]);
        if (hz > 0) {
            info.valid = true;
            info.canWrite = true;
            info.currentHz = hz;
            info.options = {125, 500, 1000};
            return true;
        }
    }
    return false;
}

}  // namespace

namespace proto {

void ReadRazer(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    auto groups = BuildGroups(all);
    for (const auto& g : groups) {
        RazerLink link;
        HidInterface chosen{};
        bool found = false;
        for (const auto& iface : g.ifaces) {
            link = RazerLink();
            if (!link.Open(iface)) continue;
            if (link.ProbeClassTx(0x00, 0xFF) || link.ProbeClassTx(0x00, 0x1F) ||
                link.ProbeClassTx(0x00, 0x3F)) {
                chosen = iface;
                found = true;
                break;
            }
        }
        if (!found) continue;
        link = RazerLink();
        if (!link.Open(chosen)) continue;

        Device d;
        d.id = L"R:" + g.base;
        d.proto = Proto::Razer;
        d.vid = kRazerVid;
        d.pid = g.pid;
        d.basePath = g.base;
        d.path = chosen.path;
        d.ifaces = all;
        d.name = RazerName(g);
        d.product = g.product;
        d.manufacturer = L"Razer";
        d.kind = ContainsNoCase(g.product, L"keyboard")
                     ? Kind::Keyboard
                     : (ContainsNoCase(g.product, L"headset") ? Kind::Headset : Kind::Mouse);
        d.conn = (ContainsNoCase(g.product, L"dongle") ||
                  ContainsNoCase(g.product, L"receiver") ||
                  ContainsNoCase(g.product, L"hyperpolling"))
                     ? Conn::Wireless24
                     : Conn::Wired;
        d.online = true;
        d.batteryValid = RazerReadBattery(link, d.battery, d.charging);
        d.dpi = {};
        RazerReadDpi(link, d.dpi);
        d.polling = {};
        RazerReadPolling(link, d.polling);
        if (!d.batteryValid && !d.dpi.valid) {
            d.note = L"设备有响应，但电量/DPI 未返回（可能休眠或 Synapse 占用）";
        }
        out.push_back(std::move(d));
    }
}

bool ApplyRazer(const Device& dev, const ApplyRequest& req, std::wstring& err) {
    HidInterface iface{};
    bool found = false;
    for (const auto& h : dev.ifaces) {
        if (h.path == dev.path && h.featLen >= 90) {
            iface = h;
            found = true;
            break;
        }
    }
    if (!found) {
        err = L"找不到 Razer HID 接口";
        return false;
    }
    RazerLink link;
    if (!link.Open(iface)) {
        err = L"无法打开 Razer 设备";
        return false;
    }
    bool ok = false;
    uint8_t out[90] = {};
    if (req.setDpi) {
        int x = ClampInt(req.dpiX, 100, 45000);
        int y = ClampInt(req.dpiY > 0 ? req.dpiY : x, 100, 45000);
        uint8_t args[7] = {1, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(y >> 8), (uint8_t)y, 0, 0};
        if (link.Cmd(0x04, 0x05, 0x07, args, 7, out)) ok = true;
        else err = L"写入 DPI 失败";
    }
    if (req.setPolling && req.pollingHz > 0) {
        if (req.pollingHz <= 1000) {
            uint8_t code = (uint8_t)RazerPollCodeStd(req.pollingHz);
            uint8_t args[1] = {code};
            if (link.Cmd(0x00, 0x05, 0x01, args, 1, out)) ok = true;
            else err = L"写入轮询率失败";
        } else {
            uint8_t code = (uint8_t)RazerPollCodeExt(req.pollingHz);
            uint8_t args[2] = {0, code};
            if (link.Cmd(0x00, 0x40, 0x02, args, 2, out)) ok = true;
            else err = L"写入高轮询率失败";
        }
    }
    if (!ok && err.empty()) err = L"设备没有接受任何设置";
    return ok;
}

}  // namespace proto
