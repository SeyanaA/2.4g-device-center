#include "common.h"
#include "hid.h"
#include "manager.h"
#include "protocols.h"
#include "ui.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <set>

namespace {

std::wstring g_consoleBuffer;

const wchar_t* BusLabel(HidBus bus) {
    switch (bus) {
        case HidBus::Usb: return L"USB";
        case HidBus::Bluetooth: return L"BT";
        default: return L"?";
    }
}

bool IsMouseOrKeyboardUsage(const HidInterface& h) {
    return h.usagePage == 0x0001 && (h.usage == 0x0002 || h.usage == 0x0006);
}

HANDLE OpenConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    return CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

void ConsoleWrite(const std::wstring& text) {
    static HANDLE h = OpenConsole();
    g_consoleBuffer += text;
    g_consoleBuffer += L"\r\n";
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteConsoleW(h, text.c_str(), (DWORD)text.size(), &written, nullptr);
        WriteConsoleW(h, L"\r\n", 2, &written, nullptr);
    }
}

void FlushConsoleFile() {
    std::wstring path = AppDataDir() + L"\\last-console.txt";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::string utf8 = WideToUtf8(g_consoleBuffer);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
}

std::wstring DeviceLine(const Device& d, int index) {
    std::wstring battery;
    if (d.batteryValid && d.battery >= 0) {
        battery = std::to_wstring(d.battery) + L"%";
        if (d.charging) battery += L" (充电)";
        if (d.stale) battery += L" [上次读数]";
    } else {
        battery = L"--";
    }
    std::wstring dpi;
    if (d.dpi.valid) {
        if (d.dpi.currentKnown) {
            dpi = std::to_wstring(d.dpi.x);
            if (d.dpi.y && d.dpi.y != d.dpi.x) dpi += L"x" + std::to_wstring(d.dpi.y);
        } else {
            dpi = L"档位表";
            for (size_t i = 0; i < d.dpi.stages.size(); ++i) {
                dpi += i ? L"/" : L" ";
                dpi += std::to_wstring(d.dpi.stages[i]);
            }
        }
    } else {
        dpi = L"--";
    }
    std::wstring poll = d.polling.valid ? std::to_wstring(d.polling.currentHz) + L" Hz" : L"--";
    std::wstring line = Fmt(L"[%d] %s | %s | %s | %s | 电量 %s | DPI %s | 轮询 %s", index,
                            d.name.c_str(), KindName(d.kind), DeviceConnLabel(d).c_str(),
                            FriendlyProtocolName(d).c_str(), battery.c_str(), dpi.c_str(),
                            poll.c_str());
    if (!d.note.empty()) line += L" | " + d.note;
    return line;
}

int RunOnce() {
    ConsoleWrite(L"2.4G 设备中心 · 设备枚举 (--once)");
    DeviceManager mgr;
    mgr.Start(nullptr, false);
    Snapshot snap;
    for (int i = 0; i < 20; ++i) {
        Sleep(400);
        snap = mgr.GetSnapshot();
        if (!snap.updated.empty()) break;
    }
    mgr.Stop();
    if (snap.devices.empty()) {
        ConsoleWrite(L"未发现兼容设备。请确认接收器已插入，并移动鼠标唤醒。");
    } else {
        for (int i = 0; i < (int)snap.devices.size(); ++i) {
            ConsoleWrite(DeviceLine(snap.devices[i], i + 1));
        }
    }
    ConsoleWrite(L"状态: " + snap.status);
    ConsoleWrite(L"日志: " + AppLogPath());
    FlushConsoleFile();
    return 0;
}

