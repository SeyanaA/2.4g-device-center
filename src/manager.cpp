#include "manager.h"

#include <map>

namespace {

int KindRank(Kind k) {
    switch (k) {
        case Kind::Mouse: return 0;
        case Kind::Keyboard: return 1;
        case Kind::Headset: return 2;
        default: return 3;
    }
}

std::vector<Device> MergeSnapshots(const std::vector<Device>& oldDevices,
                                    std::vector<Device> fresh) {
    std::map<std::wstring, const Device*> oldById;
    for (const auto& d : oldDevices) oldById[d.id] = &d;

    for (auto& d : fresh) {
        auto it = oldById.find(d.id);
        if (it == oldById.end()) continue;
        const Device& old = *it->second;
        bool stale = false;
        if (!d.batteryValid && old.batteryValid) {
            d.battery = old.battery;
            d.charging = old.charging;
            d.millivolts = old.millivolts;
            d.batteryValid = true;
            stale = true;
        }
        if (!d.dpi.valid && old.dpi.valid) {
            d.dpi = old.dpi;
            stale = true;
        }
        if (!d.polling.valid && old.polling.valid) {
            d.polling = old.polling;
            stale = true;
        }
        if (!d.debounce.valid && old.debounce.valid) {
            d.debounce = old.debounce;
            stale = true;
        }
        if (!d.angle.valid && old.angle.valid) {
            d.angle = old.angle;
            stale = true;
        }
        if (stale) {
            d.stale = true;
            if (d.note.empty()) {
                d.note = d.online ? L"设备休眠，显示上次读数" : L"未检测到设备，显示上次读数";
            }
        }
    }

    std::map<std::wstring, bool> seen;
    for (const auto& d : fresh) seen[d.id] = true;
    for (const auto& old : oldDevices) {
        if (seen[old.id]) continue;
        Device d = old;
        d.online = false;
        d.stale = true;
        d.note = L"未检测到设备（显示上次读数）";
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

}  // namespace

DeviceManager::~DeviceManager() { Stop(); }

void DeviceManager::Start(HWND notifyWnd, bool demo) {
    Stop();
    notifyWnd_ = notifyWnd;
    demo_ = demo;
    pollSeconds_ = ClampInt(ReadIniInt(L"general", L"poll_seconds", 5), 2, 300);
    if (demo_) demoDevices_ = BuildDemoDevices();
    running_ = true;
    thread_ = std::thread(&DeviceManager::Run, this);
}

void DeviceManager::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DeviceManager::RequestRefresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshRequested_ = true;
    }
    cv_.notify_all();
}

void DeviceManager::SetPollSeconds(int seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pollSeconds_ = ClampInt(seconds, 2, 300);
    }
    cv_.notify_all();
}

int DeviceManager::PollSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pollSeconds_;
}

void DeviceManager::Apply(const ApplyRequest& req) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(req);
    }
    cv_.notify_all();
}

Snapshot DeviceManager::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void DeviceManager::PostSnapshot() {
    if (notifyWnd_) PostMessageW(notifyWnd_, WM_APP_SNAPSHOT, 0, 0);
}

void DeviceManager::Publish(std::vector<Device> devices, const std::wstring& status) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.devices = std::move(devices);
        snapshot_.updated = NowTimeString();
        snapshot_.status = status;
        snapshot_.scanning = false;
    }
    PostSnapshot();
}

void DeviceManager::Run() {
    ScanOnce();
    while (running_) {
        std::deque<ApplyRequest> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(pollSeconds_), [&] {
                return !running_ || refreshRequested_ || !queue_.empty();
            });
            if (!running_) break;
            work.swap(queue_);
            refreshRequested_ = false;
        }
        for (const auto& req : work) {
            if (!running_) break;
            ProcessApply(req);
        }
        if (!work.empty()) Sleep(180);
        if (!running_) break;
        ScanOnce();
    }
}

