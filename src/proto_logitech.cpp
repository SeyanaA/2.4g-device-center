#include "protocols.h"

#include <cstring>
#include <map>

namespace {

constexpr uint16_t kLogiVid = 0x046D;
constexpr uint8_t kSwId = 0x0A;

bool IsReceiverPid(uint16_t pid) {
    switch (pid) {
        case 0xC52B: case 0xC532: case 0xC517: case 0xC51B:
        case 0xC526: case 0xC52E: case 0xC531: case 0xC542:
        case 0xC52F: case 0xC534: case 0xC53D:
        case 0xC539: case 0xC53A: case 0xC53F: case 0xC547:
        case 0xC54D: case 0xC541: case 0xC545:
        case 0xC548:
            return true;
        default:
            return false;
    }
}

Kind KindFromPairingType(uint8_t type) {
    switch (type & 0x0F) {
        case 0x01: return Kind::Keyboard;
        case 0x02: return Kind::Mouse;
        case 0x03: return Kind::Keyboard;  // numpad
        case 0x04: return Kind::Other;     // presenter
        case 0x07: return Kind::Other;     // remote
        case 0x08: return Kind::Mouse;     // trackball
        case 0x09: return Kind::Other;     // touchpad
        case 0x0A: return Kind::Other;     // tablet
        case 0x0B: return Kind::Other;     // gamepad
        case 0x0C: return Kind::Other;     // joystick
        default: return Kind::Other;
    }
}

Kind KindFromDeviceType(uint8_t type) {
    switch (type) {
        case 0: return Kind::Keyboard;
        case 2: return Kind::Keyboard;
        case 3: return Kind::Mouse;
        case 4: return Kind::Other;
        case 5: return Kind::Mouse;
        case 10: return Kind::Headset;
        default: return Kind::Other;
    }
}

int ExtRateHz(uint8_t code) {
    static const int rates[] = {125, 250, 500, 1000, 2000, 4000, 8000};
    return code < ARRAYSIZE(rates) ? rates[code] : 0;
}

struct DpiSeg {
    int from = 0;
    int to = 0;
    int step = 0;
};

std::vector<DpiSeg> ParseDpiList(const uint8_t* p, size_t n) {
    std::vector<DpiSeg> out;
    int prev = -1;
    int pending = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        int v = (p[i] << 8) | p[i + 1];
        if (v == 0) break;
        if ((v & 0xE000) == 0xE000) {
            pending = v & 0x1FFF;
            continue;
        }
        if (prev >= 0 && pending > 0) {
            out.push_back({prev, v, pending});
        } else {
            out.push_back({v, v, 1});
        }
        pending = 0;
        prev = v;
    }
    if (out.size() > 1 && out[0].from == out[0].to && out[0].to == out[1].from) {
        out.erase(out.begin());
    }
    return out;
}

struct Group {
    std::wstring base;
    uint16_t pid = 0;
    std::wstring product;
    HidInterface shortIface{};
    HidInterface longIface{};
    bool hasShort = false;
    bool hasLong = false;
    uint16_t usagePage = 0;
};

class Link {
public:
    bool Open(const Group& g) {
        if (g.hasLong) {
            longH_.Open(g.longIface, GENERIC_READ | GENERIC_WRITE, true);
        }
        if (g.hasShort) {
            shortH_.Open(g.shortIface, GENERIC_READ | GENERIC_WRITE, true);
        }
        return longH_.valid() || shortH_.valid();
    }

    bool Call(uint8_t idx, uint8_t feat, uint8_t func, const uint8_t* params, size_t plen,
              uint8_t out[16], DWORD timeout = 900) {
        if (!longH_.valid()) return false;
        uint8_t req[20] = {};
        req[0] = 0x11;
        req[1] = idx;
        req[2] = feat;
        req[3] = (uint8_t)((func << 4) | kSwId);
        if (params && plen) memcpy(req + 4, params, std::min<size_t>(plen, 16));
        if (!longH_.sendRaw(req, sizeof(req), 400)) return false;

        uint8_t addr = (uint8_t)((func << 4) | kSwId);
        int n = ReadReply(idx, feat, addr, out, timeout);
        return n > 0;
    }

