#include "protocols.h"

#include <cstring>
#include <map>

namespace {

constexpr uint16_t kRapooVid = 0x24AE;

int RapooPollHz(uint8_t code) {
    switch (code) {
        case 8: return 125;
        case 4: return 250;
        case 2: return 500;
        case 1: return 1000;
        case 132: return 2000;
        case 130: return 4000;
        case 129: return 8000;
        default: return 0;
    }
}

uint8_t RapooPollCode(int hz) {
    switch (hz) {
        case 125: return 8;
        case 250: return 4;
        case 500: return 2;
        case 1000: return 1;
        case 2000: return 132;
        case 4000: return 130;
        case 8000: return 129;
        default: return 0;
    }
}

uint16_t ReadLe16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void WriteLe16(uint8_t* p, int v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

struct RapooLink {
    HidHandle outH;
    HidHandle featH;
    HidHandle inH;
    uint16_t featLen = 0;
    bool open = false;
    uint8_t conn = 0xA5;

    bool Open(const HidInterface& outIface, const HidInterface& featIface,
              const HidInterface* inIface) {
        open = outH.Open(outIface, GENERIC_READ | GENERIC_WRITE, false) &&
               featH.Open(featIface, GENERIC_READ | GENERIC_WRITE, false);
        if (inIface) inH.Open(*inIface, GENERIC_READ, true);
        featLen = featIface.featLen;
        return open;
    }

    bool Query(uint8_t cmd, uint32_t addr, const uint8_t* data, size_t len,
               std::vector<uint8_t>* out, DWORD timeout = 700) {
        if (!open) return false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint8_t req[33] = {};
            req[0] = 6;
            req[1] = conn;
            req[2] = cmd;
            req[3] = (uint8_t)len;
            req[4] = (uint8_t)(addr & 0xFF);
            req[5] = (uint8_t)((addr >> 8) & 0xFF);
            req[6] = (uint8_t)((addr >> 16) & 0xFF);
            req[7] = (uint8_t)((addr >> 24) & 0xFF);
            if (data && len) memcpy(req + 8, data, std::min<size_t>(len, 24));
            if (!outH.sendRaw(req, sizeof(req), timeout)) return false;
            Sleep(attempt == 0 ? 15 : 10);

            uint8_t raw[64] = {};
            bool ok = false;
            if (featLen >= 33) {
                ok = featH.getFeature(8, raw, 33, timeout);
            } else {
                ok = featH.getFeature(0, raw, featLen ? featLen : 32, timeout);
            }
            if (!ok) continue;
            int status = 0;
            int dataOff = 0;
            if (raw[0] == 8) {
                status = raw[1];
                dataOff = 5;
            } else {
                status = raw[0];
                dataOff = 4;
            }
            if (status == 1) {
                if (out) {
                    out->assign(len, 0);
                    size_t avail = raw[0] == 8 ? 28 : 28;
                    size_t copy = std::min(len, avail);
                    if (copy) memcpy(out->data(), raw + dataOff, copy);
                }
                return true;
            }
            if (status != 2) return false;
            Sleep(20);
        }
        return false;
    }

    bool ReadEeprom(uint32_t addr, size_t len, std::vector<uint8_t>& data) {
        data.assign(len, 0);
        size_t off = 0;
        while (off < len) {
            size_t chunk = std::min<size_t>(24, len - off);
            std::vector<uint8_t> part;
            if (!Query(0xA4, addr + (uint32_t)off, nullptr, chunk, &part)) return false;
            if (part.size() < chunk) part.resize(chunk, 0);
            memcpy(data.data() + off, part.data(), chunk);
            off += chunk;
        }
        return true;
    }

    bool WriteEeprom(uint32_t addr, const std::vector<uint8_t>& data) {
        size_t off = 0;
        while (off < data.size()) {
            size_t chunk = std::min<size_t>(24, data.size() - off);
            if (!Query(0xA5, addr + (uint32_t)off, data.data() + off, chunk, nullptr)) return false;
            off += chunk;
        }
        return true;
    }

    bool ReadStatus(int& pct, bool& charging, int& dpiX, int& dpiY, int& stage, int& polling,
                    bool& wired) {
        pct = -1;
        charging = false;
        if (!inH.valid()) return false;
        uint8_t buf[64] = {};
        int n = inH.readMatching(buf, sizeof(buf), [](const uint8_t* b, int len) {
            return len >= 9 && b[0] == 7;
        }, 700);
        if (n < 9) return false;
        wired = (buf[1] & 0x0F) == 2;
        stage = buf[2];
        dpiX = buf[3] | (buf[4] << 8);
        dpiY = buf[5] | (buf[6] << 8);
        if (buf[8] > 0 && buf[8] <= 100) pct = buf[8];
        if (n >= 12) polling = RapooPollHz(buf[11]);
        return pct > 0 || dpiX > 0;
    }
};

struct Group {
    std::wstring base;
    uint16_t pid = 0;
    std::wstring product;
    HidInterface outIface{};
    HidInterface featIface{};
    HidInterface inIface{};
    bool hasOut = false;
    bool hasFeat = false;
    bool hasIn = false;
};

std::vector<Group> BuildGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, Group> map;
    for (const auto& h : all) {
        if (h.vid != kRapooVid) continue;
        if (h.usagePage != 0xFF00) continue;
        Group& g = map[h.basePath];
        g.base = h.basePath;
        g.pid = h.pid;
        if (g.product.empty()) g.product = h.product;
        if (h.outLen >= 33 && !g.hasOut) {
            g.outIface = h;
            g.hasOut = true;
        }
        if (h.featLen >= 32 && !g.hasFeat) {
            g.featIface = h;
            g.hasFeat = true;
        }
        if (h.inLen >= 19 && !g.hasIn) {
            g.inIface = h;
            g.hasIn = true;
        }
    }
    std::vector<Group> out;
    for (auto& [k, g] : map) {
        if (g.hasOut && g.hasFeat) out.push_back(g);
    }
    return out;
}

