#include "audio_center.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "audio.h"
#include "ui_theme.h"

using namespace uitheme;

namespace {

constexpr wchar_t kClassName[] = L"WirelessCenterAudioCenter";
constexpr int kListId = 5001;
constexpr int kOutComboId = 5002;
constexpr int kInComboId = 5003;
constexpr int kTimerRefresh = 1;
constexpr int kTimerMeter = 2;

class AudioCenterWindow {
public:
    bool ShowEmbedded(HWND parent, const RECT& area, const audiocenter::Context& ctx);
    void HideEmbedded();
    void SetBounds(const RECT& area);
    void SetWindowDpi(UINT dpi);
    void SetSuspended(bool suspended);
    void SetVisible(bool visible);
    HWND window() const { return hwnd_; }
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void CreateControls();
    void RecreateFonts();
    void Layout();
    void Paint(HDC dc);
    void PaintBuffer(HDC dc);
    void EnsureBuffer(HDC dc, int w, int h);
    void ReleaseBuffer();
    void Refresh(bool force);
    void RebuildSessionList();
    void RefreshSelectedSession();
    void PopulateAppCombos(bool force);
    void ApplyAppEndpoint(bool capture);
    void ApplyDrag(int x);
    // B03：拖动应用音量时直接写按下时缓存的会话接口，不再每条鼠标消息枚举会话。
    bool WriteAppVolume(int pct);
    void EndAppDrag();
    void ToggleMicTest();
    void StopMic();
    bool MicStarting() const;
    void SettleMicState();
    int Scale(int v) const { return MulDiv(v, (int)dpi_, 96); }
    const audio::Endpoint* FindRender() const;
    const audio::Endpoint* FindCapture() const;
    void DrawCard(HDC dc, const RECT& rc, const std::wstring& title);
    void DrawVolumeRow(HDC dc, const RECT& bar, const RECT& pill, int vol, bool mute,
                       const wchar_t* label, const RECT& area);
    void DrawMeter(HDC dc, const RECT& rc, float level);
    void DrawButton(HDC dc, const RECT& rc, const std::wstring& text, bool primary);
    std::wstring AppStatusText() const;
    std::wstring MicStatusText() const;

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    HWND list_ = nullptr;
    HWND outCombo_ = nullptr;
    HWND inCombo_ = nullptr;
    UINT dpi_ = 96;

    audiocenter::Context ctx_;
    std::vector<audio::Endpoint> renders_;
    std::vector<audio::Endpoint> captures_;
    std::string sessionKey_;  // pid|name|count，避免每 2 s 重建列表
    std::vector<audio::AppSession> sessions_;

    std::wstring renderId_, renderName_;
    std::wstring micId_, micName_;
    int micIndex_ = -1;
    int renderVol_ = -1;
    int micVol_ = -1;
    int appVol_ = -1;
    bool renderMute_ = false;
    bool micMute_ = false;
    bool appMute_ = false;
    int selectedSession_ = -1;
    unsigned long selectedPid_ = 0;
    std::wstring appStatus_;

    audio::MicMonitor mic_;
    bool micActive_ = false;
    ULONGLONG micStartTick_ = 0;
    float shownLevel_ = 0.0f;

    // 应用音量拖动缓存：按下时 Open(pid) 一次，拖动期间直接写，结束时 Close()。
    audio::AppVolumeWriter appWriter_;
    bool appWriteReopened_ = false;  // 本次拖动是否已经重开过一次缓存

    RECT rcOutputCard_{};
    RECT rcMicCard_{};
    RECT rcAppsCard_{};
    RECT rcList_{};
    RECT rcBack_{};
    RECT rcRefresh_{};
    RECT rcOpenSettings_{};
    RECT rcRenderVol_{};
    RECT rcRenderMute_{};
    RECT rcMicVol_{};
    RECT rcMicMute_{};
    RECT rcMicMeter_{};
    RECT rcMicBtn_{};
    RECT rcNoMicLink_{};
    RECT rcAppVol_{};
    RECT rcAppMute_{};
    RECT rcOutLabel_{};
    RECT rcOutCombo_{};
    RECT rcInLabel_{};
    RECT rcInCombo_{};
    RECT rcAppSettings_{};

    std::string renderKey_;
    std::string captureKey_;
    unsigned long comboPid_ = 0;

    int drag_ = 0;  // 1=系统输出，2=应用，3=麦克风
    unsigned long dragPid_ = 0;  // 应用音量拖动锁定的 PID，避免刷新后写到别的应用

    HFONT fontTitle_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HBRUSH listBrush_ = nullptr;

    HDC mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ memOld_ = nullptr;
    int memW_ = 0;
    int memH_ = 0;
};

AudioCenterWindow* g_window = nullptr;

}  // namespace

namespace audiocenter {

bool ShowPanel(HWND parent, const RECT& area, const Context& ctx) {
    if (!g_window) g_window = new AudioCenterWindow();
    return g_window->ShowEmbedded(parent, area, ctx);
}

void HidePanel() {
    if (g_window) g_window->HideEmbedded();
}

void SetPanelBounds(const RECT& area) {
    if (g_window) g_window->SetBounds(area);
}

void SetPanelDpi(UINT dpi) {
    if (g_window) g_window->SetWindowDpi(dpi);
}

bool PanelVisible() { return g_window && g_window->visible(); }

HWND PanelHwnd() { return g_window ? g_window->window() : nullptr; }

void SetPanelVisible(bool visible) {
    if (g_window) g_window->SetVisible(visible);
}

void SetPanelSuspended(bool suspended) {
    if (g_window) g_window->SetSuspended(suspended);
}

void Shutdown() {
    if (!g_window) return;
    g_window->HideEmbedded();
    delete g_window;
    g_window = nullptr;
}

}  // namespace audiocenter

