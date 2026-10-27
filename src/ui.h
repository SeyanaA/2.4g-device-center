#pragma once

#include "manager.h"
#include "tray.h"

constexpr UINT WM_APP_SHOW_MAIN = WM_APP + 36;

class MainWindow {
public:
    bool Create(HINSTANCE instance, bool demo, bool startHidden);
    HWND hwnd() const { return hwnd_; }
    static const wchar_t* ClassName();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void RecreateFonts();
    void Layout();
    void Paint(HDC dc);
    void PaintHeader(HDC dc);
    void PaintListCard(HDC dc);
    void PaintDeviceCard(HDC dc, const Device& d);
    void PaintBatteryCard(HDC dc, const Device& d);
    void PaintDpiCard(HDC dc, const Device& d);
    void PaintPerfCard(HDC dc, const Device& d);
    void PaintFooter(HDC dc);
    void PaintEmptyState(HDC dc);

    void OnSnapshot();
    void OnApplyResult();
    void UpdateControlsFromDevice();
    void OnSelectIndex(int index);
    void OnSliderChanged(int value);
    void OnDpiEditChanged();
    void OnStageClicked(int index);
    void ApplySettings();
    void RefreshTray();
    void ShowFromTray();
    void HideToTray();
    void ShowTrayMenu(POINT pt);
    void UpdateStartupCheckbox();
    int Scale(int value) const;
    const Device* Selected() const;
    int PrimaryDeviceIndex() const;
    void ShowToast(const std::wstring& text, bool ok, int seconds = 6);
    void RebuildListIfNeeded();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = 96;
    bool demo_ = false;
    bool startHidden_ = false;
    bool exiting_ = false;
    bool closeHintShown_ = false;
    bool updatingControls_ = false;
    bool dirty_ = false;

    HWND list_ = nullptr;
    HWND btnRefresh_ = nullptr;
    HWND btnApply_ = nullptr;
    HWND slider_ = nullptr;
    HWND editDpi_ = nullptr;
    HWND comboPoll_ = nullptr;
    HWND comboDeb_ = nullptr;
    HWND chkAngle_ = nullptr;
    HWND chkStartup_ = nullptr;
    std::vector<HWND> stageButtons_;

    HFONT fontTitle_ = nullptr;
    HFONT fontSub_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontBodyBold_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontBig_ = nullptr;
    HFONT fontRing_ = nullptr;
    HFONT fontBadge_ = nullptr;

    HBRUSH brushCard_ = nullptr;
    HBRUSH brushBg_ = nullptr;

    DeviceManager manager_;
    TrayIcon tray_;
    Snapshot snapshot_;
    std::vector<std::wstring> listedIds_;

    std::wstring selectedId_;
    int selectedIndex_ = -1;
    int dpiValue_ = 0;
    int stageIndex_ = -1;
    bool dpiDirty_ = false;
    bool pollingDirty_ = false;
    bool debounceDirty_ = false;
    bool angleDirty_ = false;
    bool stageDirty_ = false;

    uint64_t lastApplySerial_ = 0;
    std::wstring toast_;
    bool toastOk_ = true;
    ULONGLONG toastUntil_ = 0;

    HICON trayIcon_ = nullptr;
    HICON appIcon_ = nullptr;
    std::wstring lastTrayKey_;
    bool lowNotified_ = false;

    RECT rcHeader_{};
    RECT rcList_{};
    RECT rcDevice_{};
    RECT rcBattery_{};
    RECT rcDpi_{};
    RECT rcPerf_{};
    RECT rcFooter_{};
};