    bool Register(uint8_t sub, uint8_t reg, const uint8_t* params, size_t plen,
                  std::vector<uint8_t>& out, DWORD timeout = 700) {
        uint8_t req[7] = {0x10, 0xFF, sub, reg, 0, 0, 0};
        if (params && plen) memcpy(req + 4, params, std::min<size_t>(plen, 3));
        HidHandle& h = shortH_.valid() ? shortH_ : longH_;
        if (!h.valid()) return false;
        if (!h.sendRaw(req, sizeof(req), 400)) return false;
        uint8_t buf[64];
        int n = h.readMatching(buf, sizeof(buf), [&](const uint8_t* b, int len) {
            if (len < 4 || b[1] != 0xFF) return false;
            if (b[2] == 0x8F && b[3] == sub) return true;
            return b[2] == sub && b[3] == reg;
        }, timeout);
        if (n < 6) return false;
        if (buf[2] == 0x8F) return false;
        out.assign(buf, buf + n);
        return true;
    }

    bool Ping(uint8_t idx, bool& online) {
        uint8_t params[3] = {0, 0, 0x55};
        uint8_t r[16] = {};
        if (!Call(idx, 0x00, 1, params, sizeof(params), r, 700)) {
            online = false;
            return false;
        }
        online = true;
        return true;
    }

    bool FeatureIndex(uint8_t idx, uint16_t feature, uint8_t& fi) {
        uint8_t p[2] = {(uint8_t)(feature >> 8), (uint8_t)feature};
        uint8_t r[16] = {};
        if (!Call(idx, 0x00, 0, p, 2, r)) return false;
        if (r[0] == 0) return false;
        fi = r[0];
        return true;
    }

    bool DeviceName(uint8_t idx, std::wstring& name) {
        uint8_t f = 0;
        if (!FeatureIndex(idx, 0x0005, f)) return false;
        uint8_t r[16] = {};
        if (!Call(idx, f, 0, nullptr, 0, r)) return false;
        int count = r[0];
        if (count <= 0 || count > 128) return false;
        std::string raw;
        while ((int)raw.size() < count) {
            uint8_t p[1] = {(uint8_t)raw.size()};
            if (!Call(idx, f, 1, p, 1, r)) break;
            int take = std::min(16, count - (int)raw.size());
            raw.append((const char*)r, take);
        }
        name = Trim(Utf8ToWide(raw));
        return !name.empty();
    }

    bool DeviceType(uint8_t idx, Kind& kind) {
        uint8_t f = 0;
        if (!FeatureIndex(idx, 0x0005, f)) return false;
        uint8_t r[16] = {};
        if (!Call(idx, f, 2, nullptr, 0, r)) return false;
        kind = KindFromDeviceType(r[0]);
        return true;
    }

    bool Battery(uint8_t idx, int& pct, bool& charging, int& mv, bool& valid) {
        uint8_t f = 0;
        pct = -1;
        charging = false;
        mv = 0;
        valid = false;
        if (FeatureIndex(idx, 0x1004, f)) {
            uint8_t r[16] = {};
            if (Call(idx, f, 1, nullptr, 0, r)) {
                int level = r[0];
                uint8_t approx = r[1];
                uint8_t status = r[2];
                if (level <= 0) {
                    if (status == 3) level = 100;
                    else if (approx == 8) level = 100;
                    else if (approx == 4) level = 75;
                    else if (approx == 2) level = 30;
                    else if (approx == 1) level = 10;
                }
                if (level > 0 && level <= 100) {
                    pct = level;
                    valid = true;
                }
                charging = (status == 1 || status == 2);
            }
        }
        if (!valid && FeatureIndex(idx, 0x1000, f)) {
            uint8_t r[16] = {};
            if (Call(idx, f, 0, nullptr, 0, r)) {
                if (r[0] > 0 && r[0] <= 100) {
                    pct = r[0];
                    valid = true;
                } else if (r[2] == 3) {
                    pct = 100;
                    valid = true;
                }
                charging = (r[2] == 1 || r[2] == 2 || r[2] == 4 || r[2] == 5);
            }
        }
        if (FeatureIndex(idx, 0x1001, f)) {
            uint8_t r[16] = {};
            if (Call(idx, f, 0, nullptr, 0, r)) {
                mv = (r[0] << 8) | r[1];
                if (r[2] & 0x80) charging = true;
            }
        }
        return valid;
    }

