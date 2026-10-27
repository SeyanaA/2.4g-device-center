#include "protocols.h"

#include <cstring>
#include <map>

namespace {

constexpr uint16_t kAsusVid = 0x0B05;

struct AsusModel {
    uint16_t pid;
    const wchar_t* name;
    int scale;
    int batteryOffset;
    bool keyboard;
};

const AsusModel kModels[] = {
    {0x1ACE, L"ROG OMNI 接收器", 1, 4, false},
    {0x1AD0, L"ROG Harpe II", 1, 4, false},
    {0x1A1A, L"ROG Chakram X", 1, 4, false},
    {0x1A18, L"ROG Chakram X (USB)", 1, 4, false},
    {0x197F, L"ROG Gladius III Wireless", 1, 4, false},
    {0x197D, L"ROG Gladius III (USB)", 1, 4, false},
    {0x1A72, L"ROG Gladius III Aimpoint", 1, 4, false},
    {0x1A70, L"ROG Gladius III Aimpoint (USB)", 1, 4, false},
    {0x1B0C, L"ROG Gladius III EVA-02", 1, 4, false},
    {0x1B0A, L"ROG Gladius III EVA-02 (USB)", 1, 4, false},
    {0x1A94, L"ROG Harpe Ace Aim Lab", 1, 4, false},
    {0x1A92, L"ROG Harpe Ace Aim Lab (USB)", 1, 4, false},
    {0x1B67, L"ROG Harpe Ace Extreme (USB)", 1, 4, false},
    {0x1A68, L"ROG Keris Aimpoint", 1, 4, false},
    {0x1A66, L"ROG Keris Aimpoint (USB)", 1, 4, false},
    {0x1B16, L"ROG Keris II Ace (USB)", 1, 4, false},
    {0x1979, L"ROG Spatha X", 1, 4, false},
    {0x1977, L"ROG Spatha X (USB)", 1, 4, false},
    {0x18E5, L"ROG Chakram", 25, 4, false},
    {0x18E3, L"ROG Chakram (USB)", 25, 4, false},
    {0x1908, L"ROG Pugio II", 25, 4, false},
    {0x1906, L"ROG Pugio II (USB)", 25, 4, false},
    {0x1960, L"ROG Keris Wireless", 25, 4, false},
    {0x195E, L"ROG Keris (USB)", 25, 4, false},
    {0x1A59, L"ROG Keris EVA", 25, 4, false},
    {0x1A57, L"ROG Keris EVA (USB)", 25, 4, false},
    {0x1949, L"ROG Strix Impact II Wireless", 25, 4, false},
    {0x1947, L"ROG Strix Impact II (USB)", 25, 4, false},
    {0x18A0, L"ROG Gladius II Wireless", 25, 4, false},
    {0x18B4, L"ROG Strix Carry", 25, 6, false},
    {0x1A85, L"ROG Azoth", 1, 5, true},
    {0x1B42, L"ROG Azoth Extreme", 1, 5, true},
    {0x1B3F, L"ROG Azoth Extreme (USB)", 1, 5, true},
    {0x1CF1, L"ROG Azoth Extreme SE", 1, 5, true},
    {0x1B63, L"ROG Harpe Ace Mini (USB)", 1, 4, false},
    {0x1C69, L"ROG Harpe II Ace (USB)", 1, 4, false},
    {0x1C0C, L"ROG Keris II Origin (USB)", 1, 4, false},
};

const AsusModel* FindModel(uint16_t pid) {
    for (const auto& m : kModels) {
        if (m.pid == pid) return &m;
    }
    return nullptr;
}

struct AsusLink {
    HidHandle h;
    uint8_t reportId = 0;
    bool open = false;

    bool Open(const HidInterface& iface, uint8_t rid) {
        reportId = rid;
        open = false;
        if (!h.Open(iface, GENERIC_READ | GENERIC_WRITE, false)) return false;
        open = true;
        return true;
    }