LRESULT CALLBACK AudioCenterWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AudioCenterWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (AudioCenterWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = (AudioCenterWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->Handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool AudioCenterWindow::ShowEmbedded(HWND parent, const RECT& area,
                                     const audiocenter::Context& ctx) {
    if (!parent) return false;
    parent_ = parent;
    ctx_ = ctx;
    UINT dpi = GetDpiForWindow(parent);
    if (!dpi) dpi = 96;
    if (!hwnd_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        dpi_ = dpi;
        hwnd_ = CreateWindowExW(0, kClassName, L"",
                                WS_CHILD | WS_CLIPCHILDREN, area.left, area.top,
                                area.right - area.left, area.bottom - area.top, parent,
                                nullptr, wc.hInstance, this);
        if (!hwnd_) return false;
        CreateControls();
        RecreateFonts();
        Layout();
    } else {
        SetWindowPos(hwnd_, nullptr, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dpi != dpi_) {
        dpi_ = dpi;
        RecreateFonts();
        Layout();
    }
    Refresh(true);
    SetTimer(hwnd_, kTimerRefresh, 2000, nullptr);
    ShowWindow(hwnd_, SW_SHOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void AudioCenterWindow::HideEmbedded() {
    if (!hwnd_) return;
    if (drag_) {
        ReleaseCapture();  // 同步触发 WM_CAPTURECHANGED → EndAppDrag
        EndAppDrag();
    }
    StopMic();
    KillTimer(hwnd_, kTimerRefresh);
    ShowWindow(hwnd_, SW_HIDE);
}

void AudioCenterWindow::SetBounds(const RECT& area) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, area.left, area.top, area.right - area.left,
                 area.bottom - area.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void AudioCenterWindow::SetWindowDpi(UINT dpi) {
    if (!hwnd_ || !dpi || dpi == dpi_) return;
    dpi_ = dpi;
    RecreateFonts();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AudioCenterWindow::SetSuspended(bool suspended) {
    if (!hwnd_) return;
    if (suspended) {
        // 最小化时 IsWindowVisible 仍为 true：必须主动停掉麦克风线程与 2s 刷新，
        // 否则隐藏窗口仍持续采集音频/枚举 COM 会话。
        StopMic();
        KillTimer(hwnd_, kTimerRefresh);
        return;
    }
    Refresh(true);
    SetTimer(hwnd_, kTimerRefresh, 2000, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AudioCenterWindow::SetVisible(bool visible) {
    if (!hwnd_) return;
    if (visible) {
        SetTimer(hwnd_, kTimerRefresh, 2000, nullptr);
        ShowWindow(hwnd_, SW_SHOW);
        Refresh(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        StopMic();
        KillTimer(hwnd_, kTimerRefresh);
        EndAppDrag();
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void AudioCenterWindow::CreateControls() {
    list_ = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP |
                                LBS_NOINTEGRALHEIGHT,
                            0, 0, 10, 10, hwnd_, (HMENU)(INT_PTR)kListId,
                            GetModuleHandleW(nullptr), nullptr);
    outCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL |
                                    WS_TABSTOP,
                                0, 0, 10, Scale(220), hwnd_, (HMENU)(INT_PTR)kOutComboId,
                                GetModuleHandleW(nullptr), nullptr);
    inCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL |
                                   WS_TABSTOP,
                               0, 0, 10, Scale(220), hwnd_, (HMENU)(INT_PTR)kInComboId,
                               GetModuleHandleW(nullptr), nullptr);
    listBrush_ = CreateSolidBrush(kCard);
}

void AudioCenterWindow::RecreateFonts() {
    // DrawTextIn 只 SelectObject、不还原；字体可能仍被选进 mem_ 缓冲。
    // 直接 DeleteObject 会失败并留下悬空选入状态，DPI 切换时每次泄漏一个 HFONT。
    // 先释放整个内存 DC，再删除/重建字体。
    ReleaseBuffer();
    auto del = [](HFONT& f) {
        if (f) DeleteObject(f);
        f = nullptr;
    };
    del(fontTitle_);
    del(fontBody_);
    del(fontBold_);
    del(fontSmall_);
    fontTitle_ = MakeFont(dpi_, 16, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontBody_ = MakeFont(dpi_, 10, FW_NORMAL, L"Microsoft YaHei UI");
    fontBold_ = MakeFont(dpi_, 10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontSmall_ = MakeFont(dpi_, 9, FW_NORMAL, L"Microsoft YaHei UI");
    if (list_) SetFont(list_, fontBody_);
    if (outCombo_) SetFont(outCombo_, fontBody_);
    if (inCombo_) SetFont(inCombo_, fontBody_);
}

void AudioCenterWindow::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int m = Scale(20);
    const int gap = Scale(14);
    const int headerH = Scale(44);
    const int cardsTop = headerH + Scale(4);
    const int leftW = (rc.right - 2 * m - gap) * 42 / 100;
    const int rightX = m + leftW + gap;
    const int bottom = rc.bottom - m;

    rcOutputCard_ = {m, cardsTop, m + leftW, cardsTop + Scale(186)};
    rcMicCard_ = {m, rcOutputCard_.bottom + gap, m + leftW, bottom};
    rcAppsCard_ = {rightX, cardsTop, rc.right - m, bottom};

    rcBack_ = {m, Scale(6), m + Scale(100), Scale(34)};
    rcRefresh_ = {rc.right - m - Scale(92), Scale(6), rc.right - m, Scale(34)};
    rcOpenSettings_ = {rcRefresh_.left - Scale(10) - Scale(150), Scale(6),
                       rcRefresh_.left - Scale(10), Scale(34)};

    // 输出卡
    const int ox = rcOutputCard_.left + Scale(16);
    const int ow = rcOutputCard_.right - Scale(16) - ox;
    rcRenderMute_ = {ox + ow - Scale(56), rcOutputCard_.top + Scale(76),
                     ox + ow, rcOutputCard_.top + Scale(98)};
    rcRenderVol_ = {ox, rcOutputCard_.top + Scale(112), ox + ow - Scale(64),
                    rcOutputCard_.top + Scale(122)};

    // 麦克风卡
    const int mx = rcMicCard_.left + Scale(16);
    const int mw = rcMicCard_.right - Scale(16) - mx;
    rcMicMute_ = {mx + mw - Scale(56), rcMicCard_.top + Scale(76), mx + mw,
                  rcMicCard_.top + Scale(98)};
    rcMicVol_ = {mx, rcMicCard_.top + Scale(112), mx + mw - Scale(64),
                 rcMicCard_.top + Scale(122)};
    rcMicMeter_ = {mx, rcMicCard_.top + Scale(140), mx + mw, rcMicCard_.top + Scale(172)};
    rcMicBtn_ = {mx, rcMicCard_.top + Scale(200), mx + Scale(96), rcMicCard_.top + Scale(230)};
    rcNoMicLink_ = {mx, rcMicCard_.top + Scale(80), mx + mw, rcMicCard_.top + Scale(104)};

    // 应用卡
    const int ax = rcAppsCard_.left + Scale(16);
    const int aw = rcAppsCard_.right - Scale(16) - ax;
    const int listTop = rcAppsCard_.top + Scale(42);
    const int listH = std::max(Scale(110), (int)(rcAppsCard_.bottom - listTop - Scale(300)));
    rcList_ = {ax, listTop, ax + aw, listTop + listH};
    const int dy = rcList_.bottom + Scale(16);
    rcAppMute_ = {ax + aw - Scale(56), dy + Scale(30), ax + aw, dy + Scale(52)};
    rcAppVol_ = {ax, dy + Scale(76), ax + aw - Scale(64), dy + Scale(86)};
    rcOutLabel_ = {ax, dy + Scale(96), ax + aw - Scale(228), dy + Scale(122)};
    rcOutCombo_ = {ax + aw - Scale(220), dy + Scale(94), ax + aw, dy + Scale(122)};
    rcInLabel_ = {ax, dy + Scale(132), ax + aw - Scale(228), dy + Scale(158)};
    rcInCombo_ = {ax + aw - Scale(220), dy + Scale(130), ax + aw, dy + Scale(158)};
    rcAppSettings_ = {ax, rcAppsCard_.bottom - Scale(34), ax + aw,
                      rcAppsCard_.bottom - Scale(10)};

    if (list_) MoveWindow(list_, rcList_.left, rcList_.top, rcList_.right - rcList_.left,
                          rcList_.bottom - rcList_.top, TRUE);
    if (outCombo_) {
        MoveWindow(outCombo_, rcOutCombo_.left, rcOutCombo_.top,
                   rcOutCombo_.right - rcOutCombo_.left, Scale(220), TRUE);
    }
    if (inCombo_) {
        MoveWindow(inCombo_, rcInCombo_.left, rcInCombo_.top,
                   rcInCombo_.right - rcInCombo_.left, Scale(220), TRUE);
    }
}

void AudioCenterWindow::EnsureBuffer(HDC dc, int w, int h) {
    if (mem_ && memW_ == w && memH_ == h) return;
    ReleaseBuffer();
    if (w <= 0 || h <= 0) return;
    mem_ = CreateCompatibleDC(dc);
    bmp_ = CreateCompatibleBitmap(dc, w, h);
    if (!mem_ || !bmp_) {
        ReleaseBuffer();
        return;
    }
    memOld_ = SelectObject(mem_, bmp_);
    memW_ = w;
    memH_ = h;
}

void AudioCenterWindow::ReleaseBuffer() {
    if (mem_ && memOld_) SelectObject(mem_, memOld_);
    if (bmp_) DeleteObject(bmp_);
    if (mem_) DeleteDC(mem_);
    mem_ = nullptr;
    bmp_ = nullptr;
    memOld_ = nullptr;
    memW_ = 0;
    memH_ = 0;
}

void AudioCenterWindow::Paint(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    EnsureBuffer(dc, rc.right, rc.bottom);
    if (!mem_) return;
    PaintBuffer(mem_);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem_, 0, 0, SRCCOPY);
}

void AudioCenterWindow::DrawCard(HDC dc, const RECT& rc, const std::wstring& title) {
    GlassPanel(dc, rc, Scale(8));
    RECT tr{rc.left + Scale(16), rc.top + Scale(8), rc.right - Scale(16), rc.top + Scale(30)};
    DrawTextIn(dc, title, tr, fontBold_, kSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void AudioCenterWindow::DrawVolumeRow(HDC dc, const RECT& bar, const RECT& pill, int vol,
                                      bool mute, const wchar_t* label, const RECT& area) {
    (void)area;
    RECT lr{bar.left, bar.top - Scale(24), bar.right, bar.top - Scale(2)};
    DrawTextIn(dc, label, lr, fontSmall_, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT pr = lr;
    DrawTextIn(dc, vol >= 0 ? Fmt(L"%d%%", vol) : std::wstring(L"--"), pr, fontBold_,
               kText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    FillRounded(dc, bar, Scale(5), kTrack, kTrack);
    if (vol > 0) {
        RECT fill{bar.left, bar.top,
                  bar.left + (bar.right - bar.left) * audio::ClampVolume(vol) / 100, bar.bottom};
        if (fill.right > fill.left) FillRounded(dc, fill, Scale(5), kAccent, kAccent);
    }
    FillRounded(dc, pill, Scale(10), mute ? kAccentSoft : kHover,
                mute ? kAccentSoft : kHover);
    DrawTextIn(dc, mute ? L"已静音" : L"静音", pill, fontSmall_, mute ? kAccent : kSub,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void AudioCenterWindow::DrawMeter(HDC dc, const RECT& rc, float level) {
    const int n = 20;
    const int gap = Scale(2);
    const int w = (rc.right - rc.left - gap * (n - 1)) / n;
    if (w <= 0) return;
    const int lit = (int)std::lround(std::max(0.0f, std::min(1.0f, level)) * n);
    for (int i = 0; i < n; ++i) {
        RECT seg{rc.left + i * (w + gap), rc.top, rc.left + i * (w + gap) + w, rc.bottom};
        COLORREF c = kTrack;
        if (i < lit) c = i < 14 ? kGreen : (i < 18 ? kAmber : kRed);
        FillRounded(dc, seg, Scale(2), c, c);
    }
    RECT cap{rc.left, rc.bottom + Scale(4), rc.right, rc.bottom + Scale(20)};
    DrawTextIn(dc, Fmt(L"输入电平 %.0f%%", level * 100.0f), cap, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void AudioCenterWindow::DrawButton(HDC dc, const RECT& rc, const std::wstring& text,
                                   bool primary) {
    FillRounded(dc, rc, Scale(6), primary ? kAccent : kCard, primary ? kAccent : kBorder);
    DrawTextIn(dc, text, rc, fontBody_, primary ? RGB(255, 255, 255) : kText,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

bool AudioCenterWindow::MicStarting() const {
    // MicMonitor::Start 立即返回，线程里的 WASAPI 初始化可能超过 33 ms。
    // 在宽限期内 !running() 不能当成“已停止”，否则 UI 会复位按钮/杀掉定时器，
    // 但后台采集线程随后成功启动并一直运行到折叠面板。
    return micActive_ && mic_.error().empty() && !mic_.running() &&
           GetTickCount64() - micStartTick_ < 1500;
}

void AudioCenterWindow::SettleMicState() {
    if (!micActive_) return;
    if (!mic_.error().empty() || (!mic_.running() && !MicStarting())) {
        // Stop() 会 join 线程；即使线程刚初始化完，stop 标志也会让它立刻退出，
        // 不会留下“UI 已停止、后台仍在采集”的状态。
        StopMic();
    }
}

std::wstring AudioCenterWindow::MicStatusText() const {
    if (micIndex_ < 0) return L"未检测到该设备的麦克风";
    if (micActive_) {
        const std::wstring err = mic_.error();
        if (!err.empty()) return err;
        return mic_.running() ? L"正在测试 · 说话查看电平" : L"正在启动麦克风…";
    }
    if (!mic_.error().empty()) return mic_.error();
    return L"点击开始测试，电平会实时变化";
}

std::wstring AudioCenterWindow::AppStatusText() const {
    if (!appStatus_.empty()) return appStatus_;
    if (selectedSession_ < 0 || selectedSession_ >= (int)sessions_.size()) {
        return sessions_.empty() ? L"没有正在播放音频的应用" : L"选择一个应用调整音量";
    }
    const audio::AppSession& s = sessions_[selectedSession_];
    if (!s.endpointName.empty()) return Fmt(L"当前播放端点：%s", s.endpointName.c_str());
    return L"应用设备设置会按应用持久化";
}

void AudioCenterWindow::PaintBuffer(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    PaintBackdrop(dc, rc);

    DrawTextIn(dc, L"‹ 返回设备", rcBack_, fontBody_, kAccent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawButton(dc, rcRefresh_, L"刷新", false);
    DrawButton(dc, rcOpenSettings_, L"打开 Windows 声音设置", false);

    // 系统输出
    DrawCard(dc, rcOutputCard_, L"系统输出");
    RECT dev{rcOutputCard_.left + Scale(16), rcOutputCard_.top + Scale(34),
             rcOutputCard_.right - Scale(16), rcOutputCard_.top + Scale(58)};
    DrawTextIn(dc, renderName_.empty() ? L"未找到播放端点" : renderName_, dev, fontBold_,
               kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawVolumeRow(dc, rcRenderVol_, rcRenderMute_, renderVol_, renderMute_, L"音量",
                  rcOutputCard_);
    RECT oh{rcOutputCard_.left + Scale(16), rcOutputCard_.bottom - Scale(46),
            rcOutputCard_.right - Scale(16), rcOutputCard_.bottom - Scale(12)};
    DrawTextIn(dc, L"当前系统默认输出；应用内的音量条直接写入该端点。", oh, fontSmall_, kMuted,
               DT_LEFT | DT_TOP | DT_WORDBREAK);

    // 麦克风
    DrawCard(dc, rcMicCard_, L"麦克风");
    if (micIndex_ < 0) {
        RECT no{rcMicCard_.left + Scale(16), rcMicCard_.top + Scale(38),
                rcMicCard_.right - Scale(16), rcMicCard_.top + Scale(64)};
        DrawTextIn(dc, L"未检测到该设备的麦克风", no, fontBody_, kSub,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT link{rcNoMicLink_.left, rcNoMicLink_.top, rcNoMicLink_.right,
                  rcNoMicLink_.bottom};
        DrawTextIn(dc, L"打开 Windows 声音设置 ›", link, fontSmall_, kAccent,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT devm{rcMicCard_.left + Scale(16), rcMicCard_.top + Scale(34),
                  rcMicCard_.right - Scale(16), rcMicCard_.top + Scale(58)};
        DrawTextIn(dc, micName_, devm, fontBold_, kText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawVolumeRow(dc, rcMicVol_, rcMicMute_, micVol_, micMute_, L"输入音量", rcMicCard_);
        DrawMeter(dc, rcMicMeter_, shownLevel_);
        DrawButton(dc, rcMicBtn_, micActive_ ? L"停止测试" : L"开始测试", micActive_);
        RECT ms{rcMicBtn_.right + Scale(12), rcMicBtn_.top, rcMicCard_.right - Scale(16),
                rcMicBtn_.bottom};
        DrawTextIn(dc, MicStatusText(), ms, fontSmall_,
                   mic_.error().empty() ? kMuted : kRed,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 应用音量
    DrawCard(dc, rcAppsCard_, L"应用音量");
    RECT cnt{rcAppsCard_.left + Scale(16), rcAppsCard_.top + Scale(8),
             rcAppsCard_.right - Scale(16), rcAppsCard_.top + Scale(30)};
    DrawTextIn(dc, Fmt(L"%d 个应用", (int)sessions_.size()), cnt, fontSmall_, kMuted,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    if (sessions_.empty()) {
        RECT empty{rcList_.left, rcList_.top + Scale(20), rcList_.right, rcList_.top + Scale(48)};
        DrawTextIn(dc, L"没有正在播放音频的应用", empty, fontBody_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    const int dy = rcList_.bottom + Scale(16);
    if (selectedSession_ >= 0 && selectedSession_ < (int)sessions_.size()) {
        const audio::AppSession& s = sessions_[selectedSession_];
        RECT name{rcAppsCard_.left + Scale(16), dy, rcAppsCard_.right - Scale(16),
                  dy + Scale(28)};
        DrawTextIn(dc, s.name, name, fontTitle_, kText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT meta{rcAppsCard_.left + Scale(16), dy + Scale(30), rcAppsCard_.right - Scale(16),
                  dy + Scale(50)};
        DrawTextIn(dc, Fmt(L"%d 个活动会话 · PID %lu", s.sessionCount,
                           (unsigned long)s.pid),
                   meta, fontSmall_, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawVolumeRow(dc, rcAppVol_, rcAppMute_, appVol_, appMute_, L"应用音量", rcAppsCard_);
    }
    DrawTextIn(dc, L"输出设备", rcOutLabel_, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, L"输入设备", rcInLabel_, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT hint{rcAppsCard_.left + Scale(16), rcInCombo_.bottom + Scale(8),
              rcAppsCard_.right - Scale(16), rcAppSettings_.top - Scale(4)};
    DrawTextIn(dc, AppStatusText(), hint, fontSmall_, kMuted,
               DT_LEFT | DT_TOP | DT_WORDBREAK);
    DrawTextIn(dc, L"打开 Windows 音量合成器 ›", rcAppSettings_, fontSmall_, kAccent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

const audio::Endpoint* AudioCenterWindow::FindRender() const {
    if (!ctx_.renderEndpointId.empty()) {
        for (const auto& e : renders_) {
            if (e.id == ctx_.renderEndpointId) return &e;
        }
    }
    for (const auto& e : renders_) {
        if (e.isDefault) return &e;
    }
    return renders_.empty() ? nullptr : &renders_.front();
}

const audio::Endpoint* AudioCenterWindow::FindCapture() const {
    if (micIndex_ < 0 || micIndex_ >= (int)captures_.size()) return nullptr;
    return &captures_[micIndex_];
}

void AudioCenterWindow::Refresh(bool force) {
    if (!hwnd_) return;
    renders_ = audio::ListRenderEndpoints();
    captures_ = audio::ListCaptureEndpoints();

    const audio::Endpoint* render = FindRender();
    const std::wstring oldMic = micId_;
    renderId_.clear();
    renderName_.clear();
    renderVol_ = -1;
    renderMute_ = false;
    if (render) {
        renderId_ = render->id;
        renderName_ = render->name;
        renderVol_ = render->volume;
        renderMute_ = render->mute;
    }

    micIndex_ = audio::MatchCaptureEndpoint(ctx_.deviceName, ctx_.product,
                                            render ? render->containerId : std::wstring(),
                                            captures_);
    micId_.clear();
    micName_.clear();
    micVol_ = -1;
    micMute_ = false;
    if (const audio::Endpoint* cap = FindCapture()) {
        micId_ = cap->id;
        micName_ = cap->name;
        micVol_ = cap->volume;
        micMute_ = cap->mute;
    }
    if (micActive_ && oldMic != micId_) StopMic();

    const auto fresh = audio::ListAppSessions();
    std::string key;
    for (const auto& s : fresh) {
        key += std::to_string(s.pid) + "|" + std::to_string(s.sessionCount) + "|";
        key += WideToUtf8(s.name) + ";";
    }
    if (force || key != sessionKey_) {
        sessionKey_ = key;
        sessions_ = fresh;
        RebuildSessionList();
    } else {
        sessions_ = fresh;
        RefreshSelectedSession();
    }

    if (micActive_) SettleMicState();
    PopulateAppCombos(force);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AudioCenterWindow::RebuildSessionList() {
    if (!list_) return;
    const unsigned long keep = selectedPid_;
    const int oldTop = (int)SendMessageW(list_, LB_GETTOPINDEX, 0, 0);
    selectedSession_ = -1;
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < (int)sessions_.size(); ++i) {
        const audio::AppSession& s = sessions_[i];
        std::wstring text = s.name;
        if (s.sessionCount > 1) text += Fmt(L"（%d 个会话）", s.sessionCount);
        int idx = (int)SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)text.c_str());
        if (idx >= 0) SendMessageW(list_, LB_SETITEMDATA, idx, (LPARAM)(INT_PTR)i);
        if (keep != 0 && s.pid == keep) selectedSession_ = i;
    }
    // 用户已选中某个应用但它消失时保持“无选中”，不要静默改选另一个进程；
    // 否则拖动中的音量会写到用户没选的应用上。只有从未选择时才默认第一个。
    if (selectedSession_ < 0 && keep == 0 && !sessions_.empty()) selectedSession_ = 0;
    if (selectedSession_ >= 0) {
        SendMessageW(list_, LB_SETCURSEL, selectedSession_, 0);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    if (oldTop > 0) SendMessageW(list_, LB_SETTOPINDEX, (WPARAM)oldTop, 0);
    RefreshSelectedSession();
    PopulateAppCombos(true);
}
void AudioCenterWindow::RefreshSelectedSession() {
    appVol_ = -1;
    appMute_ = false;
    if (selectedSession_ < 0 || selectedSession_ >= (int)sessions_.size()) {
        selectedPid_ = 0;
        return;
    }
    const audio::AppSession& s = sessions_[selectedSession_];
    selectedPid_ = s.pid;
    appVol_ = s.volume;
    appMute_ = s.mute;
}

void AudioCenterWindow::PopulateAppCombos(bool force) {
    if (!outCombo_ || !inCombo_) return;
    unsigned long pid = 0;
    if (selectedSession_ >= 0 && selectedSession_ < (int)sessions_.size()) {
        pid = sessions_[selectedSession_].pid;
    }
    std::string rk;
    std::string ck;
    for (const auto& e : renders_) rk += WideToUtf8(e.id) + "|" + WideToUtf8(e.name) + ";";
    for (const auto& e : captures_) ck += WideToUtf8(e.id) + "|" + WideToUtf8(e.name) + ";";
    const bool listsChanged = rk != renderKey_ || ck != captureKey_;
    // 每 2 s 的刷新不重置下拉框，避免用户正在展开列表时被关掉。
    if (!force && !listsChanged && pid == comboPid_) return;
    renderKey_ = rk;
    captureKey_ = ck;
    comboPid_ = pid;

    auto fill = [](HWND combo, const std::vector<audio::Endpoint>& eps) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"默认");
        if (idx >= 0) SendMessageW(combo, CB_SETITEMDATA, idx, 0);
        for (int i = 0; i < (int)eps.size(); ++i) {
            idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)eps[i].name.c_str());
            if (idx >= 0) SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM)(INT_PTR)(i + 1));
        }
    };
    fill(outCombo_, renders_);
    fill(inCombo_, captures_);

    if (pid == 0) {
        EnableWindow(outCombo_, FALSE);
        EnableWindow(inCombo_, FALSE);
        SendMessageW(outCombo_, CB_SETCURSEL, 0, 0);
        SendMessageW(inCombo_, CB_SETCURSEL, 0, 0);
        return;
    }
    EnableWindow(outCombo_, TRUE);
    EnableWindow(inCombo_, TRUE);
    auto select = [&](HWND combo, bool capture) {
        std::wstring id;
        audio::GetAppEndpoint(pid, capture, id);
        int sel = 0;
        const auto& eps = capture ? captures_ : renders_;
        if (!id.empty()) {
            for (int i = 0; i < (int)eps.size(); ++i) {
                if (eps[i].id == id) {
                    sel = i + 1;
                    break;
                }
            }
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    };
    select(outCombo_, false);
    select(inCombo_, true);
}

void AudioCenterWindow::ApplyAppEndpoint(bool capture) {
    if (selectedSession_ < 0 || selectedSession_ >= (int)sessions_.size()) return;
    const audio::AppSession& s = sessions_[selectedSession_];
    if (s.pid == 0) return;
    HWND combo = capture ? inCombo_ : outCombo_;
    const int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    const INT_PTR data = (INT_PTR)SendMessageW(combo, CB_GETITEMDATA, sel, 0);
    const auto& eps = capture ? captures_ : renders_;
    std::wstring id;
    if (data > 0 && data <= (INT_PTR)eps.size()) id = eps[(size_t)data - 1].id;
    const bool ok = audio::SetAppEndpoint(s.pid, capture, id);
    if (ok) {
        appStatus_ = id.empty()
                         ? L"已恢复默认设备；部分应用需重新开始播放/录音后生效"
                         : L"已应用；部分应用需重新开始播放/录音后生效";
    } else {
        appStatus_ = L"设置失败：该应用当前没有音频会话或系统不支持";
        comboPid_ = 0;
        PopulateAppCombos(true);
    }
    InvalidateRect(hwnd_, &rcAppsCard_, FALSE);
}

void AudioCenterWindow::ApplyDrag(int x) {
    RECT bar{};
    if (drag_ == 1) bar = rcRenderVol_;
    else if (drag_ == 2) bar = rcAppVol_;
    else if (drag_ == 3) bar = rcMicVol_;
    else return;
    const int w = bar.right - bar.left;
    if (w <= 0) return;
    const int pct = audio::ClampVolume((int)((x - bar.left) * 100.0 / w + 0.5));
    if (drag_ == 1) {
        if (pct == renderVol_) return;
        renderVol_ = pct;
        renderMute_ = false;
        if (!audio::SetVolume(renderId_, pct)) appStatus_ = L"无法写入系统输出音量";
        audio::SetMute(renderId_, false);
    } else if (drag_ == 2) {
        // 拖动期间 2 s 刷新可能重建列表并改变选中项；必须锁定按下时的 PID，
        // 否则会把音量写到另一个应用。
        if (selectedPid_ == 0 || dragPid_ == 0 || selectedPid_ != dragPid_) {
            EndAppDrag();
            return;
        }
        if (pct == appVol_) return;
        appVol_ = pct;
        appMute_ = false;
        if (!WriteAppVolume(pct)) {
            // 会话在拖动中被应用结束：提示并结束拖动，避免后续每条鼠标消息继续重试。
            appStatus_ = L"无法调整该应用音量（会话可能已结束）";
            EndAppDrag();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        appStatus_.clear();
        if (selectedSession_ >= 0 && selectedSession_ < (int)sessions_.size()) {
            sessions_[selectedSession_].volume = pct;
            sessions_[selectedSession_].mute = false;
        }
    } else if (drag_ == 3) {
        if (pct == micVol_) return;
        micVol_ = pct;
        micMute_ = false;
        if (!audio::SetVolume(micId_, pct)) appStatus_ = L"无法写入麦克风音量";
        audio::SetMute(micId_, false);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool AudioCenterWindow::WriteAppVolume(int pct) {
    if (appWriter_.SetVolumeUnmuted(pct) > 0) return true;
    // B03：应用可能在拖动中重建会话（例如切换输出设备后新建会话），
    // 按 PlanAppWrite 只重开一次缓存；仍写不进去就交给调用方结束拖动，
    // 不再每条鼠标消息都枚举全部端点/会话。
    if (audio::PlanAppWrite(0, appWriteReopened_) != audio::AppWritePlan::Reopen) return false;
    appWriteReopened_ = true;
    appWriter_.Open(selectedPid_);
    return appWriter_.SetVolumeUnmuted(pct) > 0;
}

void AudioCenterWindow::EndAppDrag() {
    if (drag_ == 2) appWriter_.Close();
    appWriteReopened_ = false;
    drag_ = 0;
    dragPid_ = 0;
}

void AudioCenterWindow::ToggleMicTest() {
    if (micIndex_ < 0 || micId_.empty()) return;
    if (micActive_) {
        StopMic();
        InvalidateRect(hwnd_, &rcMicCard_, FALSE);
        return;
    }
    shownLevel_ = 0.0f;
    if (mic_.Start(micId_)) {
        micActive_ = true;
        micStartTick_ = GetTickCount64();
        SetTimer(hwnd_, kTimerMeter, 33, nullptr);
    }
    InvalidateRect(hwnd_, &rcMicCard_, FALSE);
}

void AudioCenterWindow::StopMic() {
    mic_.Stop();
    micActive_ = false;
    micStartTick_ = 0;
    shownLevel_ = 0.0f;
    if (hwnd_) KillTimer(hwnd_, kTimerMeter);
}

LRESULT AudioCenterWindow::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED_AFTERPARENT: {
            UINT dpi = GetDpiForWindow(hwnd);
            if (dpi && dpi != dpi_) {
                dpi_ = dpi;
                RecreateFonts();
                Layout();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CTLCOLORLISTBOX: {
            if (!listBrush_) return DefWindowProcW(hwnd, msg, wp, lp);
            HDC dc = (HDC)wp;
            SetBkColor(dc, kCard);
            SetTextColor(dc, kText);
            return (LRESULT)listBrush_;
        }
        case WM_TIMER:
            if (wp == kTimerRefresh) {
                Refresh(false);
            } else if (wp == kTimerMeter) {
                SettleMicState();
                if (micActive_) {
                    shownLevel_ = std::max(mic_.Level(), shownLevel_ * 0.85f);
                } else {
                    shownLevel_ = 0.0f;
                }
                InvalidateRect(hwnd, &rcMicCard_, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kOutComboId && HIWORD(wp) == CBN_SELCHANGE) {
                ApplyAppEndpoint(false);
                return 0;
            }
            if (LOWORD(wp) == kInComboId && HIWORD(wp) == CBN_SELCHANGE) {
                ApplyAppEndpoint(true);
                return 0;
            }
            if (LOWORD(wp) == kListId && HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    const INT_PTR data = (INT_PTR)SendMessageW(list_, LB_GETITEMDATA, sel, 0);
                    if (data >= 0 && data < (INT_PTR)sessions_.size()) {
                        selectedSession_ = (int)data;
                        RefreshSelectedSession();
                        appStatus_.clear();
                        PopulateAppCombos(true);
                        InvalidateRect(hwnd, &rcAppsCard_, FALSE);
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&rcBack_, p)) {
                if (parent_) PostMessageW(parent_, audiocenter::WM_BACK, 0, 0);
                return 0;
            }
            if (PtInRect(&rcRefresh_, p)) {
                Refresh(true);
                return 0;
            }
            if (PtInRect(&rcOpenSettings_, p) || PtInRect(&rcAppSettings_, p)) {
                audio::OpenSoundSettings();
                return 0;
            }
            // 无麦克风提示里的“打开设置”链接只在 micIndex_<0 时有效；
            // 否则它的矩形会覆盖静音胶囊/输入音量行，点静音却弹出系统设置。
            if (micIndex_ < 0 && PtInRect(&rcNoMicLink_, p)) {
                audio::OpenSoundSettings();
                return 0;
            }
            if (PtInRect(&rcRenderMute_, p)) {
                renderMute_ = !renderMute_;
                audio::SetMute(renderId_, renderMute_);
                InvalidateRect(hwnd, &rcOutputCard_, FALSE);
                return 0;
            }
            if (micIndex_ >= 0 && PtInRect(&rcMicMute_, p)) {
                micMute_ = !micMute_;
                audio::SetMute(micId_, micMute_);
                InvalidateRect(hwnd, &rcMicCard_, FALSE);
                return 0;
            }
            if (selectedSession_ >= 0 && PtInRect(&rcAppMute_, p)) {
                appMute_ = !appMute_;
                if (!audio::SetAppMute(selectedPid_, appMute_)) {
                    appStatus_ = L"无法切换该应用静音（会话可能已结束）";
                }
                InvalidateRect(hwnd, &rcAppsCard_, FALSE);
                return 0;
            }
            if (micIndex_ >= 0 && PtInRect(&rcMicBtn_, p)) {
                ToggleMicTest();
                return 0;
            }
            if (PtInRect(&rcRenderVol_, p)) {
                drag_ = 1;
                dragPid_ = 0;
                SetCapture(hwnd);
                ApplyDrag(p.x);
                return 0;
            }
            if (selectedSession_ >= 0 && PtInRect(&rcAppVol_, p)) {
                drag_ = 2;
                dragPid_ = selectedPid_;
                // 按下时解析一次该 PID 的会话接口；拖动期间只写缓存。
                const bool opened = appWriter_.Open(selectedPid_);
                // 按下时就一个会话都没有：拖动中不必再重开缓存，直接按失败处理。
                appWriteReopened_ = !opened;
                SetCapture(hwnd);
                ApplyDrag(p.x);
                return 0;
            }
            if (micIndex_ >= 0 && PtInRect(&rcMicVol_, p)) {
                drag_ = 3;
                dragPid_ = 0;
                SetCapture(hwnd);
                ApplyDrag(p.x);
                return 0;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (drag_) {
                ApplyDrag(GET_X_LPARAM(lp));
                return 0;
            }
            return 0;
        case WM_LBUTTONUP:
            if (drag_) {
                ReleaseCapture();  // 同步进入 WM_CAPTURECHANGED，清理统一在那里做
                EndAppDrag();
            }
            return 0;
        case WM_CAPTURECHANGED:
            EndAppDrag();
            return 0;
        case WM_DESTROY:
            StopMic();
            appWriter_.Close();
            KillTimer(hwnd, kTimerRefresh);
            ReleaseBuffer();
            if (listBrush_) DeleteObject(listBrush_);
            listBrush_ = nullptr;
            {
                auto del = [](HFONT& f) {
                    if (f) DeleteObject(f);
                    f = nullptr;
                };
                del(fontTitle_);
                del(fontBody_);
                del(fontBold_);
                del(fontSmall_);
            }
            parent_ = nullptr;
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            list_ = nullptr;
            outCombo_ = nullptr;
            inCombo_ = nullptr;
            return DefWindowProcW(hwnd, msg, wp, lp);
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

