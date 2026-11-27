#include "manager.h"

#include <ctime>
#include <map>

#include "device_merge.h"
#include "event_log.h"
#include "io_cancel.h"

namespace {

// B01：工作线程入口绑定自身句柄，让 Stop() 的 CancelSynchronousIo 能打断
// 阻塞中的协议读取；离开时解绑。Reset() 清掉上一次退出留下的取消状态。
struct WorkerBinding {
    WorkerBinding() {
        iocancel::Reset();
        iocancel::BindCurrentThread();
    }
    ~WorkerBinding() { iocancel::UnbindCurrentThread(); }
    WorkerBinding(const WorkerBinding&) = delete;
    WorkerBinding& operator=(const WorkerBinding&) = delete;
};

bool StopRequested() { return iocancel::Requested(); }

std::wstring IdentityOf(const Device& d) { return d.identity.empty() ? d.id : d.identity; }

int ChargingFlag(const Device& d) {
    // 设备没有充电标志时写 -1，历史曲线按“未知”处理，不伪报放电/充电。
    return d.chargingKnown ? (d.charging ? 1 : 0) : -1;
}

std::wstring ConnectDetail(const Device& d) {
    std::wstring fields = EventFields(d.batteryValid ? d.battery : -1, d.millivolts,
                                      ChargingFlag(d));
    if (fields.empty()) return ConnName(d.conn);
    return Fmt(L"%s · %s", ConnName(d.conn), fields.c_str());
}

// 设备事件日志：记录真实的状态跳变，5s 轮询的相同读数不落盘。
// 另每 30 分钟补一条 SAMPLE，让“电量不变”时历史曲线仍有走势。
// 事件日志是排查电量问题的证据来源，写失败不影响扫描（见 docs/SPEC-logging.md）。
void LogSnapshotChanges(const std::vector<Device>& before, const std::vector<Device>& now,
                        std::map<std::wstring, int64_t>& lastSample) {
    std::map<std::wstring, const Device*> old;
    for (const auto& d : before) old[IdentityOf(d)] = &d;
    const int64_t nowEpoch = (int64_t)time(nullptr);

    std::map<std::wstring, bool> seen;
    for (const auto& d : now) {
        const std::wstring key = IdentityOf(d);
        seen[key] = true;
        auto it = old.find(key);
        if (it == old.end()) {
            LogDeviceEvent(L"CONNECT", key, d.name, ConnectDetail(d));
            if (d.batteryApplicable && d.batteryValid) lastSample[key] = nowEpoch;
            continue;
        }
        const Device& prev = *it->second;
        if (d.batteryApplicable && d.batteryValid && d.battery >= 0 &&
            (!prev.batteryValid || prev.battery != d.battery)) {
            std::wstring detail =
                prev.batteryValid ? Fmt(L"pct %d->%d", prev.battery, d.battery)
                                  : Fmt(L"pct %d", d.battery);
            if (d.millivolts > 0 || prev.millivolts > 0) {
                detail += Fmt(L" · mv %d->%d", prev.millivolts, d.millivolts);
            }
            if (d.chargingKnown) detail += Fmt(L" · chg=%d", d.charging ? 1 : 0);
            LogDeviceEvent(L"BATTERY", key, d.name, detail);
            lastSample[key] = nowEpoch;
        }
        if (prev.batteryApplicable && prev.batteryValid && d.batteryApplicable &&
            d.batteryValid && prev.chargingKnown && d.chargingKnown &&
            d.charging != prev.charging) {
            LogDeviceEvent(L"CHARGE", key, d.name,
                           (prev.charging ? L"充电中" : L"电池供电") + std::wstring(L" -> ") +
                               (d.charging ? L"充电中" : L"电池供电") +
                               Fmt(L" · chg=%d", d.charging ? 1 : 0));
        }

        auto s = lastSample.find(key);
        int64_t last = s == lastSample.end() ? 0 : s->second;
        if (d.batteryApplicable && d.batteryValid && d.battery >= 0 &&
            ShouldLogBatterySample(last, nowEpoch)) {
            lastSample[key] = nowEpoch;
            LogDeviceEvent(L"SAMPLE", key, d.name,
                           EventFields(d.battery, d.millivolts, ChargingFlag(d)));
        }
    }

    for (const auto& d : before) {
        if (seen.count(IdentityOf(d))) continue;
        // 电量不适用的有线/蓝牙设备没有“最后读数”，直接标注连接方式已断开；
        // 有电池属性的设备仍保留原来的最后读数详情。
        std::wstring detail;
        if (!d.batteryApplicable) {
            detail = Fmt(L"%s 设备已断开", ConnName(d.conn));
        } else if (d.batteryValid) {
            detail = L"最后读数 " + EventFields(d.battery, d.millivolts, ChargingFlag(d));
        } else {
            detail = L"最后读数 无电量读数";
        }
        LogDeviceEvent(L"DISCONNECT", IdentityOf(d), d.name, detail);
    }
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
    // B01：先请求协作式取消，再唤醒等待。阻塞在协议读取里的工作线程会被
    // CancelSynchronousIo 打断，最多再走完当前 80 ms 切片，不会等整轮扫描结束。
    iocancel::Request();
    cv_.notify_all();
    if (thread_.joinable()) {
        const ULONGLONG begin = GetTickCount64();
        thread_.join();
        // 一行耗时便于回归：正常应是个位数到几十毫秒；超过一轮扫描说明有
        // 驱动不响应取消。
        LogLine(L"Manager: stopped in %llu ms", (unsigned long long)(GetTickCount64() - begin));
    }
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
    WorkerBinding binding;
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

    // B01：协议之间检查取消。单个协议内部的阻塞 I/O 由 hid.cpp 的切片检查
    // 与 CancelSynchronousIo 负责，这里保证不会把 11 个协议全部跑完。
    proto::ReadLogitech(ifaces, devices);
    if (StopRequested()) return;
    proto::ReadAsus(ifaces, devices);
    if (StopRequested()) return;
    proto::ReadRazer(ifaces, devices);
    if (StopRequested()) return;
    proto::ReadRapoo(ifaces, devices);
    if (StopRequested()) return;
    proto::ReadGeneric(ifaces, devices);
    if (StopRequested()) return;
    proto::ReadValkyrie(ifaces, devices);
    if (StopRequested()) return;
    proto::ReadMoreBetter(ifaces, devices);
    if (StopRequested()) return;
    // USB/蓝牙通用回退：已知协议已覆盖的 VID/PID 由 AddWiredHidDevices 自行跳过。
    proto::AddWiredHidDevices(ifaces, devices);
    if (StopRequested()) return;
    // 经典蓝牙音频耳机没有 HID 接口，必须单独枚举；否则 Buds/Headset 这类
    // 设备在列表里完全不可见。
    proto::AddBluetoothAudioDevices(EnumerateBluetoothAudioDevices(), devices);
    // AddSystemBatteries 依据 wirelessCapable 判断能否把 Windows 电池读数补给该
    // 设备，而 wirelessCapable 只由 FinalizeDeviceCapabilities 计算。必须在补电池
    // 之前先算一次派生字段，否则 2.4G 设备会被当成“仅蓝牙”，系统电池还会另起条目；
    // 合并之后再算一次，作为发布前的最终状态。
    proto::FinalizeDeviceCapabilities(devices);
    proto::AddSystemBatteries(devices);

    Snapshot old = GetSnapshot();
    devices = MergeDeviceSnapshots(old.devices, std::move(devices));
    // 合并可能从旧快照补回电量或改变 conn；能力字段必须在合并之后重新计算，
    // 否则会出现 batteryValid=true 但 batteryApplicable=false 的矛盾状态。
    proto::FinalizeDeviceCapabilities(devices);
    // 用合并后的快照对比上一轮，记录接入/断开/电量变化；离线设备此时已被过滤，
    // 所以它的消失同时就是一条 DISCONNECT。
    LogSnapshotChanges(old.devices, devices, lastSampleEpoch_);

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
            case Proto::Valkyrie:
            case Proto::MoreBetter:
            case Proto::GenericHid:
            case Proto::BluetoothAudio:
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
        // 与下面的“插线预览”成对出现；明确标注 2.4G 预览，避免看起来像重复设备。
        d.name = L"Zaopin Z4D（2.4G 预览）";
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
        // 无线型号插 USB 线：conn 走有线，但仍应显示“USB 有线 · 无线模式”并保留电量。
        // 仅用于 --demo 预览验收场景 2，真实扫描路径由 MergeScore/AddWired 决定。
        Device d;
        d.id = L"demo-zaopin-wired";
        d.name = L"Zaopin Z4D（插线预览）";
        d.product = L"Zaopin Z4D";
        d.manufacturer = L"JinLeMa / Zaopin";
        d.vid = 0x3554;
        d.pid = 0xF526;
        d.identity = L"zaopin:z4d";
        d.proto = Proto::Compx;
        d.conn = Conn::Wired;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = 62;
        d.batteryValid = true;
        d.charging = true;
        d.chargingKnown = true;
        d.millivolts = 3921;
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
        d.note = L"演示数据 · 插线仍按无线设备管理电量";
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
    {
        // 有线键盘：无电池，用于预览“不显示电量卡/曲线”的布局。
        Device d;
        d.id = L"demo-wired-kbd";
        d.name = L"DURGOD Taurus K310";
        d.product = d.name;
        d.manufacturer = L"Hoksi Technology";
        d.vid = 0x2F68;
        d.pid = 0x0042;
        d.proto = Proto::GenericHid;
        d.conn = Conn::Wired;
        d.kind = Kind::Keyboard;
        d.online = true;
        d.battery = -1;
        d.batteryValid = false;
        d.chargingKnown = false;
        d.note = L"USB 有线设备 · 不提供电量（无电池）";
        v.push_back(d);
    }
    {
        // 蓝牙鼠标：通用 HID，无厂商电量通道；用于预览蓝牙标签与信息卡。
        Device d;
        d.id = L"demo-bt-mouse";
        d.name = L"MX Anywhere 3S";
        d.product = d.name;
        d.manufacturer = L"Logitech";
        d.vid = 0x046D;
        d.pid = 0xB034;
        d.proto = Proto::GenericHid;
        d.conn = Conn::Bluetooth;
        d.kind = Kind::Mouse;
        d.online = true;
        d.battery = -1;
        d.batteryValid = false;
        d.chargingKnown = false;
        d.note = L"蓝牙 HID 设备 · 电量由系统电池服务提供时才显示";
        v.push_back(d);
    }
    // 演示设备也走同一套能力判定，否则 batteryApplicable 默认 false 会让电量卡全部消失。
    proto::FinalizeDeviceCapabilities(v);
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

