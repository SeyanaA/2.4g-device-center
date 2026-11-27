#pragma once

#include <vector>

#include "common.h"
#include "keyboard_test.h"
#include "mouse_test.h"
#include "protocols.h"

// 鼠标/键盘测试视图：自绘卡片 + 一个原生目标设备下拉框。
// 由 MainWindow 负责布局、绘制调度、WM_INPUT/WM_COMMAND 转发。
class MouseTestView {
public:
    enum class Mode { Mouse, Keyboard };
    static constexpr int kComboId = 3001;

    ~MouseTestView();

    void SetDpi(UINT dpi);
    void SetDevices(const std::vector<Device>& devices, const std::wstring& selectedId);
    void Show(HWND parent, bool on);
    bool visible() const { return visible_; }
    Mode mode() const { return mode_; }
    // 内容头标题：鼠标测试 / 键盘测试。
    const wchar_t* title() const { return mode_ == Mode::Keyboard ? L"键盘测试" : L"鼠标测试"; }

    void Layout(const RECT& content);
    void Paint(HDC dc);

    // 内容头副标题显示目标状态（正在测试/已断开/未匹配/demo），
    // 右侧边界让给目标下拉框，避免与工具条重叠。
    const std::wstring& status_note() const { return statusNote_; }
    int header_status_right() const {
        return mode_ == Mode::Keyboard ? (rcKbApm_.left > 0 ? rcKbApm_.left - Scale(12)
                                                           : rcContent_.right - Scale(20))
                                       : rcCombo_.left - Scale(12);
    }

    // 入场动画期间隐藏/恢复自建的原生子控件（目标设备下拉框）。
    // 它不在 MainWindow::HideContentChildren 的清单里，必须由这里转发。
    void SetChildrenHidden(bool hidden);
    // demo 模式不注册 Raw Input：演示数据不应被真实鼠标输入污染。
    void SetDemoMode(bool demo) { demo_ = demo; }

    // 返回 true 表示该点击已被测试页控件消费。
    bool OnClick(int x, int y);
    bool OnMouseMove(int x, int y);
    // 只更新统计使用的光标位置（不触发悬停重绘）。原生子控件（下拉框）
    // 会吞掉父窗口的 WM_MOUSEMOVE，必须由 ui.cpp 在收到移动时同步。
    void TrackCursor(int x, int y);
    // 由 ui_test_view.cpp 内的父窗口子类过程在 WM_MOUSELEAVE 时调用，
    // ui.cpp 若将来直接转发该消息也可复用。
    void OnMouseLeave();
    // 窗口失焦：键盘测试记录 Blur 并清空按住态；鼠标测试无操作。
    void OnBlur();
    // 重新获得焦点：清除本次失焦的去重标记，保证下一次失焦仍记录 Blur。
    void OnFocus();
    void OnTimer();
    void HandleRawInput(HRAWINPUT raw);
    void HandleCommand(int id, int code);
private:
    void EnsureControls();
    void UpdateComboVisibility();
    void RecreateFonts();
    void ReloadCombo();
    void MatchTarget();
    void ResetAll();
    void SetMode(Mode mode);
    bool IsOverPageButton(POINT p) const;
    void DrawTopBar(HDC dc);
    void DrawButtonsCard(HDC dc, const MouseTestSnapshot& s);
    void DrawStatsCard(HDC dc, const MouseTestSnapshot& s);
    void DrawPollingCard(HDC dc, const MouseTestSnapshot& s);
    void DrawClickCard(HDC dc, const MouseTestSnapshot& s);
    void DrawTrajectoryCard(HDC dc, const MouseTestSnapshot& s);
    void DrawTipsCard(HDC dc);
    void DrawMouseShape(HDC dc, const RECT& rc);
    void DrawCard(HDC dc, const RECT& rc, const std::wstring& title) const;
    void DrawButton(HDC dc, const RECT& rc, const std::wstring& text, bool primary,
                    bool hover) const;
    void DrawSegmented(HDC dc, const RECT& rc, const wchar_t* a, const wchar_t* b,
                       int selected) const;

