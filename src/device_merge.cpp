#include "device_merge.h"

#include <algorithm>
#include <map>

namespace {

std::wstring IdentityKey(const Device& d) {
    return d.identity.empty() ? d.id : d.identity;
}

int KindRank(Kind k) {
    switch (k) {
        case Kind::Mouse: return 0;
        case Kind::Keyboard: return 1;
        case Kind::Headset: return 2;
        default: return 3;
    }
}

// Prefer an entry that is actually answering and has a live battery reading.
// When both transports are healthy, prefer the 2.4G link: it is the link the
// official driver reads, and the two links can differ by one percentage point.
// A charging wired link still outranks a non-charging wireless one.
int MergeScore(const Device& d) {
    int score = 0;
    if (d.online) score += 1000;
    if (d.batteryValid) score += 100;
    if (d.charging) score += 50;
    // Zaopin 优先 2.4G（官方驱动读取的链路）；Valkyrie 有线/接收器同时在线时
    // 优先有线，因为接收器可能保留旧的无线读数。
    if (d.identity.rfind(L"valkyrie:", 0) == 0) {
        if (d.conn == Conn::Wired) score += 10;
    } else if (d.conn == Conn::Wireless24) {
        score += 10;
    }
    if (d.dpi.valid) score += 4;
    if (d.polling.valid) score += 2;
    if (d.debounce.valid) score += 1;
    if (d.angle.valid) score += 1;
    return score;
}

bool CopyMissing(Device& dst, const Device& src) {
    bool copied = false;
    if (!dst.batteryValid && src.batteryValid) {
        dst.battery = src.battery;
        dst.charging = src.charging;
        dst.millivolts = src.millivolts;
        dst.batteryValid = true;
        copied = true;
    }
    if (!dst.dpi.valid && src.dpi.valid) {
        dst.dpi = src.dpi;
        copied = true;
    }
    if (!dst.polling.valid && src.polling.valid) {
        dst.polling = src.polling;
        copied = true;
    }
    if (!dst.debounce.valid && src.debounce.valid) {
        dst.debounce = src.debounce;
        copied = true;
    }
    if (!dst.angle.valid && src.angle.valid) {
        dst.angle = src.angle;
        copied = true;
    }
    return copied;
}

void Canonicalize(Device& d) {
    if (d.identity == L"zaopin:z4d") {
        // Both the receiver (PID F527) and the charging cable (PID F526) report
        // different product strings; show the model name so the merged entry
        // does not look like a second device.
        d.name = L"Zaopin Z4D";
    } else if (d.identity == L"valkyrie:vk-m3-lite") {
        d.name = L"Valkyrie VK M3 Lite";
    } else if (d.identity == L"valkyrie:vk-m3") {
        d.name = L"Valkyrie VK M3";
    }
}

}  // namespace

std::wstring DeviceIdentity(const Device& d) { return IdentityKey(d); }

void DedupeDevicesByIdentity(std::vector<Device>& devices) {
    std::map<std::wstring, size_t> index;
    std::vector<Device> out;
    out.reserve(devices.size());

    for (auto& d : devices) {
        Canonicalize(d);
        const std::wstring key = IdentityKey(d);
        auto it = index.find(key);
        if (it == index.end()) {
            index.emplace(key, out.size());
            out.push_back(std::move(d));
            continue;
        }

        Device& primary = out[it->second];
        if (MergeScore(d) > MergeScore(primary)) {
            Device replacement = std::move(d);
            CopyMissing(replacement, primary);
            primary = std::move(replacement);
            Canonicalize(primary);
        } else {
            CopyMissing(primary, d);
            Canonicalize(primary);
        }
    }

    devices = std::move(out);
}

std::vector<Device> MergeDeviceSnapshots(const std::vector<Device>& oldDevices,
                                         std::vector<Device> fresh) {
    DedupeDevicesByIdentity(fresh);

    std::map<std::wstring, const Device*> oldById;
    std::map<std::wstring, const Device*> oldByIdentity;
    for (const auto& d : oldDevices) {
        oldById[d.id] = &d;
        if (!d.identity.empty()) oldByIdentity.emplace(d.identity, &d);
    }

    for (auto& d : fresh) {
        const Device* old = nullptr;
        auto it = oldById.find(d.id);
        if (it != oldById.end()) {
            old = it->second;
        } else if (!d.identity.empty()) {
            auto jt = oldByIdentity.find(d.identity);
            if (jt != oldByIdentity.end()) old = jt->second;
        }
        if (!old) continue;

        bool stale = false;
        if (!d.batteryValid && old->batteryValid) {
            d.battery = old->battery;
            d.charging = old->charging;
            d.millivolts = old->millivolts;
            d.batteryValid = true;
            stale = true;
        }
        if (!d.dpi.valid && old->dpi.valid) {
            d.dpi = old->dpi;
            stale = true;
        }
        if (!d.polling.valid && old->polling.valid) {
            d.polling = old->polling;
            stale = true;
        }
        if (!d.debounce.valid && old->debounce.valid) {
            d.debounce = old->debounce;
            stale = true;
        }
        if (!d.angle.valid && old->angle.valid) {
            d.angle = old->angle;
            stale = true;
        }
        if (stale) {
            d.stale = true;
            if (d.note.empty()) {
                d.note = d.online ? L"设备休眠，显示上次读数" : L"未检测到设备，显示上次读数";
            }
        }
    }

    std::map<std::wstring, bool> seenId;
    std::map<std::wstring, bool> seenIdentity;
    for (const auto& d : fresh) {
        seenId[d.id] = true;
        if (!d.identity.empty()) seenIdentity[d.identity] = true;
    }
    for (const auto& old : oldDevices) {
        if (seenId[old.id]) continue;
        // The same mouse switched from the receiver to USB (or back). Keep one
        // entry with the live transport instead of retaining the old one as a
        // second device with a stale battery reading.
        if (!old.identity.empty() && seenIdentity[old.identity]) continue;
        Device d = old;
        d.online = false;
        d.stale = true;
        d.note = L"未检测到设备（显示上次读数）";
        Canonicalize(d);
        fresh.push_back(std::move(d));
    }

    std::stable_sort(fresh.begin(), fresh.end(), [](const Device& a, const Device& b) {
        if (a.online != b.online) return a.online > b.online;
        int ra = KindRank(a.kind), rb = KindRank(b.kind);
        if (ra != rb) return ra < rb;
        return Lower(a.name) < Lower(b.name);
    });
    return fresh;
}