    bool Dpi(uint8_t idx, DpiInfo& dpi) {
        uint8_t f = 0;
        if (FeatureIndex(idx, 0x2202, f)) {
            uint8_t r[16] = {};
            uint8_t p[1] = {0};
            if (!Call(idx, f, 5, p, 1, r)) return false;
            int x = (r[1] << 8) | r[2];
            if (x == 0) x = (r[3] << 8) | r[4];
            int y = (r[5] << 8) | r[6];
            if (y == 0) y = (r[7] << 8) | r[8];
            if (x <= 0 || x > 100000) return false;
            dpi.x = x;
            dpi.y = y > 0 ? y : x;
            dpi.lod = r[9];

            std::vector<uint8_t> stream;
            for (int range = 0; range < 8; ++range) {
                uint8_t lp[3] = {0, 0, (uint8_t)range};
                if (!Call(idx, f, 2, lp, 3, r)) break;
                stream.insert(stream.end(), r + 3, r + 16);
                bool zero = false;
                for (size_t i = 0; i + 1 < stream.size(); i += 2) {
                    if (stream[i] == 0 && stream[i + 1] == 0) zero = true;
                }
                if (zero) break;
            }
            auto segs = ParseDpiList(stream.data(), stream.size());
            if (!segs.empty()) {
                dpi.min = segs.front().from;
                dpi.max = segs.back().to;
                dpi.step = segs.front().step > 0 ? segs.front().step : 50;
            }
            uint8_t pp[2] = {0, 0};
            if (Call(idx, f, 3, pp, 2, r)) {
                for (int i = 2; i + 1 < 16; i += 2) {
                    int v = (r[i] << 8) | r[i + 1];
                    if (v == 0) break;
                    dpi.stages.push_back(v);
                }
            }
            dpi.valid = true;
            dpi.canWrite = true;
            dpi.currentKnown = true;
            if (dpi.stages.empty()) {
                dpi.stages.push_back(dpi.x);
            }
            dpi.activeStage = -1;
            uint8_t of = 0;
            if (FeatureIndex(idx, 0x8100, of)) {
                uint8_t rr[16] = {};
                if (Call(idx, of, 11, nullptr, 0, rr)) {
                    if (rr[0] < dpi.stages.size()) dpi.activeStage = rr[0];
                }
                uint8_t mode[16] = {};
                if (Call(idx, of, 2, nullptr, 0, mode)) {
                    if (mode[0] == 1) {
                        dpi.note = L"鼠标处于板载配置模式，主机 DPI 可能被设备覆盖";
                    }
                }
            }
            return true;
        }

        if (FeatureIndex(idx, 0x2201, f)) {
            uint8_t r[16] = {};
            uint8_t p[1] = {0};
            if (!Call(idx, f, 2, p, 1, r)) return false;
            int x = (r[1] << 8) | r[2];
            if (x <= 0 || x > 100000) return false;
            dpi.x = x;
            dpi.y = x;
            dpi.lod = 0;
            uint8_t lr[16] = {};
            if (Call(idx, f, 1, p, 1, lr)) {
                auto segs = ParseDpiList(lr + 1, 15);
                if (!segs.empty()) {
                    dpi.min = segs.front().from;
                    dpi.max = segs.back().to;
                    dpi.step = segs.front().step > 0 ? segs.front().step : 50;
                }
            }
            dpi.valid = true;
            dpi.canWrite = true;
            dpi.currentKnown = true;
            dpi.activeStage = -1;
            if (dpi.stages.empty()) dpi.stages.push_back(x);
            return true;
        }
        return false;
    }