    // ---- 键盘测试 --------------------------------------------------------
    void PaintKeyboard(HDC dc);
    void DrawKeyboardTopBar(HDC dc);
    void DrawKeyboardBoard(HDC dc);
    void DrawKeyboardPanel(HDC dc);
    RECT KbKeyRect(int index) const;
    void HandleKeyboardInput(const RAWKEYBOARD& kb, bool down);
    void AppendKbLog(const wchar_t* key, const wchar_t* event);
    void ClearKeyboard();
    std::wstring NowClockMs() const;
    int Scale(int v) const { return MulDiv(v, (int)dpi_, 96); }
    RECT ClientToView(int x, int y) const;

    HWND parent_ = nullptr;
    HWND combo_ = nullptr;
    UINT dpi_ = 96;
    bool visible_ = false;
    bool rawRegistered_ = false;
    bool mouseRawOk_ = false;
    bool demo_ = false;
    // 内容入场动画期间的隐藏状态；模式切换不能把原生下拉框提前显示出来。
    bool introHidden_ = false;

    HFONT fontTitle_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontBig_ = nullptr;
    HFONT fontKey_ = nullptr;
    HFONT fontKeySmall_ = nullptr;
    HFONT fontMono_ = nullptr;

    Mode mode_ = Mode::Mouse;
    MouseTestEngine engine_;
    keyboardtest::KeyboardTestEngine keyboard_;
    struct KbLogEntry {
        std::wstring time;
        std::wstring key;
        std::wstring event;
    };
    std::vector<KbLogEntry> kbLog_;
    int kbPanelTab_ = 0;  // 0=日志，1=诊断
    int kbHover_ = 0;
    int lastApm_ = -1;
    ULONGLONG kbLastApmTick_ = 0;
    // 30s 折线每秒推进；即使 APM 数值不变也必须重绘一次。
    ULONGLONG kbLastHistSec_ = 0;
    bool keyboardRawOk_ = false;
    bool kbBlurred_ = false;
    std::vector<Device> devices_;
    std::wstring selectedId_;
    std::wstring targetName_;
    // 目标设备 id：拔插后按 id 优先恢复，避免同名设备选错。
    std::wstring targetDeviceId_;
    // 目标已从快照消失（顶部提示「目标设备已断开」，统计暂停）。
    bool targetLost_ = false;
    // 目标句柄未匹配或 demo 模式：暂停统计，不能静默回落成“统计任意鼠标”。
    bool inputPaused_ = false;
    std::wstring lostTargetName_;
    HANDLE targetRaw_ = nullptr;
    bool targetMatched_ = false;
    std::wstring statusNote_;
    int comboIndex_ = 0;
    int windowSec_ = 5;

    RECT rcContent_{};
    RECT rcHeader_{};
    RECT rcTop_{};
    RECT rcCombo_{};
    RECT rcRedetect_{};
    RECT rcWin5_{};
    RECT rcWin10_{};
    RECT rcReset_{};
    RECT rcBtnCard_{};
    RECT rcStatsCard_{};
    RECT rcTipsCard_{};
    RECT rcPollCard_{};
    RECT rcClickCard_{};
    RECT rcTrajCard_{};
    RECT rcTrajPlot_{};
    RECT rcClear_{};
    // 键盘模式：顶部模式切换/清空/APM、键盘板与底部日志/诊断面板。
    RECT rcModeMouse_{};
    RECT rcModeKeyboard_{};
    RECT rcKbClear_{};
    RECT rcKbLayout_{};
    RECT rcKbApm_{};
    RECT rcKbBoard_{};
    RECT rcKbPanel_{};
    RECT rcKbLogTab_{};
    RECT rcKbDiagTab_{};
    float kbUnit_ = 1.0f;
    // 按键/滚轮卡左侧的鼠标图形区域：只有光标落在其中时按键与滚轮才计数。
    RECT rcShape_{};

    int hover_ = 0;  // 1 redetect, 2 win5, 3 win10, 4 reset, 5 clear
    int pressed_ = -1;
    ULONGLONG lastPressTick_ = 0;
    std::vector<POINT> jumpPoints_;
    POINT lastTrajPoint_{};
    POINT cursor_{};
    bool cursorValid_ = false;
    ULONGLONG lastCursorSync_ = 0;
    bool hasLastTraj_ = false;
    bool dirty_ = true;
    bool targetChosen_ = false;  // 用户在下拉框里显式选择过目标设备
    bool trackLeave_ = false;    // 已向 parent_ 注册 TrackMouseEvent(TME_LEAVE)
    bool subclassed_ = false;    // 已在 parent_ 上安装 WM_MOUSELEAVE 子类过程
    int64_t qpcFreq_ = 0;
};
