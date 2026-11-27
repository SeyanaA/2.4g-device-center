#pragma once

#include "common.h"

#include <shellapi.h>

HICON CreateBatteryIcon(int percent, bool charging, bool stale, int sizePx);

// 有线/蓝牙无电量设备的托盘徽章：蓝底白字 "U"（USB）或 "B"（蓝牙）。
HICON CreateTransportIcon(bool bluetooth, bool stale, int sizePx);

class TrayIcon {
public:
    bool Add(HWND hwnd, UINT callbackMessage, HICON icon, const std::wstring& tip);
    bool Update(HICON icon, const std::wstring& tip);
    bool Balloon(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO);
    void Remove();
    bool Added() const { return added_; }

private:
    HWND hwnd_ = nullptr;
    UINT id_ = 1;
    bool added_ = false;
};
