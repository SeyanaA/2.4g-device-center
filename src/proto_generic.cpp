#include "protocols.h"

#include <cstring>
#include <map>

namespace {

// ---------------------------------------------------------------------------
// Compx (0xFF02, 17-byte interrupt reports, report id 8)
// ---------------------------------------------------------------------------

struct CompxGroup {
    std::wstring base;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::wstring product;
    HidInterface outIface{};
    HidInterface inIface{};
    bool hasOut = false;
    bool hasIn = false;
};

std::vector<CompxGroup> BuildCompxGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, CompxGroup> map;
    for (const auto& h : all) {
        if (h.usagePage != 0xFF02) continue;
        if (h.inLen < 17 || h.outLen < 17) continue;
        CompxGroup& g = map[h.basePath];
        g.base = h.basePath;
        g.vid = h.vid;
        g.pid = h.pid;
        if (g.product.empty()) g.product = h.product;
        if (h.outLen >= 17 && !g.hasOut) {
            g.outIface = h;
            g.hasOut = true;
        }
        if (h.inLen >= 17 && !g.hasIn) {
            g.inIface = h;
            g.hasIn = true;
        }
    }
    std::vector<CompxGroup> out;
    for (auto& [k, g] : map) {
        if (g.hasOut && g.hasIn) out.push_back(g);
    }
    return out;
}

uint8_t CompxChecksum(const uint8_t* p, size_t n) {
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += p[i];
    return (uint8_t)(0x55 - sum);
}

struct CompxLink {
    HidHandle outH;
    HidHandle inH;
    bool open = false;
    bool fastRead_ = false;

    bool Open(const CompxGroup& g) {
        open = outH.Open(g.outIface, GENERIC_READ | GENERIC_WRITE, true) &&
               inH.Open(g.inIface, GENERIC_READ, true);
        return open;
    }

    bool Exchange(uint8_t cmd, std::vector<uint8_t>& reply, DWORD timeout = 700) {
        if (!open) return false;
        uint8_t req[17] = {};
        req[0] = 8;
        req[1] = cmd;
        req[16] = CompxChecksum(req, 16);
        if (!outH.sendRaw(req, sizeof(req), 500)) {
            uint8_t payload[16] = {};
            memcpy(payload, req + 1, 16);
            if (!outH.setFeature(8, payload, 16, 500)) return false;
        }
        uint8_t buf[64] = {};
        int n = inH.readMatching(buf, sizeof(buf), [&](const uint8_t* b, int len) {
            if (len < 6) return false;
            if (b[0] == 8) return b[1] == cmd;
            return b[0] == cmd;
        }, timeout);
        if (n < 6) return false;
        reply.assign(buf, buf + n);
        return true;
    }