    bool Polling(uint8_t idx, PollingInfo& info) {
        uint8_t f = 0;
        if (FeatureIndex(idx, 0x8061, f)) {
            uint8_t r[16] = {};
            uint8_t p[1] = {1};
            uint8_t ct = 1;
            if (!Call(idx, f, 2, p, 1, r)) {
                ct = 0;
                p[0] = 0;
                if (!Call(idx, f, 2, p, 1, r)) return false;
            }
            int hz = ExtRateHz(r[0]);
            if (hz <= 0) return false;
            info.currentHz = hz;
            info.valid = true;
            info.canWrite = true;
            uint8_t mask[16] = {};
            if (Call(idx, f, 1, p, 1, mask)) {
                for (int b = 0; b < 7; ++b) {
                    if (mask[1] & (1 << b)) info.options.push_back(ExtRateHz(b));
                }
            }
            if (info.options.empty()) info.options = {125, 250, 500, 1000, 2000, 4000, 8000};
            (void)ct;
            return true;
        }
        if (FeatureIndex(idx, 0x8060, f)) {
            uint8_t r[16] = {};
            if (!Call(idx, f, 1, nullptr, 0, r)) return false;
            if (r[0] == 0) return false;
            info.currentHz = 1000 / r[0];
            info.valid = true;
            info.canWrite = true;
            uint8_t mask[16] = {};
            if (Call(idx, f, 0, nullptr, 0, mask)) {
                for (int b = 0; b < 8; ++b) {
                    if (mask[0] & (1 << b)) info.options.push_back(1000 / (b + 1));
                }
            }
            if (info.options.empty()) info.options = {125, 250, 500, 1000};
            return true;
        }
        return false;
    }

    bool SetDpi(uint8_t idx, int x, int y, int lod) {
        uint8_t f = 0;
        if (FeatureIndex(idx, 0x2202, f)) {
            uint8_t p[6] = {0, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(y >> 8), (uint8_t)y,
                            (uint8_t)lod};
            uint8_t r[16] = {};
            return Call(idx, f, 6, p, sizeof(p), r);
        }
        if (FeatureIndex(idx, 0x2201, f)) {
            uint8_t p[3] = {0, (uint8_t)(x >> 8), (uint8_t)x};
            uint8_t r[16] = {};
            return Call(idx, f, 3, p, sizeof(p), r);
        }
        return false;
    }

    bool SelectStage(uint8_t idx, int stage) {
        uint8_t f = 0;
        if (!FeatureIndex(idx, 0x8100, f)) return false;
        uint8_t p[1] = {(uint8_t)stage};
        uint8_t r[16] = {};
        return Call(idx, f, 12, p, 1, r);
    }

    bool SetPolling(uint8_t idx, int hz) {
        uint8_t f = 0;
        if (FeatureIndex(idx, 0x8061, f)) {
            static const int rates[] = {125, 250, 500, 1000, 2000, 4000, 8000};
            int code = -1;
            for (int i = 0; i < (int)ARRAYSIZE(rates); ++i) {
                if (rates[i] == hz) code = i;
            }
            if (code < 0) return false;
            uint8_t p[1] = {(uint8_t)code};
            uint8_t r[16] = {};
            return Call(idx, f, 3, p, 1, r);
        }
        if (FeatureIndex(idx, 0x8060, f)) {
            int ms = 1000 / std::max(hz, 1);
            ms = ClampInt(ms, 1, 8);
            uint8_t p[1] = {(uint8_t)ms};
            uint8_t r[16] = {};
            return Call(idx, f, 2, p, 1, r);
        }
        return false;
    }

private:
    int ReadReply(uint8_t idx, uint8_t feat, uint8_t addr, uint8_t out[16], DWORD timeout) {
        ULONGLONG end = GetTickCount64() + timeout;
        uint8_t buf[64];
        while (GetTickCount64() < end) {
            HidHandle* handles[2] = {&longH_, &shortH_};
            for (auto* h : handles) {
                if (!h->valid()) continue;
                int n = h->readRaw(buf, sizeof(buf), 60);
                if (n < 4) continue;
                if (buf[1] != idx) continue;
                if (buf[2] == 0x8F) {
                    if (buf[3] == feat && buf[4] == addr) return -1;
                    continue;
                }
                if (buf[2] == 0xFF) {
                    if (buf[3] == feat && buf[4] == addr) return -1;
                    continue;
                }
                if (buf[2] == feat && buf[3] == addr) {
                    memset(out, 0, 16);
                    int take = std::min(16, n - 4);
                    if (take > 0) memcpy(out, buf + 4, take);
                    return take;
                }
            }
        }
        return 0;
    }

