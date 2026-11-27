#pragma once

#include "audio.h"
#include "battery_notify.h"
#include "event_log.h"
#include "manager.h"
#include "tray.h"
#include "ui_anim.h"
#include "ui_test_view.h"

constexpr UINT WM_APP_SHOW_MAIN = WM_APP + 36;

class MainWindow {
public:
    enum class View { Device = 0, Log = 1, Test = 2 };

    bool Create(HINSTANCE instance, bool demo, bool startHidden);
    HWND hwnd() const { return hwnd_; }
    static const wchar_t* ClassName();
    // 主消息循环预处理：Tab 焦点链、Ctrl+Tab/Ctrl+1..3、F5 等全局快捷键。
    // 返回 true 表示消息已被消费，调用方不要再 Translate/Dispatch。
    bool PreTranslateMessage(const MSG& msg);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void RecreateFonts();
    void Layout();
    void Paint(HDC dc);
    // B05：WM_PAINT 只给 ps.rcPaint。整窗无效才整帧，其余只重画脏区。
    void Paint(HDC dc, const RECT& dirty);
    void PaintFullFrame(HDC dc);
    // 返回 false 表示无法增量（背景缓存不可用），调用方退回整帧。
    bool PaintDirty(HDC dc, const RECT& dirty);
    // 内容区（内容头 + 当前视图）的唯一绘制入口：整帧、增量与入场离屏共用。
    void PaintContent(HDC dc);
    void PaintNav(HDC dc);
    void PaintListPanel(HDC dc);
    void PaintContentHeader(HDC dc);
    void PaintDeviceMeta(HDC dc, const Device& d);
    void PaintConnCard(HDC dc, const Device& d);
    void PaintInfoCard(HDC dc, const Device& d);
    void PaintDeviceCard(HDC dc, const Device& d);
    void PaintBatteryCard(HDC dc, const Device& d);
    void PaintDpiCard(HDC dc, const Device& d);
    void PaintPerfCard(HDC dc, const Device& d);
    // 音量与音效卡：占用“设备信息”的格子，仅对耳机/匹配到音频端点的设备显示。
    void PaintAudioCard(HDC dc, const Device& d);
    bool AudioCardAvailable() const;
    void RefreshAudioState();
    void RebuildSysEndpointCombo();
    void ApplyVolumeDrag(int x);
    // 音量卡的数字输入：0..100，失焦/Enter 归一化；与拖动共用同一份状态。
    void OnVolumeEditChanged(bool device);
    void NormalizeVolumeEdit(bool device);
    void SyncVolumeEdits();
    bool HeadsetDeviceTarget() const;
    bool HeadsetSystemTarget() const;
    void ResetHeadsetLoudConsent();
    void ShowHeadsetLoudWarning(bool deviceRow, int attempted);
    void ApplyHeadsetLoudPending();
    void PaintFooter(HDC dc);
    void PaintEmptyState(HDC dc);
    void PaintLogView(HDC dc);
    void PaintMiniChart(HDC dc);

    void SetLogView(bool on);
    void SetTestView(bool on);
    void SetView(View v);
    // 设备页音频卡展开为内容区内的音频面板（不新建窗口）。
    void ExpandAudio();
    void CollapseAudio();
    void StartAudioSlide(bool expanding);
    void InvalidateAudioSlide();
    void FinishAudioSlide(bool expanded);
    bool CaptureAudioPanelSnapshot();
    void ReleaseAudioSnapshot();
    void PaintAudioSnapshot(HDC dc);
    void OnNavClick(int x, int y);
    // 键盘导航：收集当前可见的 WS_TABSTOP 原生控件，Tab/Shift+Tab 在其中循环，
    // 并与自绘导航栏（主窗口自身）互相回绕。
    std::vector<HWND> BuildFocusChain() const;
    bool HandleTabKey(bool backward);
    void SetNavFocus(int index, bool activate);
    void InvalidateNavFocus();
    void ReloadLogs();
    void ApplyLogSelection();
    void FillLogList();
    void ExportLogs();
    void UpdateMiniSeries();
    void UpdateLogScrollbar();
    int LogMaxScroll() const;
    RECT LogThumbRect() const;

    void OnSnapshot();
    void OnApplyResult();
    void UpdateControlsFromDevice();
    void OnSelectIndex(int index);
    void OnSliderChanged(int value);
    void OnDpiEditChanged();
    void OnStageClicked(int index);
    void ApplySettings();
    void RefreshTray();
    void NotifyBatteryEvents();
    void ShowFromTray();
    void HideToTray();
    void ShowTrayMenu(POINT pt);
    int Scale(int value) const;
    const Device* Selected() const;
    int PrimaryDeviceIndex() const;
    void ShowToast(const std::wstring& text, bool ok, int seconds = 6);
    void RebuildListIfNeeded();
    void StartAnimTimer();
    void StopAnimTimer();
    void OnAnimTimer();
    void AnimateBattery(const Device& d);
    // 只重绘电量卡（充电脉冲 / 电量补间），避免整窗 60–120 fps 重绘。
    void RepaintBatteryCard();