    bool ReadEeprom(uint16_t addr, uint8_t len, std::vector<uint8_t>& out) {
        out.assign(len, 0);
        for (int attempt = 0; attempt < 2; ++attempt) {
            uint8_t req[17] = {};
            req[0] = 8;
            req[1] = 8;
            req[2] = 0;
            req[3] = (uint8_t)(addr >> 8);
            req[4] = (uint8_t)addr;
            req[5] = len;
            req[16] = CompxChecksum(req, 16);
            if (!outH.sendRaw(req, sizeof(req), 600)) return false;

            uint8_t buf[256] = {};
            auto matches = [&](const uint8_t* b, int total) {
                // Responses can be batched (several 17-byte reports in one
                // ReadFile), and some firmware strips the report ID.
                for (int o = 0; o + 6 <= total; ++o) {
                    int dataOff = -1;
                    if (b[o] == 8 && b[o + 1] == 8 && b[o + 3] == (uint8_t)(addr >> 8) &&
                        b[o + 4] == (uint8_t)addr && b[o + 5] == len) {
                        // Report ID + command echo + 00 + addr + len + data.
                        dataOff = o + 6;
                    } else if (b[o] == 8 && b[o + 1] == 0 && b[o + 2] == (uint8_t)(addr >> 8) &&
                               b[o + 3] == (uint8_t)addr && b[o + 4] == len) {
                        // Report ID stripped: command + 00 + addr + len + data.
                        dataOff = o + 5;
                    }
                    if (dataOff >= 0) {
                        int avail = std::min<int>(len, total - dataOff);
                        if (avail > 0) memcpy(out.data(), b + dataOff, avail);
                        return avail == len;
                    }
                }
                return false;
            };
            int n = inH.readMatching(buf, sizeof(buf), matches, fastRead_ ? 650 : 1300);
            if (n > 0) {
                fastRead_ = true;
                return true;
            }

            // A sleeping child behind the receiver may queue the read until the
            // next status query; poke it and retry once.
            uint8_t wake[17] = {8, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            wake[16] = CompxChecksum(wake, 16);
            outH.sendRaw(wake, sizeof(wake), 400);
            Sleep(160);
        }
        return false;
    }
};

void CompxReadDpi(CompxLink& link, DpiInfo& dpi) {
    std::vector<uint8_t> raw(32, 0xFF);
    for (int i = 0; i < 4; ++i) {
        int addr = 0x0C + i * 8;
        std::vector<uint8_t> part;
        if (!link.ReadEeprom((uint16_t)addr, 8, part)) return;
        if (part.size() < 8) return;
        memcpy(raw.data() + i * 8, part.data(), 8);
    }
    std::vector<int> stages;
    for (int i = 0; i < 8; ++i) {
        int x = raw[i * 4 + 0];
        int y = raw[i * 4 + 1];
        int z = raw[i * 4 + 2];
        int crc = raw[i * 4 + 3];
        if (x == 0xFF) break;
        if (crc != ((0x55 - x - y - z) & 0xFF)) break;
        int code = x | (((z >> 6) & 0x03) << 8);
        int unit = (z & 0x22) ? 100 : 50;
        int dpiValue = (code + 1) * unit;
        if (z & 0x11) dpiValue *= 2;
        if (dpiValue < 50 || dpiValue > 60000) break;
        stages.push_back(dpiValue);
    }
    if (stages.size() < 2) return;
    dpi.valid = true;
    dpi.currentKnown = false;
    dpi.canWrite = false;
    dpi.stages = stages;
    dpi.min = *std::min_element(stages.begin(), stages.end());
    dpi.max = *std::max_element(stages.begin(), stages.end());
    dpi.step = 50;
    dpi.x = stages.front();
    dpi.y = stages.front();
    dpi.activeStage = -1;
    dpi.note = L"公版协议未公开当前档位；这里显示设备内保存的 DPI 档位表";
}

void CompxReadZaopin(CompxLink& link, Device& d) {
    std::vector<uint8_t> header;
    int currentStage = -1;
    int maxStages = 8;
    int reportRateCode = 0;
    if (link.ReadEeprom(0x0000, 8, header) && header.size() >= 5) {
        // MouseConfigParser: 0x00 report rate bitmask, 0x02 maxDPI count,
        // 0x04 current DPI stage (0-based).
        reportRateCode = header[0];
        maxStages = ClampInt(header[2], 1, 8);
        int cur = header[4];
        if (cur >= 0 && cur < maxStages) currentStage = cur;
    }

    std::vector<uint8_t> raw(32, 0xFF);
    for (int i = 0; i < 4; ++i) {
        std::vector<uint8_t> part;
        if (!link.ReadEeprom((uint16_t)(0x0C + i * 8), 8, part)) return;
        if (part.size() < 8) return;
        memcpy(raw.data() + i * 8, part.data(), 8);
    }

    std::vector<int> stages;
    for (int i = 0; i < 8; ++i) {
        int x = raw[i * 4 + 0];
        int y = raw[i * 4 + 1];
        int ex = raw[i * 4 + 2];
        int crc = raw[i * 4 + 3];
        if (x == 0xFF) break;
        if (crc != ((0x55 - x - y - ex) & 0xFF)) break;
        // DPIControl::UpdateUI: bit7..6 of DPIex are code bits 9..8,
        // 0x22 selects 100-unit steps, 0x11 doubles the result.
        int code = x | (((ex >> 6) & 0x03) << 8);
        int unit = (ex & 0x22) ? 100 : 50;
        int value = (code + 1) * unit;
        if (ex & 0x11) value *= 2;
        if (value < 50 || value > 60000) break;
        stages.push_back(value);
    }
    if (stages.empty()) return;
    // maxDPI is the enabled count; higher slots can still contain valid
    // historical values. Keep them visible but state the enabled count, and
    // trim only trailing duplicates used as erased/placeholder slots.
    while (stages.size() > 1 && stages.back() == stages[stages.size() - 2]) {
        stages.pop_back();
    }

    d.dpi.valid = true;
    d.dpi.canWrite = false;
    d.dpi.currentKnown = true;
    d.dpi.stages = stages;
    d.dpi.min = *std::min_element(stages.begin(), stages.end());
    d.dpi.max = *std::max_element(stages.begin(), stages.end());
    d.dpi.step = 50;
    if (currentStage < 0 || currentStage >= (int)stages.size()) currentStage = 0;
    d.dpi.activeStage = currentStage;
    d.dpi.x = stages[currentStage];
    d.dpi.y = d.dpi.x;
    d.dpi.note = Fmt(L"Zaopin Z4D：maxDPI=%d（仅前 %d 档启用）；闪存档位只读，写入请用官方 Mouse Drive",
                     maxStages, maxStages);

    if (reportRateCode & 0x40) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 8000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000, 8000};
    } else if (reportRateCode & 0x20) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 4000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000};
    } else if (reportRateCode & 0x10) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 2000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000};
    } else if (reportRateCode & 0x08) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 125;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000};
    } else if (reportRateCode & 0x04) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 250;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000};
    } else if (reportRateCode & 0x02) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 500;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000};
    } else if (reportRateCode & 0x01) {
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 1000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000};
    }

    std::vector<uint8_t> misc;
    if (link.ReadEeprom(0x00A8, 8, misc) && misc.size() >= 2) {
        int deb = misc[1];  // 0xA9 keyDebounceTime
        if (deb >= 0 && deb <= 32) {
            d.debounce.valid = true;
            d.debounce.canWrite = false;
            d.debounce.currentMs = deb;
            d.debounce.options = {0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};
        }
    }
}