    HidHandle shortH_;
    HidHandle longH_;
};

std::vector<Group> BuildGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, Group> map;
    for (const auto& h : all) {
        if (h.vid != kLogiVid) continue;
        if (h.usagePage != 0xFF00 && h.usagePage != 0xFF43) continue;
        Group& g = map[h.basePath];
        g.base = h.basePath;
        g.pid = h.pid;
        g.usagePage = h.usagePage;
        if (g.product.empty()) g.product = h.product;
        bool isShort = (h.usage == 1) || (h.inLen > 0 && h.inLen <= 8);
        bool isLong = (h.usage == 2) || (h.inLen >= 20);
        if (isLong && !g.hasLong) {
            g.longIface = h;
            g.hasLong = true;
        }
        if (isShort && !g.hasShort) {
            g.shortIface = h;
            g.hasShort = true;
        }
    }
    std::vector<Group> out;
    for (auto& [k, g] : map) {
        if (!g.hasLong && g.hasShort) {
            g.longIface = g.shortIface;
            g.hasLong = true;
        }
        if (g.hasLong) out.push_back(g);
    }
    return out;
}

std::wstring ReceiverLabel(uint16_t pid) {
    switch (pid) {
        case 0xC548: return L"Bolt 接收器";
        case 0xC54D: case 0xC539: case 0xC53A: case 0xC53F: case 0xC547: case 0xC541:
        case 0xC545:
            return L"Lightspeed 接收器";
        case 0xC52F: case 0xC534: case 0xC53D: return L"Nano 接收器";
        default: return L"Unifying 接收器";
    }
}

}  // namespace