    // 音频端点状态（每次设备刷新时重读，不做后台轮询）。
    std::vector<audio::Endpoint> audioEndpoints_;
    std::wstring audioDevId_;   // 当前设备的播放端点（可能是系统默认输出）
    std::wstring audioSysId_;   // 系统输出目标端点（默认或用户固定）
    std::wstring audioSysPinnedId_;  // 空 = 跟随系统默认；否则固定到该端点
    std::vector<std::wstring> audioSysChoices_;  // 下拉项 0 = 跟随默认，其余为端点 id
    std::wstring audioSysComboSig_;  // 端点列表签名，未变化时不重建下拉
    // 耳机听力保护：推荐 ≤60%，超过需勾选同意。
    bool headsetLoudOk_ = false;        // 已勾选同意
    bool headsetLoudWarn_ = false;      // 显示警告与同意框
    int headsetLoudPending_ = -1;       // 被拦截的请求值，勾选后应用
    bool headsetLoudPendingSys_ = false;  // true = 系统输出行
    int audioDevVol_ = -1;      // -1 = 不可用
    int audioSysVol_ = -1;
    bool audioDevMute_ = false;
    bool audioSysMute_ = false;
    bool audioMatched_ = false;  // 设备名精确/片段匹配到了端点
    int volDrag_ = 0;            // 0 无，1 设备音量，2 系统输出
    RECT rcVolDev_{};
    RECT rcVolSys_{};
    RECT rcMuteDev_{};
    RECT rcMuteSys_{};
    RECT rcSoundLink_{};
    RECT rcAudioCenter_{};

    // 页面切换的内容入场（淡入 + 上浮）。只在 SetView 中触发。
    void StartContentIntro();
    void FinishContentIntro();
    void HideContentChildren();

    // 持久化绘制缓冲：动画最高 120 Hz，不能每帧创建/销毁兼容位图。
    void EnsurePaintBuffer(HDC dc, int w, int h);
    void EnsureIntroBuffer(HDC dc, int w, int h);
    void ReleaseDrawBuffers();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT dpi_ = 96;
    bool demo_ = false;
    bool audioExpanded_ = false;
    // 音频面板入场/退场：真实面板先截成快照并隐藏，主窗口只移动/淡入淡出快照，
    // 避免移动原生子窗口产生的重绘鬼影。
    Animator::AnimId audioSlideAnim_ = 0;
    float audioSlideT_ = 0.0f;      // 0=展开目标位置，1=主窗口右侧外
    float audioSlideAlpha_ = 1.0f;  // = 1 - t
    bool audioSliding_ = false;
    RECT audioPanelRect_{};         // 展开态目标矩形
    HDC audioSnapMem_ = nullptr;
    HBITMAP audioSnapBmp_ = nullptr;
    HGDIOBJ audioSnapOld_ = nullptr;
    int audioSnapW_ = 0;
    int audioSnapH_ = 0;
    bool audioSnapValid_ = false;
    bool startHidden_ = false;
    bool exiting_ = false;
    bool closeHintShown_ = false;
    bool updatingControls_ = false;
    bool rebuildingList_ = false;
    bool dirty_ = false;

    HWND list_ = nullptr;
    HWND btnRefresh_ = nullptr;
    HWND logCombo_ = nullptr;
    HWND logDaysCombo_ = nullptr;
    HWND logList_ = nullptr;
    HWND logFolder_ = nullptr;
    HWND logExport_ = nullptr;
    View view_ = View::Device;
    int dragTarget_ = 0;  // 2=日志页横向滚动条
    int dragGrab_ = 0;
    HWND btnApply_ = nullptr;
    HWND slider_ = nullptr;
    HWND editDpi_ = nullptr;
    HWND editVolDev_ = nullptr;
    HWND editVolSys_ = nullptr;
    HWND comboVolSys_ = nullptr;
    HWND chkLoudOk_ = nullptr;
    HWND comboPoll_ = nullptr;
    HWND comboDeb_ = nullptr;
    HWND chkAngle_ = nullptr;
    std::vector<HWND> stageButtons_;

    HFONT fontTitle_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontBodyBold_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontRing_ = nullptr;
    HFONT fontRingSmall_ = nullptr;
    HFONT fontBadge_ = nullptr;

    HBRUSH brushCard_ = nullptr;
    // 原生控件（复选框）在玻璃卡上的近似底色：玻璃是半透明的，控件无法透明，
    // 用一个接近卡片实际呈现色的纯色块，避免出现刺眼的纯白方块。
    HBRUSH brushField_ = nullptr;
    HBRUSH brushBg_ = nullptr;