Conn CompxConnection(uint16_t vid, uint16_t pid, const std::wstring& product) {
    if (vid == 0x3554) {
        if (pid == 0xF527 || pid == 0xF524 || pid == 0xF50D) return Conn::Wireless24;
        if (pid == 0xF526 || pid == 0xF50F) return Conn::Wired;
    }
    if (vid == 0x33E4 && (pid == 0x3854 || pid == 0x3517)) return Conn::Wireless24;
    if (vid == 0x33E4 && (pid == 0x3619 || pid == 0x3508)) return Conn::Wired;
    if (vid == 0x3367 && pid == 0x1961) return Conn::Wireless24;
    if (vid == 0x3367 && pid == 0x1960) return Conn::Wired;
    if (vid == 0x25A7 && pid == 0xFA07) return Conn::Wireless24;
    if (vid == 0x25A7 && pid == 0xFA08) return Conn::Wired;
    if (ContainsNoCase(product, L"receiver") || ContainsNoCase(product, L"dongle") ||
        ContainsNoCase(product, L"wireless") || ContainsNoCase(product, L"2.4g")) {
        return Conn::Wireless24;
    }
    return Conn::Wired;
}

std::wstring CompxName(uint16_t vid, const std::wstring& product) {
    if (vid == 0x3554) {
        if (ContainsNoCase(product, L"Z4D")) return L"Zaopin Z4D";
        if (ContainsNoCase(product, L"Z1 PRO") || ContainsNoCase(product, L"ZI PRO"))
            return L"Zaopin Z1 Pro";
        if (ContainsNoCase(product, L"Z2")) return L"Zaopin Z2";
        if (!product.empty() && product[0] == L'Z' && product.size() <= 8)
            return L"Zaopin " + product;
    }
    if (!product.empty() && !ContainsNoCase(product, L"wireless mouse") &&
        !ContainsNoCase(product, L"receiver") && !ContainsNoCase(product, L"dongle")) {
        return product;
    }
    switch (vid) {
        case 0x373B: return L"ATK / VXE 无线鼠标";
        case 0x3554: return L"Zaopin / Compx 无线鼠标";
        case 0x3367: return L"Endgame Gear 无线鼠标";
        case 0x33E4: return L"G-Wolves 无线鼠标";
        case 0x25A7: return L"Redragon / Areson 无线鼠标";
        case 0x3151: return L"Ajazz 无线鼠标";
        default: return product.empty() ? L"2.4G 公版鼠标" : product;
    }
}

// ---------------------------------------------------------------------------
// Nordic 54 (0xFF05, 64-byte reports, command 0x72)
// ---------------------------------------------------------------------------

struct NordicGroup {
    std::wstring base;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::wstring product;
    HidInterface outIface{};
    HidInterface inIface{};
    bool hasOut = false;
    bool hasIn = false;
};