int RunDiag() {
    ConsoleWrite(L"2.4G 设备中心 · HID 诊断 (--diag)");
    ConsoleWrite(Fmt(L"Architecture: x86-64 / PE32+ (%u-bit process)", (unsigned)(sizeof(void*) * 8)));
    // UI 分辨率参数：字体全部按 pt + DPI 创建（ClearType），图标是矢量 path。
    HDC screenDc = GetDC(nullptr);
    const int sysDpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) ReleaseDC(nullptr, screenDc);
    auto px = [sysDpi](int pt) { return MulDiv(pt, sysDpi > 0 ? sysDpi : 96, 72); };
    ConsoleWrite(Fmt(L"UI: 系统 %d DPI (%d%%) · Per-Monitor V2 · 窗口 1000x780 / 最小 1000x740 逻辑像素",
                     sysDpi, MulDiv(sysDpi, 100, 96)));
    ConsoleWrite(Fmt(L"字体: 标题 16pt=%dpx · 正文 10pt=%dpx · 辅助 9pt=%dpx · 电量 26pt=%dpx "
                     L"(Segoe UI)",
                     px(16), px(10), px(9), px(26)));
    ConsoleWrite(L"图标/圆角: SVG path + GDI+ 抗锯齿 · 动画: QPC + 刷新率自适应定时器");
    // 播放端点：设备页“音量与音效”卡就是按这里的名字匹配的。
    {
        const auto eps = audio::ListRenderEndpoints();
        const auto caps = audio::ListCaptureEndpoints();
        std::wstring line = Fmt(L"音频: %d 个播放端点 · %d 个采集端点", (int)eps.size(),
                                (int)caps.size());
        for (const auto& e : eps) {
            if (!e.isDefault) continue;
            line += Fmt(L" · 默认「%s」%d%%%s", e.name.c_str(), e.volume,
                        e.mute ? L"（静音）" : L"");
            break;
        }
        ConsoleWrite(line);
        const auto sessions = audio::ListAppSessions();
        std::wstring apps;
        for (const auto& s : sessions) {
            if (!apps.empty()) apps += L" · ";
            apps += Fmt(L"%s %d%%%s", s.name.c_str(), s.volume, s.mute ? L"（静音）" : L"");
        }
        ConsoleWrite(Fmt(L"音频会话: %d 个应用%s%s", (int)sessions.size(),
                         apps.empty() ? L"" : L" · ", apps.c_str()));
    }
    ConsoleWrite(Fmt(L"Windows 通知: %s", SystemNotificationsEnabled()
                                             ? L"已开启"
                                             : L"已关闭（ToastEnabled=0）"));
    auto ifaces = EnumerateHidInterfaces();
    ConsoleWrite(Fmt(L"HID 接口数量: %d", (int)ifaces.size()));
    int usbIfaces = 0;
    int btIfaces = 0;
    int unknownIfaces = 0;
    for (const auto& h : ifaces) {
        const std::wstring container = h.containerId.empty() ? L"-" : h.containerId.substr(0, 8);
        ConsoleWrite(Fmt(L"[%s]VID_%04X PID_%04X page=0x%04X usage=0x%04X in=%u out=%u feat=%u "
                         L"mi=%d col=%d rw=%d bus=%s container=%s product=%s",
                         BusLabel(h.bus), h.vid, h.pid, h.usagePage, h.usage, h.inLen, h.outLen,
                         h.featLen, h.interfaceNumber, h.collection, h.canOpenRW ? 1 : 0,
                         BusLabel(h.bus), container.c_str(), h.product.c_str()));
        ConsoleWrite(L"  " + h.path);

        if (h.bus == HidBus::Usb) ++usbIfaces;
        if (h.bus == HidBus::Bluetooth) ++btIfaces;
        if (h.bus == HidBus::Unknown) ++unknownIfaces;
    }
    // 与正式扫描（DeviceManager::ScanOnce）使用完全相同的读取管线：已知协议先
    // 输出设备，AddWiredHidDevices 才会按 VID/PID/container/basePath 跳过重复项。
    // 只统计 AddWired 真正新增的条目，诊断数字才和界面列表一致。
    std::vector<Device> scanDevices;
    proto::ReadLogitech(ifaces, scanDevices);
    proto::ReadAsus(ifaces, scanDevices);
    proto::ReadRazer(ifaces, scanDevices);
    proto::ReadRapoo(ifaces, scanDevices);
    proto::ReadGeneric(ifaces, scanDevices);
    proto::ReadValkyrie(ifaces, scanDevices);
    proto::ReadMoreBetter(ifaces, scanDevices);
    const size_t beforeWired = scanDevices.size();
    proto::AddWiredHidDevices(ifaces, scanDevices);
    const size_t wiredAdded = scanDevices.size() - beforeWired;
    auto bats = EnumerateSystemBatteries();
    ConsoleWrite(Fmt(L"Windows HID 电池: %d", (int)bats.size()));
    for (const auto& b : bats) {
        ConsoleWrite(Fmt(L"  %s  %d%%  %d mV  charging=%d", b.name.c_str(), b.percent,
                         b.millivolts, b.charging ? 1 : 0));
    }
    // 无 HID 的经典蓝牙耳机只能从 Bluetooth API 枚举；单独列出便于排查
    // “设备已连接但列表不显示”的问题。
    auto btAudio = EnumerateBluetoothAudioDevices();
    ConsoleWrite(Fmt(L"蓝牙音频设备: %d", (int)btAudio.size()));
    for (const auto& b : btAudio) {
        ConsoleWrite(Fmt(L"  %s  %s  cod=0x%06lX headset=%d", b.address.c_str(),
                         b.name.c_str(), (unsigned long)b.classOfDevice,
                         b.headset ? 1 : 0));
    }
    // Unknown 接口（如 HID VHF）必须单列，否则 USB+蓝牙 与总数对不上。
    // “新增设备”是设备条目数，不是接口数，单位必须写清楚。
    ConsoleWrite(Fmt(L"USB 接口: %d · 蓝牙接口: %d · 未知接口: %d（接口合计 %d） · 新增设备: %d",
                     usbIfaces, btIfaces, unknownIfaces, (int)ifaces.size(),
                     (int)wiredAdded));
    ConsoleWrite(L"提示: 把这份输出附在问题反馈里，可快速定位不支持的公版协议。");
    FlushConsoleFile();
    return 0;
}