namespace proto {

void ReadLogitech(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    auto groups = BuildGroups(all);
    for (const auto& g : groups) {
        Link link;
        if (!link.Open(g)) continue;
        bool receiver = IsReceiverPid(g.pid) || ContainsNoCase(g.product, L"receiver") ||
                        ContainsNoCase(g.product, L"unifying") ||
                        ContainsNoCase(g.product, L"lightspeed") ||
                        ContainsNoCase(g.product, L"bolt");
        if (!receiver) {
            bool online = false;
            if (!link.Ping(0xFF, online) || !online) continue;
            Device d;
            d.id = L"L:" + g.base + L"#255";
            d.proto = Proto::Logitech;
            d.vid = kLogiVid;
            d.pid = g.pid;
            d.basePath = g.base;
            d.path = g.longIface.path;
            d.lgIndex = 0xFF;
            d.ifaces = all;
            d.online = true;
            d.name = g.product.empty() ? L"Logitech 设备" : g.product;
            d.product = d.name;
            d.manufacturer = L"Logitech";
            d.conn = g.usagePage == 0xFF43 || ContainsNoCase(g.product, L"bluetooth")
                         ? Conn::Bluetooth
                         : Conn::Wired;
            if (ContainsNoCase(g.product, L"receiver") || ContainsNoCase(g.product, L"dongle")) {
                d.conn = Conn::Wireless24;
            }
            std::wstring devName;
            if (link.DeviceName(0xFF, devName)) d.name = devName;
            Kind kind = Kind::Other;
            if (link.DeviceType(0xFF, kind)) d.kind = kind;
            if (d.kind == Kind::Other) d.kind = Kind::Mouse;
            link.Battery(0xFF, d.battery, d.charging, d.millivolts, d.batteryValid);
            d.dpi = {};
            link.Dpi(0xFF, d.dpi);
            d.polling = {};
            link.Polling(0xFF, d.polling);
            out.push_back(std::move(d));
            continue;
        }

        bool any = false;
        for (int slot = 0; slot < 6; ++slot) {
            uint8_t idx = (uint8_t)(slot + 1);
            std::vector<uint8_t> info;
            uint8_t p[1] = {(uint8_t)(0x20 | slot)};
            if (!link.Register(0x83, 0xB5, p, 1, info)) continue;
            if (info.size() < 12) continue;
            uint16_t wpid = (info[7] << 8) | info[8];
            if (wpid == 0) continue;
            uint8_t type = info[11] & 0x0F;

            Device d;
            d.id = L"L:" + g.base + L"#" + std::to_wstring(idx);
            d.proto = Proto::Logitech;
            d.vid = kLogiVid;
            d.pid = wpid;
            d.basePath = g.base;
            d.path = g.longIface.path;
            d.lgIndex = idx;
            d.ifaces = all;
            d.manufacturer = L"Logitech";
            d.conn = Conn::Wireless24;
            d.kind = KindFromPairingType(type);
            d.name = L"Logitech 无线设备";
            d.product = d.name;
            d.note = ReceiverLabel(g.pid);

            std::vector<uint8_t> nameInfo;
            uint8_t np[1] = {(uint8_t)(0x40 | slot)};
            if (link.Register(0x83, 0xB5, np, 1, nameInfo) && nameInfo.size() > 6) {
                int len = std::min<int>(nameInfo[5], (int)nameInfo.size() - 6);
                if (len > 0) {
                    std::string nm((const char*)nameInfo.data() + 6, len);
                    std::wstring wn = Trim(Utf8ToWide(nm));
                    if (!wn.empty()) d.name = wn;
                }
            }

            bool online = false;
            bool pinged = link.Ping(idx, online);
            if (pinged && online) {
                d.online = true;
                std::wstring dn;
                if (link.DeviceName(idx, dn)) d.name = dn;
                Kind k = Kind::Other;
                if (link.DeviceType(idx, k)) d.kind = k;
                link.Battery(idx, d.battery, d.charging, d.millivolts, d.batteryValid);
                link.Dpi(idx, d.dpi);
                link.Polling(idx, d.polling);
            } else {
                d.online = false;
                d.note = L"接收器在线，设备休眠或已关机";
            }
            out.push_back(std::move(d));
            any = true;
        }
        if (!any) {
            Device d;
            d.id = L"L:" + g.base + L"#0";
            d.proto = Proto::Logitech;
            d.vid = kLogiVid;
            d.pid = g.pid;
            d.basePath = g.base;
            d.path = g.longIface.path;
            d.ifaces = all;
            d.name = ReceiverLabel(g.pid);
            d.product = g.product;
            d.manufacturer = L"Logitech";
            d.conn = Conn::Wireless24;
            d.kind = Kind::Other;
            d.online = true;
            d.note = L"未发现已配对设备，请在 Logitech 软件中配对";
            out.push_back(std::move(d));
        }
    }
}

bool ApplyLogitech(const Device& dev, const ApplyRequest& req, std::wstring& err) {
    Group g;
    g.base = dev.basePath;
    g.pid = dev.pid;
    g.product = dev.product;
    for (const auto& h : dev.ifaces) {
        if (h.vid != kLogiVid || h.basePath != dev.basePath) continue;
        bool isShort = (h.usage == 1) || (h.inLen > 0 && h.inLen <= 8);
        bool isLong = (h.usage == 2) || (h.inLen >= 20);
        if (isLong) {
            g.longIface = h;
            g.hasLong = true;
        }
        if (isShort) {
            g.shortIface = h;
            g.hasShort = true;
        }
    }
    if (!g.hasLong && g.hasShort) {
        g.longIface = g.shortIface;
        g.hasLong = true;
    }
    Link link;
    if (!g.hasLong || !link.Open(g)) {
        err = L"无法打开 Logitech 设备";
        return false;
    }
    uint8_t idx = (uint8_t)(dev.lgIndex ? dev.lgIndex : 0xFF);
    bool ok = false;
    if (req.setDpi) {
        int x = ClampInt(req.dpiX, 50, 100000);
        int y = ClampInt(req.dpiY > 0 ? req.dpiY : x, 50, 100000);
        if (link.SetDpi(idx, x, y, dev.dpi.lod)) {
            ok = true;
        } else {
            err = L"写入 DPI 失败（可能被板载配置锁定）";
        }
    }
    if (req.selectStage && req.stageIndex >= 0) {
        if (link.SelectStage(idx, req.stageIndex)) ok = true;
    }
    if (req.setPolling && req.pollingHz > 0) {
        if (link.SetPolling(idx, req.pollingHz)) {
            ok = true;
        } else {
            err = L"写入轮询率失败";
        }
    }
    if (!ok && err.empty()) err = L"设备没有接受任何设置";
    return ok;
}

}  // namespace proto
