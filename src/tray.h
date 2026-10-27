#pragma once

#include "common.h"

#include <shellapi.h>

HICON CreateBatteryIcon(int percent, bool charging, bool stale, int sizePx);

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