void ShowHelp() {
    const wchar_t* text =
        L"2.4G 设备中心 v1.0\n\n"
        L"用法:\n"
        L"  WirelessCenter.exe            启动主界面\n"
        L"  WirelessCenter.exe --demo     使用演示设备预览界面\n"
        L"  WirelessCenter.exe --tray     启动后只显示托盘图标\n"
        L"  WirelessCenter.exe --once     在控制台枚举设备并退出\n"
        L"  WirelessCenter.exe --diag     输出 HID 接口诊断信息\n\n"
        L"支持协议:\n"
        L"  Logitech HID++ 2.0 / ASUS ROG / Razer / Rapoo / Compx /\n"
        L"  Nordic 54 / Aurora-mouse.xyz / Cherry / Lofree / Windows HID 电池\n\n"
        L"安全说明:\n"
        L"  电量读取默认只发只读查询；只有点击“应用设置”才会写入设备。\n"
        L"  不包含宏、灯效和固件写入功能。";
    ConsoleWrite(text);
    MessageBoxW(nullptr, text, L"2.4G 设备中心", MB_OK | MB_ICONINFORMATION);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool demo = false;
    bool startHidden = false;
    bool once = false;
    bool diag = false;
    bool help = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = Lower(argv[i]);
        if (a == L"--demo") demo = true;
        else if (a == L"--tray" || a == L"--hidden") startHidden = true;
        else if (a == L"--once") once = true;
        else if (a == L"--diag") diag = true;
        else if (a == L"--help" || a == L"-h" || a == L"/?") help = true;
    }
    if (argv) LocalFree(argv);

    if (help) {
        ShowHelp();
        FlushConsoleFile();
        return 0;
    }
    if (once) return RunOnce();
    if (diag) return RunDiag();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\2.4GCenter_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(MainWindow::ClassName(), nullptr);
        for (int i = 0; i < 20 && !existing; ++i) {
            Sleep(100);
            existing = FindWindowW(MainWindow::ClassName(), nullptr);
        }
        if (existing) PostMessageW(existing, WM_APP_SHOW_MAIN, 0, 0);
        CloseHandle(mutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    MainWindow window;
    if (!window.Create(instance, demo, startHidden)) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Tab/Shift+Tab、Ctrl+Tab、Ctrl+1..3、F5 等全局键在分发前统一处理，
        // 否则焦点在原生控件上时 Tab 会被控件吞掉，自绘导航栏也无法到达。
        if (window.PreTranslateMessage(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