void DeviceManager::ScanOnce() {
    if (demo_) {
        std::vector<Device> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = demoDevices_;
        }
        Publish(std::move(copy), L"演示模式 · 未连接真实设备");
        return;
    }

    auto begin = GetTickCount64();
    std::vector<HidInterface> ifaces = EnumerateHidInterfaces();
    std::vector<Device> devices;

    proto::ReadLogitech(ifaces, devices);
    proto::ReadAsus(ifaces, devices);
    proto::ReadRazer(ifaces, devices);
    proto::ReadRapoo(ifaces, devices);
    proto::ReadGeneric(ifaces, devices);
    proto::AddSystemBatteries(devices);

    Snapshot old = GetSnapshot();
    devices = MergeSnapshots(old.devices, std::move(devices));

    int online = 0, withBattery = 0;
    for (const auto& d : devices) {
        if (d.online) ++online;
        if (d.batteryValid) ++withBattery;
    }
    auto elapsed = GetTickCount64() - begin;
    std::wstring status = Fmt(L"%d 个设备在线 · %d 个有电量读数 · 扫描 %llu ms", online,
                              withBattery, (unsigned long long)elapsed);
    if (devices.empty()) {
        status = L"未发现兼容设备 · 请连接 2.4G 接收器并移动鼠标唤醒";
    }
    Publish(std::move(devices), status);
}

void DeviceManager::ProcessApply(const ApplyRequest& req) {
    Snapshot snap = GetSnapshot();
    const Device* target = nullptr;
    for (const auto& d : snap.devices) {
        if (d.id == req.deviceId) {
            target = &d;
            break;
        }
    }
    bool ok = false;
    std::wstring msg;
    if (!target) {
        msg = L"设备已断开，无法应用设置";
    } else if (demo_) {
        ApplyDemo(req, ok, msg);
    } else {
        switch (target->proto) {
            case Proto::Logitech:
                ok = proto::ApplyLogitech(*target, req, msg);
                break;
            case Proto::Asus:
                ok = proto::ApplyAsus(*target, req, msg);
                break;
            case Proto::Razer:
                ok = proto::ApplyRazer(*target, req, msg);
                break;
            case Proto::Rapoo:
                ok = proto::ApplyRapoo(*target, req, msg);
                break;
            case Proto::Compx:
                ok = proto::ApplyGeneric(*target, req, msg);
                break;
            case Proto::Nordic54:
            case Proto::Aurora:
            case Proto::Cherry:
            case Proto::Lofree:
            case Proto::SystemBattery:
                msg = L"该协议只读，不支持修改设置";
                break;
            default:
                msg = L"未知协议";
                break;
        }
    }
    if (ok) {
        if (!demo_) msg = L"设置已写入设备";
    } else if (msg.empty()) {
        msg = L"写入失败";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok) refreshRequested_ = true;
        snapshot_.applySerial++;
        snapshot_.applyOk = ok;
        snapshot_.applyMessage = msg;
        snapshot_.applyDeviceId = req.deviceId;
    }
    LogLine(L"Apply %s -> %s: %s", target ? target->name.c_str() : L"(missing)",
            ok ? L"OK" : L"FAIL", msg.c_str());
    if (notifyWnd_) {
        PostMessageW(notifyWnd_, WM_APP_APPLY_RESULT, 0, 0);
    }
}

