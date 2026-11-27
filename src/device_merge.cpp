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

// 优先选择真正在线且有有效电量读数的条目。两种传输同时在线时优先有线：
// 插线即代表设备进入有线模式（UI 要显示“USB 有线 · 无线模式”），而接收器
// 可能仍在上报旧的无线读数，不能让它压过当前有线链路。其余评分项不变。
int MergeScore(const Device& d) {
    int score = 0;
    if (d.online) score += 1000;
    if (d.batteryValid) score += 100;
    if (d.charging) score += 50;
    // 插线即有线模式：有线链路在线说明数据线已物理连接，此时对所有机型都
    // 优先有线条目，以显示“USB 有线 · 无线模式”并保留电量；接收器保留的
    // 旧无线读数不应胜出。离线有线条目不参与该加分。
    // 有线链路在线 = 用户真的把线插上了。这个信号必须压过“旧无线读数/充电位”
    // 等派生分数，否则会出现插着线却显示「2.4G 无线」。
    if (d.online && d.conn == Conn::Wired) score += 200;
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
        // 充电状态必须和电量一起复制：Finalize 会先把“暂不适用电池”的有线条目
        // chargingKnown 置 false，如果这里只补电量不补 chargingKnown，合并后
        // batteryApplicable=true 但充电状态永远显示未知（chg=-1）。
        dst.chargingKnown = src.chargingKnown;
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
            d.chargingKnown = old->chargingKnown;
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

    // 离线设备不展示：未在本次扫描中出现的旧设备直接丢弃，本次扫描里
    // online=false 的接收器/休眠设备也过滤掉。只保留当前在线设备。
    fresh.erase(std::remove_if(fresh.begin(), fresh.end(),
                               [](const Device& d) { return !d.online; }),
                fresh.end());

    std::stable_sort(fresh.begin(), fresh.end(), [](const Device& a, const Device& b) {
        if (a.online != b.online) return a.online > b.online;
        int ra = KindRank(a.kind), rb = KindRank(b.kind);
        if (ra != rb) return ra < rb;
        return Lower(a.name) < Lower(b.name);
    });
    return fresh;
}