std::wstring RapooName(const Group& g) {
    std::wstring p = Trim(g.product);
    if (!p.empty() && !ContainsNoCase(p, L"receiver") && !ContainsNoCase(p, L"dongle") &&
        !ContainsNoCase(p, L"2.4g wireless") && !ContainsNoCase(p, L"wireless device")) {
        return p;
    }
    if (ContainsNoCase(p, L"receiver") || ContainsNoCase(p, L"dongle")) {
        return L"Rapoo 2.4G 接收器";
    }
    return L"Rapoo 2.4G 设备";
}

}  // namespace

namespace proto {

void ReadRapoo(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    auto groups = BuildGroups(all);
    for (const auto& g : groups) {
        RapooLink link;
        if (!link.Open(g.outIface, g.featIface, g.hasIn ? &g.inIface : nullptr)) continue;

        Device d;
        d.id = L"P:" + g.base;
        d.proto = Proto::Rapoo;
        d.vid = kRapooVid;
        d.pid = g.pid;
        d.basePath = g.base;
        d.path = g.outIface.path;
        d.ifaces = all;
        d.name = RapooName(g);
        d.product = g.product;
        d.manufacturer = L"Rapoo";
        d.kind = ContainsNoCase(g.product, L"keyboard") ? Kind::Keyboard
                                                       : (ContainsNoCase(g.product, L"headset")
                                                              ? Kind::Headset
                                                              : Kind::Mouse);
        d.conn = Conn::Wireless24;
        d.online = true;

        int pct = -1;
        bool charging = false;
        int dpiX = 0, dpiY = 0, stage = -1, polling = 0;
        bool wired = false;
        bool status = link.ReadStatus(pct, charging, dpiX, dpiY, stage, polling, wired);
        if (wired) d.conn = Conn::Wired;
        if (status && pct > 0) {
            d.battery = pct;
            d.batteryValid = true;
            d.charging = charging;
        }

        std::vector<uint8_t> block;
        const uint32_t kBase = 0x0600;
        if (link.ReadEeprom(kBase + 640, 80, block) && block.size() >= 80) {
            uint8_t pollCode = block[0];
            int pollHz = RapooPollHz(pollCode);
            if (pollHz > 0) {
                d.polling.valid = true;
                d.polling.canWrite = true;
                d.polling.currentHz = polling > 0 ? polling : pollHz;
                d.polling.options = {125, 250, 500, 1000, 2000, 4000, 8000};
            }

            int enabled = ClampInt(block[22] + 1, 1, 7);
            int current = block[24];
            std::vector<int> xs, ys;
            for (int i = 0; i < 7; ++i) {
                int x = ReadLe16(&block[8 + i * 2]);
                int y = ReadLe16(&block[72 + i * 2]);
                xs.push_back(x);
                ys.push_back(y);
            }
            std::vector<int> stages;
            for (int i = 0; i < enabled && i < (int)xs.size(); ++i) {
                if (xs[i] >= 50 && xs[i] <= 60000) stages.push_back(xs[i]);
            }
            if (!stages.empty()) {
                d.dpi.valid = true;
                d.dpi.canWrite = true;
                d.dpi.currentKnown = true;
                d.dpi.stages = stages;
                d.dpi.activeStage = current >= 0 && current < (int)stages.size() ? current : 0;
                d.dpi.x = stages[d.dpi.activeStage];
                d.dpi.y = d.dpi.x;
                d.dpi.min = 50;
                d.dpi.max = 60000;
                d.dpi.step = 50;
            }
            if (status && dpiX >= 50 && dpiX <= 60000) {
                d.dpi.valid = true;
                d.dpi.x = dpiX;
                d.dpi.y = dpiY >= 50 ? dpiY : dpiX;
                d.dpi.currentKnown = true;
                if (stage >= 0 && stage < (int)d.dpi.stages.size()) d.dpi.activeStage = stage;
            }

            int deb = block[64];
            d.debounce.valid = true;
            d.debounce.canWrite = true;
            d.debounce.currentMs = deb;
            d.debounce.options = {0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};

            d.angle.valid = true;
            d.angle.canWrite = true;
            d.angle.on = (block[67] & 0x01) != 0;
        } else {
            d.note = L"设备响应了状态，但配置文件读取失败（可能休眠）";
        }

        if (!d.batteryValid && d.dpi.valid) {
            d.note = L"鼠标休眠时电量不可读，移动鼠标后刷新";
        }
        if (!d.batteryValid && !d.dpi.valid && !status) {
            d.online = false;
            d.note = L"接收器在线，鼠标休眠或已关机";
        }
        out.push_back(std::move(d));
    }
}

bool ApplyRapoo(const Device& dev, const ApplyRequest& req, std::wstring& err) {
    HidInterface outIface{}, featIface{}, inIface{};
    bool hasOut = false, hasFeat = false, hasIn = false;
    for (const auto& h : dev.ifaces) {
        if (h.vid != kRapooVid || h.basePath != dev.basePath) continue;
        if (h.usagePage != 0xFF00) continue;
        if (h.outLen >= 33 && !hasOut) {
            outIface = h;
            hasOut = true;
        }
        if (h.featLen >= 32 && !hasFeat) {
            featIface = h;
            hasFeat = true;
        }
        if (h.inLen >= 19 && !hasIn) {
            inIface = h;
            hasIn = true;
        }
    }
    if (!hasOut || !hasFeat) {
        err = L"找不到 Rapoo 控制接口";
        return false;
    }
    RapooLink link;
    if (!link.Open(outIface, featIface, hasIn ? &inIface : nullptr)) {
        err = L"无法打开 Rapoo 设备";
        return false;
    }
    link.conn = dev.conn == Conn::Wired ? 0xFF : 0xA5;
    const uint32_t base = 0x0600;
    bool ok = false;

    int stage = req.stageIndex >= 0 ? req.stageIndex
                                    : (dev.dpi.activeStage >= 0 ? dev.dpi.activeStage : 0);
    if (req.setDpi) {
        int dpi = ClampInt(req.dpiX, 50, 60000);
        dpi = ((dpi + 25) / 50) * 50;
        std::vector<uint8_t> x(2), y(2), cur(1);
        WriteLe16(x.data(), dpi);
        WriteLe16(y.data(), req.dpiY > 0 ? req.dpiY : dpi);
        cur[0] = (uint8_t)stage;
        if (link.WriteEeprom(base + 648 + stage * 2, x) &&
            link.WriteEeprom(base + 712 + stage * 2, y) &&
            link.WriteEeprom(base + 664, cur)) {
            ok = true;
        } else {
            err = L"写入 DPI 失败";
        }
    } else if (req.selectStage && req.stageIndex >= 0) {
        std::vector<uint8_t> cur(1, (uint8_t)req.stageIndex);
        if (link.WriteEeprom(base + 664, cur)) ok = true;
        else err = L"切换 DPI 档位失败";
    }

    if (req.setPolling && req.pollingHz > 0) {
        uint8_t code = RapooPollCode(req.pollingHz);
        if (code) {
            std::vector<uint8_t> v(1, code);
            if (link.WriteEeprom(base + 640, v)) ok = true;
            else err = L"写入轮询率失败";
        }
    }
    if (req.setDebounce) {
        std::vector<uint8_t> v(2, (uint8_t)ClampInt(req.debounceMs, 0, 32));
        if (link.WriteEeprom(base + 704, v)) ok = true;
        else err = L"写入按键去抖失败";
    }
    if (req.setAngle) {
        std::vector<uint8_t> v;
        if (link.ReadEeprom(base + 707, 1, v) && !v.empty()) {
            if (req.angleOn) v[0] |= 0x01;
            else v[0] &= (uint8_t)~0x01;
            if (link.WriteEeprom(base + 707, v)) ok = true;
            else err = L"写入角度捕捉失败";
        }
    }
    if (!ok && err.empty()) err = L"设备没有接受任何设置";
    return ok;
}

}  // namespace proto