std::vector<Device> DeviceManager::BuildDemoDevices() const {
    std::vector<Device> v;

    {
        Device d;
        d.id = L"demo-logi";
        d.name = L"PRO X SUPERLIGHT 2";
        d.product = d.name;
        d.manufacturer = L"Logitech";
        d.vid = 0x046D;
        d.pid = 0xC54D;
        d.proto = Proto::Logitech;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 87;
        d.batteryValid = true;
        d.millivolts = 4050;
        d.dpi.valid = true;
        d.dpi.canWrite = true;
        d.dpi.x = 1600;
        d.dpi.y = 1600;
        d.dpi.min = 100;
        d.dpi.max = 32000;
        d.dpi.step = 50;
        d.dpi.stages = {400, 800, 1600, 3200};
        d.dpi.activeStage = 2;
        d.polling.valid = true;
        d.polling.canWrite = true;
        d.polling.currentHz = 1000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000, 8000};
        d.note = L"演示数据 · HID++ 2.0";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-asus";
        d.name = L"ROG Harpe Ace Aim Lab";
        d.product = d.name;
        d.manufacturer = L"ASUSTeK";
        d.vid = 0x0B05;
        d.pid = 0x1A94;
        d.proto = Proto::Asus;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 64;
        d.batteryValid = true;
        d.charging = true;
        d.dpi.valid = true;
        d.dpi.canWrite = true;
        d.dpi.x = 1600;
        d.dpi.y = 1600;
        d.dpi.min = 50;
        d.dpi.max = 36000;
        d.dpi.step = 50;
        d.dpi.stages = {400, 800, 1600, 3200};
        d.dpi.activeStage = 2;
        d.polling.valid = true;
        d.polling.canWrite = true;
        d.polling.currentHz = 1000;
        d.polling.options = {125, 250, 500, 1000};
        d.debounce.valid = true;
        d.debounce.canWrite = true;
        d.debounce.currentMs = 8;
        d.debounce.options = {4, 8, 12, 16, 20, 24, 28, 32};
        d.angle.valid = true;
        d.angle.canWrite = true;
        d.angle.on = true;
        d.note = L"演示数据 · ASUS 私有 HID";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-razer";
        d.name = L"Razer Viper V3 Pro";
        d.product = d.name;
        d.manufacturer = L"Razer";
        d.vid = 0x1532;
        d.pid = 0x00B4;
        d.proto = Proto::Razer;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 43;
        d.batteryValid = true;
        d.dpi.valid = true;
        d.dpi.canWrite = true;
        d.dpi.x = 3200;
        d.dpi.y = 3200;
        d.dpi.min = 100;
        d.dpi.max = 45000;
        d.dpi.step = 50;
        d.polling.valid = true;
        d.polling.canWrite = true;
        d.polling.currentHz = 4000;
        d.polling.options = {125, 500, 1000, 2000, 4000, 8000};
        d.note = L"演示数据 · Razer 90 字节 HID";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-rapoo";
        d.name = L"Rapoo VT3 Air";
        d.product = d.name;
        d.manufacturer = L"Rapoo";
        d.vid = 0x24AE;
        d.pid = 0x1413;
        d.proto = Proto::Rapoo;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 23;
        d.batteryValid = true;
        d.dpi.valid = true;
        d.dpi.canWrite = true;
        d.dpi.x = 1600;
        d.dpi.y = 1600;
        d.dpi.min = 50;
        d.dpi.max = 60000;
        d.dpi.step = 50;
        d.dpi.stages = {400, 800, 1200, 1600, 2400, 3200, 6400};
        d.dpi.activeStage = 3;
        d.polling.valid = true;
        d.polling.canWrite = true;
        d.polling.currentHz = 2000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000, 8000};
        d.debounce.valid = true;
        d.debounce.canWrite = true;
        d.debounce.currentMs = 4;
        d.debounce.options = {0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};
        d.angle.valid = true;
        d.angle.canWrite = true;
        d.angle.on = false;
        d.note = L"演示数据 · Rapoo 状态报文 + EEPROM";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-compx";
        d.name = L"VXE R1 SE+";
        d.product = d.name;
        d.manufacturer = L"ATK / VXE";
        d.vid = 0x373B;
        d.pid = 0x1085;
        d.proto = Proto::Compx;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 71;
        d.batteryValid = true;
        d.millivolts = 3980;
        d.dpi.valid = true;
        d.dpi.currentKnown = false;
        d.dpi.stages = {400, 800, 1600, 3200, 6400};
        d.dpi.min = 400;
        d.dpi.max = 6400;
        d.dpi.step = 50;
        d.dpi.note = L"公版协议未公开当前档位，显示档位表";
        d.note = L"演示数据 · Compx 17 字节报文";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-zaopin";
        d.name = L"Zaopin Z4D";
        d.product = d.name;
        d.manufacturer = L"JinLeMa / Zaopin";
        d.vid = 0x3554;
        d.pid = 0xF527;
        d.proto = Proto::Compx;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 50;
        d.batteryValid = true;
        d.millivolts = 3879;
        d.dpi.valid = true;
        d.dpi.canWrite = true;
        d.dpi.currentKnown = true;
        d.dpi.x = 3100;
        d.dpi.y = 3100;
        d.dpi.min = 50;
        d.dpi.max = 26000;
        d.dpi.step = 50;
        d.dpi.stages = {3100};
        d.dpi.activeStage = 0;
        d.polling.valid = true;
        d.polling.canWrite = false;
        d.polling.currentHz = 4000;
        d.polling.options = {125, 250, 500, 1000, 2000, 4000, 8000};
        d.debounce.valid = true;
        d.debounce.canWrite = false;
        d.debounce.currentMs = 8;
        d.debounce.options = {0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32};
        d.note = L"演示数据 · Zaopin Z4D · DPI 可写（自检+回读）";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-kbd";
        d.name = L"ROG Azoth";
        d.product = d.name;
        d.manufacturer = L"ASUSTeK";
        d.vid = 0x0B05;
        d.pid = 0x1A85;
        d.proto = Proto::Asus;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Keyboard;
        d.online = true;
        d.battery = 92;
        d.batteryValid = true;
        d.note = L"演示数据 · 键盘电量";
        v.push_back(d);
    }
    {
        Device d;
        d.id = L"demo-headset";
        d.name = L"G733 LIGHTSPEED";
        d.product = d.name;
        d.manufacturer = L"Logitech";
        d.vid = 0x046D;
        d.pid = 0x0AB5;
        d.proto = Proto::Logitech;
        d.conn = Conn::Wireless24;
        d.kind = Kind::Headset;
        d.online = true;
        d.battery = 55;
        d.batteryValid = true;
        d.millivolts = 3900;
        d.note = L"演示数据 · 耳机电量";
        v.push_back(d);
    }
    return v;
}

