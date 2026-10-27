#include "common.h"
#include "hid.h"
#include "manager.h"
#include "protocols.h"
#include "ui.h"

#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>

namespace {

std::wstring g_consoleBuffer;

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
                            d.name.c_str(), KindName(d.kind), ConnName(d.conn),
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
    auto ifaces = EnumerateHidInterfaces();
    ConsoleWrite(Fmt(L"HID 接口数量: %d", (int)ifaces.size()));
    for (const auto& h : ifaces) {
        ConsoleWrite(Fmt(L"VID_%04X PID_%04X page=0x%04X usage=0x%04X in=%u out=%u feat=%u "
                         L"mi=%d col=%d rw=%d product=%s",
                         h.vid, h.pid, h.usagePage, h.usage, h.inLen, h.outLen, h.featLen,
                         h.interfaceNumber, h.collection, h.canOpenRW ? 1 : 0,
                         h.product.c_str()));
        ConsoleWrite(L"  " + h.path);
    }
    auto bats = EnumerateSystemBatteries();
    ConsoleWrite(Fmt(L"Windows HID 电池: %d", (int)bats.size()));
    for (const auto& b : bats) {
        ConsoleWrite(Fmt(L"  %s  %d%%  %d mV  charging=%d", b.name.c_str(), b.percent,
                         b.millivolts, b.charging ? 1 : 0));
    }
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
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    MainWindow window;
    if (!window.Create(instance, demo, startHidden)) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