    DeviceManager manager_;
    TrayIcon tray_;
    MouseTestView testView_;
    Snapshot snapshot_;
    std::vector<std::wstring> listedIds_;

    std::wstring selectedId_;
    int selectedIndex_ = -1;
    bool lastBatteryApplicable_ = false;  // 设备页是否显示电量卡/曲线，用于切换时重排
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
    // toast 出现动画：0→1，文字上滑 + 颜色渐入。
    float toastShown_ = 1.0f;
    Animator::AnimId toastAnim_ = 0;

    HICON trayIcon_ = nullptr;
    HICON appIcon_ = nullptr;
    std::wstring lastTrayKey_;
    BatteryNotifier batteryNotifier_;
    bool notificationsEnabled_ = true;
    bool notifyDisabledHintShown_ = false;

    bool animTimerActive_ = false;
    UINT animTimerIntervalMs_ = 0;   // 当前 WM_TIMER(2) 间隔，按显示器刷新率换算
    bool animTimerPeriodSet_ = false;  // 已 timeBeginPeriod(1)（引用计数需成对）
    bool ringAnimating_ = false;
    double ringShown_ = -1.0;
    double ringFrom_ = 0.0;
    double ringTo_ = -1.0;
    double ringStart_ = 0.0;  // 与 NowMs() 同一时间基准（毫秒）
    std::wstring ringDeviceId_;

    // 持久化主绘制缓冲（客户区大小，仅在尺寸变化时重建）。
    HDC paintMem_ = nullptr;
    HBITMAP paintBmp_ = nullptr;
    HBITMAP paintOld_ = nullptr;
    int paintW_ = 0;
    int paintH_ = 0;
    // 缓冲刚重建：位图里没有任何旧像素，这一帧必须整帧画。
    bool paintFresh_ = false;
    // 增量重绘统计：退出时写一行日志，核对 ps.rcPaint 是否真的生效。
    unsigned long long paintFull_ = 0;
    unsigned long long paintDirty_ = 0;
    unsigned long long paintDirtyPx_ = 0;
    unsigned long long paintDirtyMinPx_ = ~0ULL;
    unsigned long long paintDirtyMaxPx_ = 0;
    // 页面切换淡入的离屏缓冲：先把真实背景 + 内容合成，再整体 alpha 混合。
    HDC introMem_ = nullptr;
    HBITMAP introBmp_ = nullptr;
    HBITMAP introOld_ = nullptr;
    int introW_ = 0;
    int introH_ = 0;
    float introT_ = 1.0f;  // 0→1；Animator 已按 EmphasizedDecelerate 缓动
    bool introActive_ = false;
    Animator::AnimId introAnim_ = 0;

    RECT rcRail_{};
    RECT rcListPane_{};
    RECT rcContent_{};
    RECT rcContentHeader_{};
    RECT rcMeta_{};
    RECT rcConn_{};
    RECT rcInfo_{};
    RECT rcNavItems_[3]{};
    int hoverNav_ = -1;
    // 键盘焦点停在导航栏时指向 0..2；焦点在原生控件时仅保留上次位置。
    int focusNav_ = 0;
    bool navMouseTracking_ = false;  // 已向系统注册 TME_LEAVE
    float navIndicatorY_ = 0.0f;
    Animator::AnimId navAnim_ = 0;
    // 果冻：胶囊移动时的轻微拉伸（像素），由 SetView 的包络轨道驱动。
    float navStretch_ = 0.0f;
    // 导航悬停渐变（0..1，逐项），避免 hover 立刻变色。
    float navHover_[3] = {0.0f, 0.0f, 0.0f};
    Animator::AnimId navHoverAnim_[3] = {0, 0, 0};
    void SetNavHover(int nav);
    Animator anim_;
    RECT rcDevice_{};
    RECT rcBattery_{};
    RECT rcDpi_{};
    RECT rcPerf_{};
    RECT rcFooter_{};

    // 日志视图（内嵌在主界面，不再单独开窗口）。
    std::vector<EventRecord> logRecords_;
    std::vector<EventDevice> logDevices_;
    std::wstring logIdentity_;  // 空 = 全部设备
    bool logFilterReady_ = false;  // 本次进入是否已完成设备匹配（之后保留用户选择）
    int logRowCount_ = 0;
    int logDays_ = 7;         // 日志视图时间尺度：1..7 天
    int logScrollX_ = 0;      // 曲线横向滚动偏移（像素）
    ULONGLONG logReloadAt_ = 0;
    std::vector<BatterySample> miniSeries_;  // 设备页底部迷你曲线
    RECT rcMiniChart_{};
    RECT rcLogChart_{};
    int logListLabelY_ = 0;
};