void DeviceManager::ApplyDemo(const ApplyRequest& req, bool& ok, std::wstring& msg) {
    ok = false;
    msg = L"演示设备不存在";
    for (auto& d : demoDevices_) {
        if (d.id != req.deviceId) continue;
        if (req.setDpi) {
            int stage = req.stageIndex >= 0 ? req.stageIndex
                                            : (d.dpi.activeStage >= 0 ? d.dpi.activeStage : 0);
            if (!d.dpi.stages.empty() && stage >= 0 && stage < (int)d.dpi.stages.size()) {
                d.dpi.stages[stage] = req.dpiX;
                d.dpi.activeStage = stage;
            }
            d.dpi.x = req.dpiX;
            d.dpi.y = req.dpiY > 0 ? req.dpiY : req.dpiX;
            d.dpi.currentKnown = true;
        }
        if (req.selectStage && req.stageIndex >= 0 &&
            req.stageIndex < (int)d.dpi.stages.size()) {
            d.dpi.activeStage = req.stageIndex;
            d.dpi.x = d.dpi.stages[req.stageIndex];
            d.dpi.y = d.dpi.x;
        }
        if (req.setPolling) d.polling.currentHz = req.pollingHz;
        if (req.setDebounce) d.debounce.currentMs = req.debounceMs;
        if (req.setAngle) d.angle.on = req.angleOn;
        ok = true;
        msg = L"演示模式：设置已应用（未写入真实硬件）";
        break;
    }
}