    bool Query(uint16_t cmd, const uint8_t* params, size_t plen, std::vector<uint8_t>& payload,
               DWORD timeout = 500) {
        if (!open) return false;
        size_t size = h.info().outLen ? h.info().outLen : 64;
        if (size < 8) size = 64;
        std::vector<uint8_t> req(size, 0);
        req[0] = reportId;
        req[1] = (uint8_t)(cmd & 0xFF);
        req[2] = (uint8_t)(cmd >> 8);
        if (params && plen) memcpy(req.data() + 3, params, std::min(plen, size - 3));
        if (!h.sendRaw(req.data(), req.size(), timeout)) return false;

        std::vector<uint8_t> buf(size + 8);
        int n = h.readMatching(buf.data(), buf.size(), [&](const uint8_t* b, int len) {
            if (len < 3) return false;
            if (b[1] == 0xFF && b[2] == 0xAA) return true;
            if (b[1] == req[1] && b[2] == req[2]) return true;
            return b[0] == req[1] && b[1] == req[2];
        }, timeout);
        if (n < 4) return false;
        int echo = -1;
        if (buf[1] == req[1] && buf[2] == req[2]) echo = 1;
        else if (buf[0] == req[1] && buf[1] == req[2]) echo = 0;
        if (echo < 0) return false;
        if (buf[echo] == 0xFF && buf[echo + 1] == 0xAA) return false;
        payload.assign(buf.begin() + echo, buf.begin() + n);
        return payload.size() >= 5;
    }
};

std::wstring AsusDeviceName(uint16_t pid, const std::wstring& product) {
    if (const AsusModel* m = FindModel(pid)) return m->name;
    if (ContainsNoCase(product, L"OMNI")) return L"ROG OMNI 接收器";
    if (!product.empty()) return product;
    return L"ASUS ROG 设备";
}

bool AsusReadBattery(AsusLink& link, bool keyboard, int scale, int offset, int& pct,
                     bool& charging) {
    uint16_t cmd = keyboard ? 0x1201 : 0x0712;
    std::vector<uint8_t> p;
    if (!link.Query(cmd, nullptr, 0, p)) return false;
    int idx = keyboard ? 5 : offset;
    if ((int)p.size() <= idx) return false;
    int raw = p[idx];
    int value = raw * scale;
    if (value <= 0 || value > 100) return false;
    pct = value;
    if (keyboard) {
        charging = p.size() > 8 && p[8] == 1;
    } else {
        charging = (p.size() > 9 && p[9] != 0) || (p.size() > 5 && p[5] == 1);
    }
    return true;
}

void AsusReadMouseSettings(AsusLink& link, Device& d) {
    std::vector<uint8_t> p;
    uint8_t param = 0x02;
    if (link.Query(0x0412, &param, 1, p) && p.size() >= 20) {
        std::vector<int> stages;
        for (int i = 0; i < 4; ++i) {
            int x = p[4 + i * 4] | (p[5 + i * 4] << 8);
            int y = p[6 + i * 4] | (p[7 + i * 4] << 8);
            if (x > 0 && x <= 720) {
                stages.push_back(x * 50 + 50);
                if (y <= 0) y = x;
            }
        }
        if (!stages.empty()) {
            d.dpi.valid = true;
            d.dpi.canWrite = true;
            d.dpi.currentKnown = true;
            d.dpi.stages = stages;
            d.dpi.min = 50;
            d.dpi.max = 36000;
            d.dpi.step = 50;
            d.dpi.activeStage = 0;
            d.dpi.x = stages[0];
            d.dpi.y = stages[0];
        }
    }

    std::vector<uint8_t> prof;
    if (link.Query(0x0012, nullptr, 0, prof) && prof.size() >= 12) {
        if (prof[10] != 0 || prof[11] != 0) {
            int active = prof[11] > 0 ? prof[11] - 1 : -1;
            if (active >= 0 && active < (int)d.dpi.stages.size()) {
                d.dpi.activeStage = active;
                d.dpi.x = d.dpi.stages[active];
                d.dpi.y = d.dpi.x;
            }
        }
    }

    std::vector<uint8_t> extra;
    uint8_t zero = 0;
    if (link.Query(0x0412, &zero, 1, extra) && extra.size() >= 18) {
        int rateIdx = extra[12] & 0x07;
        static const int rates[] = {125, 250, 500, 1000};
        if (rateIdx < 4) {
            d.polling.valid = true;
            d.polling.canWrite = true;
            d.polling.currentHz = rates[rateIdx];
            d.polling.options = {125, 250, 500, 1000};
        }
        int debIdx = extra[14];
        static const int debs[] = {4, 8, 12, 16, 20, 24, 28, 32};
        if (debIdx < 8) {
            d.debounce.valid = true;
            d.debounce.canWrite = true;
            d.debounce.currentMs = debs[debIdx];
            d.debounce.options.assign(debs, debs + 8);
        }
        d.angle.valid = true;
        d.angle.canWrite = true;
        d.angle.on = extra[16] != 0;
    }
}

struct Group {
    std::wstring base;
    uint16_t pid = 0;
    std::wstring product;
    std::vector<HidInterface> ifaces;
};

std::vector<Group> BuildGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, Group> map;
    for (const auto& h : all) {
        if (h.vid != kAsusVid) continue;
        if (h.usagePage != 0xFF01 && h.usagePage != 0xFF00) continue;
        if (h.outLen < 64) continue;
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

}  // namespace

namespace proto {

void ReadAsus(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    auto groups = BuildGroups(all);
    for (const auto& g : groups) {
        const AsusModel* model = FindModel(g.pid);
        int scale = model ? model->scale : 1;
        int offset = model ? model->batteryOffset : 4;
        bool modelKeyboard = model && model->keyboard;

        AsusLink link;
        HidInterface chosen{};
        uint8_t chosenReport = 0;
        bool isKeyboard = modelKeyboard;
        bool found = false;

        for (const auto& iface : g.ifaces) {
            for (uint8_t rid : {0, 3, 2, 1}) {
                link.h.Close();
                if (!link.Open(iface, rid)) continue;
                int pct = -1;
                bool charging = false;
                if (AsusReadBattery(link, false, scale, offset, pct, charging)) {
                    chosen = iface;
                    chosenReport = rid;
                    isKeyboard = false;
                    found = true;
                    break;
                }
                if (AsusReadBattery(link, true, scale, 5, pct, charging)) {
                    chosen = iface;
                    chosenReport = rid;
                    isKeyboard = true;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) continue;

        if (!link.Open(chosen, chosenReport)) continue;
        Device d;
        d.id = L"A:" + g.base + L"#" + std::to_wstring(chosenReport);
        d.proto = Proto::Asus;
        d.vid = kAsusVid;
        d.pid = g.pid;
        d.basePath = g.base;
        d.path = chosen.path;
        d.statusReportId = chosenReport;
        d.ifaces = all;
        d.batteryScale = isKeyboard ? 1 : scale;
        d.name = AsusDeviceName(g.pid, g.product);
        d.product = g.product;
        d.manufacturer = L"ASUSTeK";
        d.kind = isKeyboard ? Kind::Keyboard : Kind::Mouse;
        if (ContainsNoCase(g.product, L"headset") || ContainsNoCase(d.name, L"耳机")) {
            d.kind = Kind::Headset;
        }
        d.conn = (g.pid == 0x1ACE || ContainsNoCase(g.product, L"OMNI") ||
                  ContainsNoCase(g.product, L"receiver") ||
                  ContainsNoCase(g.product, L"dongle") ||
                  ContainsNoCase(g.product, L"wireless"))
                     ? Conn::Wireless24
                     : Conn::Wired;
        d.online = true;
        if (!AsusReadBattery(link, isKeyboard, isKeyboard ? 1 : scale, offset, d.battery,
                             d.charging)) {
            d.battery = -1;
            d.batteryValid = false;
        } else {
            d.batteryValid = true;
        }
        if (!isKeyboard) {
            AsusReadMouseSettings(link, d);
        }
        out.push_back(std::move(d));
    }
}

bool ApplyAsus(const Device& dev, const ApplyRequest& req, std::wstring& err) {
    HidInterface iface{};
    bool found = false;
    for (const auto& h : dev.ifaces) {
        if (h.path == dev.path) {
            iface = h;
            found = true;
            break;
        }
    }
    if (!found) {
        err = L"找不到 ASUS HID 接口";
        return false;
    }
    AsusLink link;
    if (!link.Open(iface, (uint8_t)dev.statusReportId)) {
        err = L"无法打开 ASUS 设备";
        return false;
    }
    bool ok = false;
    std::vector<uint8_t> p;

    int stage = req.stageIndex >= 0 ? req.stageIndex
                                    : (dev.dpi.activeStage >= 0 ? dev.dpi.activeStage : 0);
    if (req.setDpi) {
        int dpi = ClampInt(req.dpiX, 50, 36000);
        dpi = ((dpi + 25) / 50) * 50;
        int idpi = (dpi - 50) / 50;
        uint8_t params[4] = {(uint8_t)stage, 0, (uint8_t)(idpi & 0xFF),
                             (uint8_t)((idpi >> 8) & 0xFF)};
        if (link.Query(0x3151, params, 4, p)) ok = true;
        else err = L"写入 DPI 失败";
    }
    if (req.selectStage && req.stageIndex >= 0) {
        uint8_t params[4] = {9, 0, (uint8_t)(req.stageIndex + 1), 0};
        if (link.Query(0x3151, params, 4, p)) ok = true;
        else err = L"切换 DPI 档位失败";
    }
    if (req.setPolling) {
        static const int rates[] = {125, 250, 500, 1000};
        int idx = -1;
        for (int i = 0; i < 4; ++i) {
            if (rates[i] == req.pollingHz) idx = i;
        }
        if (idx >= 0) {
            uint8_t params[4] = {4, 0, (uint8_t)idx, 0};
            if (link.Query(0x3151, params, 4, p)) ok = true;
            else err = L"写入轮询率失败";
        }
    }
    if (req.setDebounce) {
        static const int debs[] = {4, 8, 12, 16, 20, 24, 28, 32};
        int idx = -1;
        for (int i = 0; i < 8; ++i) {
            if (debs[i] == req.debounceMs) idx = i;
        }
        if (idx >= 0) {
            uint8_t params[4] = {5, 0, (uint8_t)idx, 0};
            if (link.Query(0x3151, params, 4, p)) ok = true;
            else err = L"写入按键去抖失败";
        }
    }
    if (req.setAngle) {
        uint8_t params[4] = {6, 0, (uint8_t)(req.angleOn ? 1 : 0), 0};
        if (link.Query(0x3151, params, 4, p)) ok = true;
        else err = L"写入角度捕捉失败";
    }
    if (ok) {
        if (!link.Query(0x0350, nullptr, 0, p)) {
            err = L"设置已发送，但保存到设备失败";
            return false;
        }
    } else if (err.empty()) {
        err = L"设备没有接受任何设置";
    }
    return ok;
}

}  // namespace proto