std::vector<NordicGroup> BuildNordicGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, NordicGroup> map;
    for (const auto& h : all) {
        if (h.usagePage != 0xFF05) continue;
        if (h.inLen < 64 || h.outLen < 64) continue;
        NordicGroup& g = map[h.basePath];
        g.base = h.basePath;
        g.vid = h.vid;
        g.pid = h.pid;
        if (g.product.empty()) g.product = h.product;
        if (h.outLen >= 64 && !g.hasOut) {
            g.outIface = h;
            g.hasOut = true;
        }
        if (h.inLen >= 64 && !g.hasIn) {
            g.inIface = h;
            g.hasIn = true;
        }
    }
    std::vector<NordicGroup> out;
    for (auto& [k, g] : map) {
        if (g.hasOut && g.hasIn) out.push_back(g);
    }
    return out;
}

struct NordicLink {
    HidHandle outH;
    HidHandle inH;
    bool open = false;

    bool Open(const NordicGroup& g) {
        open = outH.Open(g.outIface, GENERIC_READ | GENERIC_WRITE, true) &&
               inH.Open(g.inIface, GENERIC_READ, true);
        return open;
    }

    bool Query(uint8_t link, int& pct, int& mv) {
        uint8_t req[64] = {};
        req[0] = 0x08;
        req[1] = 0x7D;
        req[2] = 0x72;
        req[3] = 0x02;
        req[4] = 0x00;
        req[5] = link;
        req[6] = 0x07;
        req[7] = 0x01;
        if (!outH.sendRaw(req, sizeof(req), 600)) return false;
        uint8_t buf[64] = {};
        int n = inH.readMatching(buf, sizeof(buf), [](const uint8_t* b, int len) {
            return len >= 10 && b[1] == 0x72 && b[5] == 0x07;
        }, 750);
        if (n < 10) return false;
        if (buf[2] != 0) {
            pct = -1;
            return false;
        }
        int p = buf[7];
        int v = buf[8] | (buf[9] << 8);
        if (p > 0 && p <= 100) {
            pct = p;
            mv = v;
            return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Aurora / mouse.xyz feature protocol (0xFFFF, 65-byte feature report)
// ---------------------------------------------------------------------------

struct AuroraGroup {
    std::wstring base;
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::wstring product;
    HidInterface iface{};
    bool has = false;
};

std::vector<AuroraGroup> BuildAuroraGroups(const std::vector<HidInterface>& all) {
    std::map<std::wstring, AuroraGroup> map;
    for (const auto& h : all) {
        if (h.usagePage != 0xFFFF) continue;
        if (h.featLen < 65) continue;
        AuroraGroup& g = map[h.basePath];
        g.base = h.basePath;
        g.vid = h.vid;
        g.pid = h.pid;
        if (g.product.empty()) g.product = h.product;
        if (!g.has || h.interfaceNumber == 2) {
            g.iface = h;
            g.has = true;
        }
    }
    std::vector<AuroraGroup> out;
    for (auto& [k, g] : map) {
        if (g.has) out.push_back(g);
    }
    return out;
}

bool AuroraRead(const AuroraGroup& g, int& pct, bool& charging) {
    HidHandle h;
    if (!h.Open(g.iface, GENERIC_READ | GENERIC_WRITE, false)) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint8_t req[64] = {};
        req[2] = 0x02;
        req[3] = 0x02;
        req[5] = 0x83;
        if (h.setFeature(0, req, sizeof(req), 550)) {
            Sleep(90);
            uint8_t raw[65] = {};
            if (h.getFeature(0, raw, sizeof(raw), 550)) {
                if (raw[1] == 0xA1 && raw[6] == 0x83) {
                    int p = raw[8];
                    if (p >= 0 && p <= 100) {
                        pct = p;
                        charging = raw[7] != 0;
                        return true;
                    }
                }
            }
        }

        // Legacy 0x8F command.
        memset(req, 0, sizeof(req));
        req[1] = 0x02;
        req[2] = 0x8F;
        if (h.setFeature(0, req, sizeof(req), 550)) {
            Sleep(90);
            uint8_t raw[65] = {};
            if (h.getFeature(0, raw, sizeof(raw), 550)) {
                if (raw[1] == 0xA1 && raw[2] == 0x02 && raw[3] == 0x8F) {
                    int p = raw[6];
                    if (p >= 0 && p <= 100) {
                        pct = p;
                        charging = raw[5] != 0;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Cherry (0x046A, 64-byte output report, key 0x1A)
// ---------------------------------------------------------------------------

bool CherryRead(const HidInterface& iface, int& pct, bool& charging) {
    HidHandle h;
    if (!h.Open(iface, GENERIC_READ | GENERIC_WRITE, false)) return false;
    uint8_t req[64] = {};
    req[0] = 4;
    req[1] = 0x20;
    req[2] = 0x00;
    req[3] = 0x1A;
    req[4] = 6;
    if (!h.sendRaw(req, sizeof(req), 700)) return false;
    uint8_t buf[128] = {};
    int n = h.readMatching(buf, sizeof(buf), [](const uint8_t* b, int len) {
        return len >= 10 && b[0] == 4 && b[1] == 0x20 && b[3] == 0x1A;
    }, 1200);
    if (n < 10 || buf[7] == 0xFF) return false;
    if (buf[8] == 0 || buf[8] > 100) return false;
    pct = buf[8];
    charging = buf[9] != 0;
    return true;
}

// ---------------------------------------------------------------------------
// Lofree Hyzen (WebHID reverse-engineered, 0x388D:0x0025)
// ---------------------------------------------------------------------------

bool LofreeRead(const HidInterface& iface, int& pct) {
    HidHandle h;
    if (!h.Open(iface, GENERIC_READ | GENERIC_WRITE, true)) return false;
    auto send = [&](uint8_t b1, uint8_t b2, uint8_t b3) {
        uint8_t req[63] = {};
        req[0] = 4;
        req[1] = b1;
        req[2] = b2;
        req[3] = b3;
        return h.sendRaw(req, sizeof(req), 500);
    };
    auto readEcho = [&](uint8_t echo) {
        uint8_t buf[128] = {};
        return h.readMatching(buf, sizeof(buf), [&](const uint8_t* b, int len) {
            return len >= 9 && b[0] == 4 && b[3] == echo;
        }, 450);
    };
    if (!send(0, 0, 1)) return false;
    readEcho(1);
    if (!send(0, 0, 0x1A)) return false;
    uint8_t buf[128] = {};
    int n = h.readMatching(buf, sizeof(buf), [](const uint8_t* b, int len) {
        return len >= 9 && b[0] == 4 && b[3] == 0x1A;
    }, 500);
    send(0, 0, 2);
    if (n < 9) return false;
    int p = buf[8];
    if (p < 0 || p > 100) return false;
    pct = p;
    return true;
}

}  // namespace

namespace proto {

void ReadGeneric(const std::vector<HidInterface>& all, std::vector<Device>& out) {
    // ---- Compx -----------------------------------------------------------
    for (const auto& g : BuildCompxGroups(all)) {
        CompxLink link;
        if (!link.Open(g)) continue;
        int pct = -1;
        bool charging = false;
        int mv = 0;
        std::vector<uint8_t> reply;
        bool got = false;
        for (int attempt = 0; attempt < 3 && !got; ++attempt) {
            if (attempt) Sleep(180);
            // The first status query wakes a sleeping mouse; some devices ignore
            // the first battery query after the link has been idle.
            link.Exchange(0x03, reply);
            Sleep(25);
            if (!link.Exchange(0x04, reply)) continue;
            int p = 0, c = 0, volts = 0;
            if (reply[0] == 8) {
                p = reply[6];
                c = reply[7];
                volts = (reply[8] << 8) | reply[9];
            } else {
                p = reply[5];
                c = reply[6];
                volts = (reply[7] << 8) | reply[8];
            }
            if (p >= 0 && p <= 100 && (p > 0 || c != 0 || volts > 0)) {
                pct = p;
                charging = c != 0;
                mv = volts;
                got = true;
            }
        }
        if (!got && link.Exchange(0x17, reply)) {
            int idx = reply[0] == 8 ? 5 : 4;
            if ((int)reply.size() > idx) {
                int p = reply[idx] * 2;
                if (p > 0 && p <= 100) {
                    pct = p;
                    got = true;
                }
            }
        }

        bool knownVid = g.vid == 0x3554 || g.vid == 0x373B || g.vid == 0x25A7 ||
                        g.vid == 0x3367 || g.vid == 0x33E4 || g.vid == 0x3151;
        if (!got && !knownVid) continue;

        Device d;
        d.id = L"C:" + g.base;
        d.proto = Proto::Compx;
        d.vid = g.vid;
        d.pid = g.pid;
        d.basePath = g.base;
        d.path = g.outIface.path;
        d.ifaces = all;
        d.name = CompxName(g.vid, g.product);
        d.product = g.product;
        d.manufacturer = L"Compx 方案";
        d.kind = ContainsNoCase(g.product, L"keyboard")
                     ? Kind::Keyboard
                     : (ContainsNoCase(g.product, L"headset") ? Kind::Headset : Kind::Mouse);
        d.conn = CompxConnection(g.vid, g.pid, g.product);
        d.online = true;
        d.battery = pct;
        d.batteryValid = got;
        d.charging = charging;
        d.millivolts = mv;
        if (!got) {
            d.note = L"接收器在线，鼠标休眠或已关机；移动鼠标后刷新";
        } else if (g.vid == 0x3554 && (g.pid == 0xF527 || g.pid == 0xF526 ||
                                       g.pid == 0xF524 || g.pid == 0xF50F ||
                                       g.pid == 0xF50D)) {
            CompxReadZaopin(link, d);
            if (d.dpi.valid) {
                d.note = L"Zaopin Z4D · 电量/DPI/回报率/去抖只读 · 写入请用官方 Mouse Drive";
            } else {
                d.note = L"Zaopin Z4D · 电量已读取 · DPI 闪存读取失败（移动鼠标后刷新）";
            }
        } else {
            CompxReadDpi(link, d.dpi);
            if (d.dpi.valid) {
                d.note = d.dpi.note;
            } else if (ContainsNoCase(g.product, L"wireless")) {
                d.note = L"公版 Compx 协议：电量可读，DPI 档位未能读取";
            }
        }
        out.push_back(std::move(d));
    }

    // ---- Nordic 54 -------------------------------------------------------
    for (const auto& g : BuildNordicGroups(all)) {
        NordicLink link;
        if (!link.Open(g)) continue;
        int pct = -1;
        int mv = 0;
        uint8_t usedLink = 1;
        if (!link.Query(1, pct, mv)) {
            usedLink = 0;
            if (!link.Query(0, pct, mv)) {
                // Still expose the device when the receiver is present but the mouse sleeps.
                Device d;
                d.id = L"N:" + g.base;
                d.proto = Proto::Nordic54;
                d.vid = g.vid;
                d.pid = g.pid;
                d.basePath = g.base;
                d.path = g.outIface.path;
                d.ifaces = all;
                d.name = ContainsNoCase(g.product, L"wireless") ? L"ATK / VXE 无线鼠标" : g.product;
                d.product = g.product;
                d.manufacturer = L"Nordic 54";
                d.kind = Kind::Mouse;
                d.conn = Conn::Wireless24;
                d.online = false;
                d.note = L"接收器在线，鼠标休眠或已关机";
                out.push_back(std::move(d));
                continue;
            }
        }
        Device d;
        d.id = L"N:" + g.base;
        d.proto = Proto::Nordic54;
        d.vid = g.vid;
        d.pid = g.pid;
        d.basePath = g.base;
        d.path = g.outIface.path;
        d.ifaces = all;
        d.name = ContainsNoCase(g.product, L"wireless") ? L"ATK / VXE 无线鼠标" : g.product;
        d.product = g.product;
        d.manufacturer = L"Nordic 54";
        d.kind = Kind::Mouse;
        d.conn = usedLink ? Conn::Wireless24 : Conn::Wired;
        d.online = true;
        d.battery = pct;
        d.batteryValid = pct > 0;
        d.millivolts = mv;
        d.note = L"Nordic 54 协议：电量由接收器缓存，DPI 未公开";
        out.push_back(std::move(d));
    }

    // ---- Aurora / mouse.xyz ---------------------------------------------
    for (const auto& g : BuildAuroraGroups(all)) {
        int pct = -1;
        bool charging = false;
        if (!AuroraRead(g, pct, charging)) continue;
        Device d;
        d.id = L"U:" + g.base;
        d.proto = Proto::Aurora;
        d.vid = g.vid;
        d.pid = g.pid;
        d.basePath = g.base;
        d.path = g.iface.path;
        d.ifaces = all;
        d.name = g.product.empty() ? L"LAMZU / G-Wolves 无线鼠标" : g.product;
        d.product = g.product;
        d.manufacturer = L"Aurora";
        d.kind = ContainsNoCase(g.product, L"keyboard") ? Kind::Keyboard
                                                       : (ContainsNoCase(g.product, L"headset")
                                                              ? Kind::Headset
                                                              : Kind::Mouse);
        d.conn = ContainsNoCase(g.product, L"wired") ? Conn::Wired : Conn::Wireless24;
        d.online = true;
        d.battery = pct;
        d.batteryValid = true;
        d.charging = charging;
        d.note = L"反向自 mouse.xyz / Aurora 网页驱动，仅保留电量读取";
        out.push_back(std::move(d));
    }

    // ---- Cherry ----------------------------------------------------------
    for (const auto& h : all) {
        if (h.vid != 0x046A) continue;
        if (h.pid != 0x01AC && h.pid != 0x00EA) continue;
        if (h.outLen < 64) continue;
        if (!ContainsNoCase(h.path, L"col04")) continue;
        int pct = -1;
        bool charging = false;
        if (!CherryRead(h, pct, charging)) continue;
        Device d;
        d.id = L"H:" + h.basePath;
        d.proto = Proto::Cherry;
        d.vid = h.vid;
        d.pid = h.pid;
        d.basePath = h.basePath;
        d.path = h.path;
        d.ifaces = all;
        d.name = h.product.empty() ? L"CHERRY 无线键盘" : h.product;
        d.product = h.product;
        d.manufacturer = L"CHERRY";
        d.kind = Kind::Keyboard;
        d.conn = ContainsNoCase(h.product, L"bluetooth") ? Conn::Bluetooth : Conn::Wireless24;
        d.online = true;
        d.battery = pct;
        d.batteryValid = true;
        d.charging = charging;
        out.push_back(std::move(d));
    }

    // ---- Lofree Hyzen ----------------------------------------------------
    for (const auto& h : all) {
        if (h.vid != 0x388D || h.pid != 0x0025) continue;
        if (h.usagePage != 0xFF1C) continue;
        if (h.outLen < 63) continue;
        int pct = -1;
        if (!LofreeRead(h, pct)) continue;
        Device d;
        d.id = L"F:" + h.basePath;
        d.proto = Proto::Lofree;
        d.vid = h.vid;
        d.pid = h.pid;
        d.basePath = h.basePath;
        d.path = h.path;
        d.ifaces = all;
        d.name = h.product.empty() ? L"Lofree 无线键盘" : h.product;
        d.product = h.product;
        d.manufacturer = L"Lofree";
        d.kind = Kind::Keyboard;
        d.conn = Conn::Wireless24;
        d.online = true;
        d.battery = pct;
        d.batteryValid = true;
        d.note = L"反向自 Lofree Hub WebHID，仅保留电量读取";
        out.push_back(std::move(d));
    }
}

bool ApplyGeneric(const Device& dev, const ApplyRequest& req, std::wstring& err) {
    (void)dev;
    (void)req;
    err = L"该公版/网页驱动协议目前只读，不支持写入设置";
    return false;
}

}  // namespace proto

namespace proto {

void AddSystemBatteries(std::vector<Device>& out) {
    auto batteries = EnumerateSystemBatteries();
    for (const auto& sb : batteries) {
        bool duplicate = false;
        for (const auto& d : out) {
            if (d.batteryValid &&
                (ContainsNoCase(d.name, sb.name.c_str()) ||
                 ContainsNoCase(sb.name, d.name.c_str()))) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        Device d;
        d.id = L"B:" + sb.id;
        d.proto = Proto::SystemBattery;
        d.name = sb.name;
        d.product = sb.name;
        d.manufacturer = L"Windows HID 电池驱动";
        d.kind = ContainsNoCase(sb.name, L"keyboard")
                     ? Kind::Keyboard
                     : (ContainsNoCase(sb.name, L"headset") ? Kind::Headset : Kind::Mouse);
        d.conn = ContainsNoCase(sb.name, L"bluetooth") ? Conn::Bluetooth : Conn::Wireless24;
        d.online = true;
        d.battery = sb.percent;
        d.batteryValid = sb.percent >= 0;
        d.charging = sb.charging;
        d.millivolts = sb.millivolts;
        d.note = L"由 Windows 的 HID 电池驱动提供，通用回退路径";
        out.push_back(std::move(d));
    }
}

}  // namespace proto
