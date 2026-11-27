#include "ui_test_view.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "svg_icon.h"
#include "ui_theme.h"

using namespace uitheme;
using keyboardtest::KeyState;

namespace {

constexpr wchar_t kAnyMouseName[] = L"任意鼠标";

// ui.cpp 没有 WM_MOUSELEAVE 分支，而该消息只会发给调用 TrackMouseEvent 的窗口。
// 这里在父窗口上挂一个只观察 WM_MOUSELEAVE 的子类过程，其余消息原样交回，
// 从而不必修改 ui.cpp 也能清除悬停高亮。
constexpr UINT_PTR kTestViewSubclassId = 0x4D54;  // 'MT'

LRESULT CALLBACK TestViewSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR /*id*/, DWORD_PTR ref) {
    if (ref) {
        auto* view = reinterpret_cast<MouseTestView*>(ref);
        if (msg == WM_MOUSELEAVE) view->OnMouseLeave();
        else if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)) {
            // 键盘 Raw Input 只在前台窗口到达；失焦时必须记录 Blur 并清空按住态。
            view->OnBlur();
        } else if (msg == WM_SETFOCUS ||
                   (msg == WM_ACTIVATE && LOWORD(wp) == WA_ACTIVE)) {
            // 重新获得焦点后允许记录下一次失焦；否则第二次 Blur 会被去重吞掉。
            view->OnFocus();
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

MouseTestView::~MouseTestView() {
    if (parent_ && subclassed_ && IsWindow(parent_)) {
        RemoveWindowSubclass(parent_, TestViewSubclassProc, kTestViewSubclassId);
    }
    subclassed_ = false;
    auto del = [](HFONT& f) {
        if (f) DeleteObject(f);
        f = nullptr;
    };
    del(fontTitle_);
    del(fontBody_);
    del(fontBold_);
    del(fontSmall_);
    del(fontBig_);
    del(fontKey_);
    del(fontKeySmall_);
    del(fontMono_);
}

void MouseTestView::SetDpi(UINT dpi) {
    if (dpi == dpi_) return;
    dpi_ = dpi ? dpi : 96;
    RecreateFonts();
    if (combo_) {
        SendMessageW(combo_, WM_SETFONT, (WPARAM)fontBody_, TRUE);
    }
    if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
}

void MouseTestView::RecreateFonts() {
    auto del = [](HFONT& f) {
        if (f) DeleteObject(f);
        f = nullptr;
    };
    del(fontTitle_);
    del(fontBody_);
    del(fontBold_);
    del(fontSmall_);
    del(fontBig_);
    del(fontKey_);
    del(fontKeySmall_);
    del(fontMono_);
    fontTitle_ = MakeFont(dpi_, 13, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontBody_ = MakeFont(dpi_, 10, FW_NORMAL, L"Microsoft YaHei UI");
    fontBold_ = MakeFont(dpi_, 10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontSmall_ = MakeFont(dpi_, 9, FW_NORMAL, L"Microsoft YaHei UI");
    fontBig_ = MakeFont(dpi_, 24, FW_SEMIBOLD, L"Segoe UI");
    fontKey_ = MakeFont(dpi_, 9, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontKeySmall_ = MakeFont(dpi_, 7, FW_NORMAL, L"Microsoft YaHei UI");
    fontMono_ = MakeFont(dpi_, 9, FW_NORMAL, L"Consolas");
}

void MouseTestView::EnsureControls() {
    if (combo_ || !parent_) return;
    combo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                             WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10,
                             200, parent_, (HMENU)(INT_PTR)kComboId,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(combo_, WM_SETFONT, (WPARAM)fontBody_, TRUE);
}

void MouseTestView::SetDevices(const std::vector<Device>& devices,
                               const std::wstring& selectedId) {
    bool changed = devices.size() != devices_.size() || selectedId != selectedId_;
    if (!changed) {
        for (size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].id != devices_[i].id || devices[i].name != devices_[i].name ||
                devices[i].online != devices_[i].online) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;
    devices_ = devices;
    selectedId_ = selectedId;
    // 键盘模式不维护鼠标目标句柄/下拉；快照刷新只更新设备列表，
    // 不能把键盘状态提示覆盖成鼠标的“正在测试/演示模式”文案。
    if (visible_ && mode_ == Mode::Mouse) {
        ReloadCombo();
        MatchTarget();
    }
}

void MouseTestView::ReloadCombo() {
    EnsureControls();
    if (!combo_) return;

    // Show() 先于 ui.cpp 的 SetDevices() 执行，首次进入时 targetName_ 还只是占位符；
    // 只有用户显式选过（targetChosen_）才算真实选择，否则按快照里选中的设备作为默认值。
    const std::wstring keep = targetName_;
    const bool keepIsAny = keep == kAnyMouseName;
    const bool keepIsReal = !keep.empty() && !(keepIsAny && !targetChosen_);

    SendMessageW(combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo_, CB_ADDSTRING, 0, (LPARAM)kAnyMouseName);
    int select = 0;
    int byName = -1;
    int nameCount = 0;
    std::wstring preferred;
    std::vector<std::wstring> mouseIds;
    for (size_t i = 0; i < devices_.size(); ++i) {
        const Device& d = devices_[i];
        if (d.kind != Kind::Mouse) continue;
        int idx = (int)SendMessageW(combo_, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
        if (idx < 0) continue;
        SendMessageW(combo_, CB_SETITEMDATA, idx, (LPARAM)(INT_PTR)(i + 1));
        mouseIds.push_back(d.id);
        if (keepIsReal && !keepIsAny && d.name == keep) {
            ++nameCount;
            byName = (int)mouseIds.size() - 1;
        }
        if (preferred.empty() && !selectedId_.empty() && d.id == selectedId_) {
            preferred = d.name;
        }
    }
    if (keepIsReal && !keepIsAny) {
        // id 优先；只有目标名唯一时才退回按名称匹配，重名设备不能猜。
        const int byId = MouseTargetIndexById(mouseIds, targetDeviceId_);
        if (byId >= 0) {
            select = byId + 1;
        } else if (nameCount == 1) {
            select = byName + 1;
        }
    } else if (!keepIsReal && !preferred.empty()) {
        int n = (int)SendMessageW(combo_, CB_GETCOUNT, 0, 0);
        for (int i = 1; i < n; ++i) {
            wchar_t buf[256] = {};
            SendMessageW(combo_, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf);
            if (preferred == buf) {
                select = i;
                break;
            }
        }
    }
    SendMessageW(combo_, CB_SETCURSEL, (WPARAM)select, 0);
    comboIndex_ = select;

    // 用户选过真实目标、但新快照里找不到：保留目标名并暂停统计。
    // 显式选“任意鼠标”不算丢失；设备重插后按 id/唯一名称自动恢复。
    targetLost_ = keepIsReal && !keepIsAny && select == 0;
    if (targetLost_) {
        lostTargetName_ = keep;
        // 下拉框暂时显示“任意鼠标”，但 targetName_ 保留原名供下次恢复。
        targetName_ = keep;
        return;
    }
    lostTargetName_.clear();
    wchar_t buf[256] = {};
    SendMessageW(combo_, CB_GETLBTEXT, (WPARAM)select, (LPARAM)buf);
    targetName_ = buf;
}

void MouseTestView::MatchTarget() {
    targetRaw_ = nullptr;
    targetMatched_ = false;
    inputPaused_ = false;
    engine_.SetTargetHandle(nullptr);

    // demo 设备没有真实 Raw Input 句柄：明确提示，不能显示“正在测试”。
    if (demo_) {
        targetLost_ = false;
        targetDeviceId_.clear();
        inputPaused_ = true;
        statusNote_ = L"演示模式 · 不统计真实输入";
        dirty_ = true;
        return;
    }
    if (targetLost_) {
        inputPaused_ = true;
        statusNote_ = Fmt(L"目标设备已断开：%s", lostTargetName_.c_str());
        dirty_ = true;
        return;
    }

    if (comboIndex_ <= 0) {
        targetMatched_ = true;
        targetDeviceId_.clear();
        statusNote_ = L"统计任意鼠标";
        dirty_ = true;
        return;
    }

    // 下拉项 data 存 devices_ 的索引 + 1（0 表示任意鼠标）。
    INT_PTR data = (INT_PTR)SendMessageW(combo_, CB_GETITEMDATA, (WPARAM)comboIndex_, 0);
    if (data <= 0 || data > (INT_PTR)devices_.size()) {
        inputPaused_ = true;
        statusNote_ = L"未找到目标设备，统计已暂停";
        dirty_ = true;
        return;
    }
    const Device& d = devices_[(size_t)data - 1];
    targetDeviceId_ = d.id;
    lostTargetName_ = d.name;

    std::wstring want = Lower(d.path.empty() ? d.basePath : d.path);
    size_t hash = want.find(L"#{");
    if (hash != std::wstring::npos) want = want.substr(0, hash);
    std::wstring vidpid = Fmt(L"vid_%04x&pid_%04x", d.vid, d.pid);

    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || !count) {
        inputPaused_ = true;
        statusNote_ = L"无法枚举 Raw Input 设备，统计已暂停";
        dirty_ = true;
        return;
    }
    std::vector<RAWINPUTDEVICELIST> list(count);
    UINT got = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (got == (UINT)-1) {
        inputPaused_ = true;
        statusNote_ = L"无法枚举 Raw Input 设备，统计已暂停";
        dirty_ = true;
        return;
    }

    HANDLE fallback = nullptr;
    for (UINT i = 0; i < got; ++i) {
        if (list[i].dwType != RIM_TYPEMOUSE) continue;
        UINT chars = 0;
        GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &chars);
        if (!chars) continue;
        std::vector<wchar_t> name(chars + 1, 0);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name.data(), &chars) <= 0) {
            continue;
        }
        std::wstring lower = Lower(name.data());
        bool vidpidMatch = !vidpid.empty() && lower.find(vidpid) != std::wstring::npos;
        bool pathMatch = !want.empty() && (lower.find(want) != std::wstring::npos ||
                                           want.find(lower) != std::wstring::npos);
        if (pathMatch) {
            targetRaw_ = list[i].hDevice;
            targetMatched_ = true;
            break;
        }
        if (vidpidMatch && !fallback) fallback = list[i].hDevice;
    }
    if (!targetMatched_ && fallback) {
        targetRaw_ = fallback;
        targetMatched_ = true;
    }
    engine_.SetTargetHandle(targetRaw_);
    if (targetMatched_) {
        statusNote_ = Fmt(L"正在测试：%s", d.name.c_str());
    } else {
        // 目标存在但句柄失效：暂停统计，不能把其它鼠标的输入算到该目标上。
        inputPaused_ = true;
        statusNote_ = L"未匹配到目标设备句柄，统计已暂停";
        engine_.SetTargetHandle(nullptr);
    }
    dirty_ = true;
}

void MouseTestView::UpdateComboVisibility() {
    if (!combo_) return;
    const bool show = !introHidden_ && visible_ && mode_ == Mode::Mouse;
    ShowWindow(combo_, show ? SW_SHOW : SW_HIDE);
}

void MouseTestView::SetChildrenHidden(bool hidden) {
    // 只由内容入场动画/主窗口调用；模式切换走 UpdateComboVisibility，
    // 不能把入场期间应该隐藏的下拉框提前显示出来。
    introHidden_ = hidden;
    UpdateComboVisibility();
}

void MouseTestView::Show(HWND parent, bool on) {
    parent_ = parent;
    if (on) {
        EnsureControls();
        RecreateFonts();
        if (combo_) SendMessageW(combo_, WM_SETFONT, (WPARAM)fontBody_, TRUE);
        visible_ = true;
        // 切到日志/测试再回来时保留本轮统计；只有「重置」或切换目标设备才清零。
        engine_.SetClickWindow(windowSec_);
        // 新一次进入页面不继承上一次的失焦去重状态。
        kbBlurred_ = false;
        dirty_ = true;
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(parent_, &p);
        cursor_ = p;
        cursorValid_ = true;
        if (mode_ == Mode::Mouse) {
            ReloadCombo();
            MatchTarget();
        }
        // 鼠标在 demo 下没有真实目标，不注册；键盘测试始终使用真实输入。
        bool mouseOk = !demo_;
        bool keyboardOk = true;
        if (parent_) {
            if (!demo_) {
                RAWINPUTDEVICE mouse{};
                mouse.usUsagePage = 0x01;
                mouse.usUsage = 0x02;
                mouse.dwFlags = 0;  // 仅前台窗口，避免全局拦截
                mouse.hwndTarget = parent_;
                mouseOk = RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) == TRUE;
            }
            RAWINPUTDEVICE kb{};
            kb.usUsagePage = 0x01;
            kb.usUsage = 0x06;
            kb.dwFlags = 0;
            kb.hwndTarget = parent_;
            keyboardOk = RegisterRawInputDevices(&kb, 1, sizeof(kb)) == TRUE;
        }
        rawRegistered_ = mouseOk || keyboardOk;
        mouseRawOk_ = !demo_ && mouseOk;
        keyboardRawOk_ = keyboardOk;
        if (!rawRegistered_) statusNote_ = L"Raw Input 注册失败，无法统计";
        else if (mode_ == Mode::Keyboard && !keyboardOk) statusNote_ = L"键盘 Raw Input 注册失败";
        else if (mode_ == Mode::Keyboard) {
            statusNote_ = demo_ ? L"演示模式 · 键盘测试使用真实输入"
                                : L"键盘测试 · ANSI 104 · 真实输入";
        } else if (!demo_ && !mouseOk) {
            // 键盘注册成功不代表鼠标可用；两个 RID 必须分别判断。
            inputPaused_ = true;
            statusNote_ = L"鼠标 Raw Input 注册失败，统计已暂停";
        }
        // 新一次进入页面不继承上一次的入场隐藏状态；下拉框按模式重新决定。
        introHidden_ = false;
        UpdateComboVisibility();
        QueryPerformanceFrequency((LARGE_INTEGER*)&qpcFreq_);
        if (qpcFreq_ <= 0) qpcFreq_ = 1000000;
        if (parent_ && !subclassed_) {
            subclassed_ = SetWindowSubclass(parent_, TestViewSubclassProc, kTestViewSubclassId,
                                            reinterpret_cast<DWORD_PTR>(this)) != FALSE;
        }
        dirty_ = true;
    } else {
        visible_ = false;
        hover_ = 0;
        kbHover_ = 0;
        trackLeave_ = false;
        pressed_ = -1;
        cursorValid_ = false;
        // 离开页面（切导航/隐藏到托盘）时清理键盘按住态：之后键盘消息不再
        // 转发到这里，物理 KeyUp 会丢失，不能把按下色永久留在键盘板上。
        if (mode_ == Mode::Keyboard) {
            keyboard_.ReleaseAll();
            kbBlurred_ = false;
        }
        UpdateComboVisibility();
        if (rawRegistered_ && parent_) {
            RAWINPUTDEVICE rid[2]{};
            rid[0].usUsagePage = 0x01;
            rid[0].usUsage = 0x02;
            rid[0].dwFlags = RIDEV_REMOVE;
            rid[1].usUsagePage = 0x01;
            rid[1].usUsage = 0x06;
            rid[1].dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
            rawRegistered_ = false;
            mouseRawOk_ = false;
            keyboardRawOk_ = false;
        }
    }
    if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
}

void MouseTestView::SetMode(Mode mode) {
    if (mode_ == mode) return;
    const Mode previous = mode_;
    mode_ = mode;
    hover_ = 0;
    kbHover_ = 0;
    kbBlurred_ = false;
    if (previous == Mode::Keyboard) {
        // 切走后键盘消息不再转发；释放按住态，避免切回时出现永久按下的键。
        keyboard_.ReleaseAll();
    }
    if (mode_ == Mode::Mouse) {
        UpdateComboVisibility();
        ReloadCombo();
        MatchTarget();
        if (visible_ && !demo_ && !mouseRawOk_) {
            // MatchTarget 不能感知 Raw Input 注册失败，这里补一次暂停状态。
            inputPaused_ = true;
            statusNote_ = L"鼠标 Raw Input 注册失败，统计已暂停";
        }
    } else {
        UpdateComboVisibility();
        // 键盘测试不使用鼠标目标状态；失焦/暂停只属于鼠标模式。
        inputPaused_ = false;
        statusNote_ = !keyboardRawOk_ ? L"键盘 Raw Input 未连接"
                      : (demo_ ? L"演示模式 · 键盘测试使用真实输入"
                               : L"键盘测试 · ANSI 104 · 真实输入");
    }
    // 两种模式的控件/画布矩形不同，切换后必须重新布局。
    if (rcContent_.right > rcContent_.left) Layout(rcContent_);
    dirty_ = true;
    if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
}

void MouseTestView::ResetAll() {
    engine_.ResetAll();
    engine_.SetClickWindow(windowSec_);
    jumpPoints_.clear();
    lastTrajPoint_ = POINT{0, 0};
    hasLastTraj_ = false;
    dirty_ = true;
}

void MouseTestView::Layout(const RECT& content) {
    rcContent_ = content;
    const int m = Scale(20);
    const int gap = Scale(12);

    int left = content.left + m;
    int right = content.right - m;
    // 48px 内容头（规格 3.3）：标题由 MainWindow::PaintContentHeader 绘制。
    // 模式切换/目标/操作按钮放在内容头下方的独立工具行，避免压住标题。
    const int headerH = Scale(48);
    rcHeader_ = {content.left, content.top, content.right, content.top + headerH};
    const int toolbarH = Scale(30);
    const int top = content.top + headerH + Scale(9);

    rcTop_ = {left, top, right, top + Scale(30)};
    rcModeMouse_ = {left, top, left + Scale(82), top + Scale(30)};
    rcModeKeyboard_ = {rcModeMouse_.right, top, rcModeMouse_.right + Scale(82), top + Scale(30)};

    if (mode_ == Mode::Keyboard) {
        rcKbClear_ = {right - Scale(76), top, right, top + Scale(30)};
        rcKbLayout_ = {rcKbClear_.left - Scale(8) - Scale(104), top, rcKbClear_.left - Scale(8),
                       top + Scale(30)};
        rcKbApm_ = {rcKbLayout_.left - Scale(10) - Scale(196), top,
                    rcKbLayout_.left - Scale(10), top + Scale(30)};
        const int boardTop = content.top + headerH + toolbarH + Scale(21);
        const int contentBottom = content.bottom - Scale(16);
        // 键盘板不能只按宽度算：超宽/矮窗口会把 23.5:6 的板按宽度放大到
        // 占满高度，把日志/诊断面板推出内容区。这里同时给面板预留最小高度。
        const int panelReserve = Scale(180);
        int maxBoardH = contentBottom - Scale(12) - panelReserve - boardTop;
        if (maxBoardH < Scale(90)) maxBoardH = Scale(90);
        const float unitW = (float)(right - left) / 23.5f;
        const float unitH = (float)maxBoardH / 6.0f;
        kbUnit_ = std::min(unitW, unitH);
        if (kbUnit_ < 1.0f) kbUnit_ = 1.0f;
        const int boardW = (int)std::lround(23.5f * kbUnit_);
        const int boardH = (int)std::lround(6.0f * kbUnit_);
        const int boardLeft = left + ((right - left) - boardW) / 2;
        rcKbBoard_ = {boardLeft, boardTop, boardLeft + boardW, boardTop + boardH};
        int panelTop = rcKbBoard_.bottom + Scale(12);
        int panelBottom = contentBottom;
        // 面板底边绝不越过内容区；高度不足时压缩面板而不是画进 footer。
        if (panelBottom - panelTop < Scale(120)) {
            panelBottom = std::min(contentBottom, panelTop + Scale(120));
        }
        rcKbPanel_ = {left, panelTop, right, panelBottom};
        rcKbLogTab_ = {rcKbPanel_.left + Scale(16), panelTop + Scale(6),
                       rcKbPanel_.left + Scale(16) + Scale(58), panelTop + Scale(32)};
        rcKbDiagTab_ = {rcKbLogTab_.right + Scale(4), panelTop + Scale(6),
                        rcKbLogTab_.right + Scale(4) + Scale(58), panelTop + Scale(32)};
        // 只按当前模式隐藏下拉框；不要写成 SetChildrenHidden(true)，
        // 那会把“入场动画隐藏”状态误置为真。
        UpdateComboVisibility();
        return;
    }

    rcReset_ = {right - Scale(76), top, right, top + Scale(30)};
    rcWin10_ = {rcReset_.left - Scale(12) - Scale(62), top, rcReset_.left - Scale(12),
                top + Scale(30)};
    rcWin5_ = {rcWin10_.left - Scale(4) - Scale(82), top, rcWin10_.left - Scale(4),
               top + Scale(30)};
    rcRedetect_ = {rcWin5_.left - Scale(24) - Scale(84), top, rcWin5_.left - Scale(24),
                   top + Scale(30)};
    rcCombo_ = {rcRedetect_.left - Scale(8) - Scale(200), top, rcRedetect_.left - Scale(8),
                top + Scale(30)};

    int cardsTop = content.top + headerH + toolbarH + Scale(19);
    int leftW = (right - left - gap) * 55 / 100;
    rcBtnCard_ = {left, cardsTop, left + leftW, cardsTop + Scale(300)};
    rcStatsCard_ = {left, rcBtnCard_.bottom + gap, left + leftW,
                    rcBtnCard_.bottom + gap + Scale(120)};
    rcTipsCard_ = {left, rcStatsCard_.bottom + gap, left + leftW, content.bottom - Scale(16)};
    if (rcTipsCard_.bottom < rcTipsCard_.top + Scale(80)) {
        rcTipsCard_.bottom = rcTipsCard_.top + Scale(80);
    }

    int rx = rcBtnCard_.right + gap;
    int rw = right - rx;
    rcPollCard_ = {rx, cardsTop, rx + rw, cardsTop + Scale(150)};
    rcClickCard_ = {rx, rcPollCard_.bottom + gap, rx + rw, rcPollCard_.bottom + gap + Scale(150)};
    rcTrajCard_ = {rx, rcClickCard_.bottom + gap, rx + rw, content.bottom - Scale(16)};
    if (rcTrajCard_.bottom < rcTrajCard_.top + Scale(120)) {
        rcTrajCard_.bottom = rcTrajCard_.top + Scale(120);
    }

    rcTrajPlot_ = {rcTrajCard_.left + Scale(16), rcTrajCard_.top + Scale(36),
                   rcTrajCard_.right - Scale(16), rcTrajCard_.bottom - Scale(40)};
    rcClear_ = {rcTrajCard_.right - Scale(86), rcTrajCard_.bottom - Scale(32),
                rcTrajCard_.right - Scale(16), rcTrajCard_.bottom - Scale(8)};

    // 鼠标图形区域：DrawButtonsCard 用它绘图，HandleRawInput 用它判定按键/滚轮是否计数。
    rcShape_ = {rcBtnCard_.left + Scale(20), rcBtnCard_.top + Scale(42),
                rcBtnCard_.left + (rcBtnCard_.right - rcBtnCard_.left) * 45 / 100,
                rcBtnCard_.bottom - Scale(16)};

    if (combo_) {
        MoveWindow(combo_, rcCombo_.left, rcCombo_.top, rcCombo_.right - rcCombo_.left,
                   Scale(240), TRUE);
    }
}

void MouseTestView::Paint(HDC dc) {
    // 不再用纯色铺满内容区：主窗口已经画好毛玻璃渐变底，
    // 测试页的卡片应与设备页/日志页共用同一层背景。
    if (mode_ == Mode::Keyboard) {
        PaintKeyboard(dc);
        return;
    }
    DrawTopBar(dc);
    MouseTestSnapshot snap = engine_.Snapshot();
    DrawButtonsCard(dc, snap);
    DrawStatsCard(dc, snap);
    DrawTipsCard(dc);
    DrawPollingCard(dc, snap);
    DrawClickCard(dc, snap);
    DrawTrajectoryCard(dc, snap);
}

void MouseTestView::DrawTopBar(HDC dc) {
    // 模式分段固定在左侧；鼠标目标与操作按钮仍右对齐。
    DrawSegmented(dc,
                  RECT{rcModeMouse_.left, rcModeMouse_.top, rcModeKeyboard_.right,
                       rcModeKeyboard_.bottom},
                  L"鼠标", L"键盘", 0);
    // 目标下拉框与操作按钮右对齐放进 48px 内容头；左上是 MainWindow 画的
    // “鼠标测试”标题，状态提示占据标题与下拉框之间的剩余空间。
    RECT label{rcCombo_.left - Scale(38), rcTop_.top, rcCombo_.left - Scale(8), rcTop_.bottom};
    DrawTextIn(dc, L"目标", label, fontBody_, kSub,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    auto button = [&](const RECT& rc, const wchar_t* text, int hoverId, bool primary) {
        DrawButton(dc, rc, text, primary, hover_ == hoverId);
    };
    button(rcRedetect_, L"重新检测", 1, false);
    DrawSegmented(dc, RECT{rcWin5_.left, rcWin5_.top, rcWin10_.right, rcWin10_.bottom}, L"5s",
                  L"10s", windowSec_ == 5 ? 0 : 1);
    button(rcReset_, L"重置", 4, false);
    // 状态提示画在内容头副标题（MainWindow::PaintContentHeader），这里不再重复。
}

void MouseTestView::PaintKeyboard(HDC dc) {
    DrawKeyboardTopBar(dc);
    DrawKeyboardBoard(dc);
    DrawKeyboardPanel(dc);
}

void MouseTestView::DrawKeyboardTopBar(HDC dc) {
    DrawSegmented(dc,
                  RECT{rcModeMouse_.left, rcModeMouse_.top, rcModeKeyboard_.right,
                       rcModeKeyboard_.bottom},
                  L"鼠标", L"键盘", 1);

    // 三态图例：按下 / 已释放 / 未测试。
    // 右边界让给 APM/ANSI/清空；空间不足时丢图例文字，避免压到 APM 读数。
    int lx = rcModeKeyboard_.right + Scale(16);
    const int ly = rcTop_.top + Scale(9);
    const int legendLimit = rcKbApm_.left - Scale(8);
    auto legend = [&](COLORREF fill, COLORREF border, const wchar_t* text) {
        if (lx + Scale(12) > legendLimit) return;
        RECT sw{lx, ly + Scale(2), lx + Scale(12), ly + Scale(14)};
        FillRounded(dc, sw, Scale(3), fill, border);
        const int textLeft = sw.right + Scale(6);
        const int textRight = std::min(textLeft + Scale(46), legendLimit);
        if (textRight > textLeft) {
            RECT tr{textLeft, rcTop_.top, textRight, rcTop_.bottom};
            DrawTextIn(dc, text, tr, fontSmall_, kMuted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        lx = std::max((int)sw.right, textRight) + Scale(8);
    };
    legend(kAccent, kAccent, L"按下");
    legend(kAccentSoft, kAccent, L"已释放");
    legend(kCard, kBorder, L"未测试");

    // APM：最近 5 秒有效按下 ×12；右侧画最近 30 秒每秒按下次数。
    const uint64_t now = (uint64_t)GetTickCount64();
    const int apm = keyboard_.Apm(now);
    RECT apmText{rcKbApm_.left, rcTop_.top, rcKbApm_.left + Scale(74), rcTop_.bottom};
    DrawTextIn(dc, Fmt(L"APM %d", apm), apmText, fontBold_, kText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const auto hist = keyboard_.ApmHistory(now, 30);
    if (!hist.empty()) {
        int maxV = 1;
        for (int v : hist) maxV = std::max(maxV, v);
        RECT plot{apmText.right + Scale(2), rcTop_.top + Scale(9), rcKbApm_.right,
                  rcTop_.bottom - Scale(8)};
        POINT pts[30] = {};
        const int n = (int)std::min<size_t>(hist.size(), 30);
        const int w = std::max(1, (int)(plot.right - plot.left));
        const int h = std::max(1, (int)(plot.bottom - plot.top));
        for (int i = 0; i < n; ++i) {
            pts[i].x = plot.left + (n <= 1 ? 0 : w * i / (n - 1));
            pts[i].y = plot.bottom - (int)((long long)hist[(size_t)i] * h / maxV);
        }
        HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), kAccent);
        HGDIOBJ old = SelectObject(dc, pen);
        Polyline(dc, pts, n);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    DrawBadge(dc, rcKbLayout_, L"ANSI 104 键", fontSmall_, kHover, kSub);
    DrawButton(dc, rcKbClear_, L"清空", false, kbHover_ == 12);
}

RECT MouseTestView::KbKeyRect(int index) const {
    const auto& d = keyboard_.Def(index);
    const int gap = std::max(Scale(2), (int)std::lround(kbUnit_ * 0.08f));
    RECT rc{};
    rc.left = rcKbBoard_.left + (int)std::lround(d.x * kbUnit_) + gap / 2;
    rc.top = rcKbBoard_.top + (int)std::lround(d.y * kbUnit_) + gap / 2;
    rc.right = rcKbBoard_.left + (int)std::lround((d.x + d.w) * kbUnit_) - gap / 2;
    rc.bottom = rcKbBoard_.top + (int)std::lround((d.y + d.h) * kbUnit_) - gap / 2;
    return rc;
}

void MouseTestView::DrawKeyboardBoard(HDC dc) {
    GlassPanel(dc, rcKbBoard_, Scale(10));
    for (int i = 0; i < keyboard_.KeyCount(); ++i) {
        const auto& def = keyboard_.Def(i);
        const auto state = keyboard_.State(i);
        const RECT rc = KbKeyRect(i);
        COLORREF fill = kCard;
        COLORREF border = kBorder;
        COLORREF fg = kSub;
        if (state == KeyState::Down) {
            fill = kAccent;
            border = kAccent;
            fg = RGB(255, 255, 255);
        } else if (state == KeyState::Up) {
            fill = kAccentSoft;
            border = kAccent;
            fg = kAccent;
        }
        FillRounded(dc, rc, Scale(5), fill, border);
        const std::wstring label = def.label;
        const std::wstring shift = def.shiftLabel;
        if (label.empty()) {
            // 空格键：画一条居中的短横，而不是留空。
            RECT bar{rc.left + (rc.right - rc.left) / 5, rc.top + (rc.bottom - rc.top) / 2,
                     rc.right - (rc.right - rc.left) / 5,
                     rc.top + (rc.bottom - rc.top) / 2 + std::max(1, Scale(1))};
            HBRUSH b = CreateSolidBrush(fg);
            FillRect(dc, &bar, b);
            DeleteObject(b);
            continue;
        }
        if (!shift.empty()) {
            RECT top{rc.left + Scale(4), rc.top + Scale(1), rc.right - Scale(3),
                     rc.top + (rc.bottom - rc.top) / 2};
            DrawTextIn(dc, shift, top, fontKeySmall_, fg,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT bottom{rc.left + Scale(4), top.bottom, rc.right - Scale(3), rc.bottom - Scale(1)};
            DrawTextIn(dc, label, bottom, fontKey_, fg,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (label.size() >= 5 || (label.size() >= 4 && def.w <= 1.0f)) {
            DrawTextIn(dc, label, rc, fontKeySmall_, fg,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            DrawTextIn(dc, label, rc, fontKey_, fg,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

void MouseTestView::DrawKeyboardPanel(HDC dc) {
    GlassPanel(dc, rcKbPanel_, Scale(8));
    const bool logTab = kbPanelTab_ == 0;
    auto tab = [&](const RECT& rc, const wchar_t* text, bool active, int hoverId) {
        if (active) {
            FillRounded(dc, rc, Scale(5), kAccentSoft, kAccentSoft);
        } else if (kbHover_ == hoverId) {
            FillRounded(dc, rc, Scale(5), kHover, kHover);
        }
        DrawTextIn(dc, text, rc, fontBold_, active ? kAccent : kSub,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (active) {
            RECT line{rc.left + Scale(14), rc.bottom - Scale(3), rc.right - Scale(14),
                      rc.bottom - Scale(1)};
            FillRounded(dc, line, Scale(1), kAccent, kAccent);
        }
    };
    tab(rcKbLogTab_, L"日志", logTab, 13);
    tab(rcKbDiagTab_, L"诊断", !logTab, 14);

    RECT body{rcKbPanel_.left + Scale(16), rcKbPanel_.top + Scale(40),
              rcKbPanel_.right - Scale(16), rcKbPanel_.bottom - Scale(12)};
    if (logTab) {
        const int rowH = Scale(22);
        int y = body.top;
        RECT head{body.left, y, body.right, y + Scale(18)};
        DrawTextIn(dc, L"时间", {head.left, head.top, head.left + Scale(120), head.bottom},
                   fontSmall_, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(dc, L"按键", {head.left + Scale(126), head.top,
                                 head.left + Scale(226), head.bottom},
                   fontSmall_, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(dc, L"事件", {head.left + Scale(232), head.top, head.right, head.bottom},
                   fontSmall_, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += Scale(22);
        if (kbLog_.empty()) {
            RECT empty{body.left, body.top + Scale(24), body.right, body.bottom};
            DrawTextIn(dc, L"按下键盘开始测试；按住多个键可查看 N 键无冲", empty, fontBody_,
                       kMuted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            int shown = 0;
            // 底部还有一行说明文字；行区域必须避开它，否则最后一行会被覆盖。
            const int rowBottom = body.bottom - Scale(20);
            for (int i = (int)kbLog_.size() - 1; i >= 0 && shown < 12; --i, ++shown) {
                const auto& e = kbLog_[(size_t)i];
                if (y + rowH > rowBottom) break;
                RECT tr{body.left, y, body.left + Scale(120), y + rowH};
                RECT kr{body.left + Scale(126), y, body.left + Scale(226), y + rowH};
                RECT er{body.left + Scale(232), y, body.right, y + rowH};
                DrawTextIn(dc, e.time, tr, fontMono_, kMuted,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                DrawTextIn(dc, e.key, kr, fontBody_, kText,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                COLORREF ec = e.event == L"KeyDown" ? kAccent
                              : (e.event == L"Blur" ? kAmber : kSub);
                DrawTextIn(dc, e.event, er, fontSmall_, ec,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                y += rowH;
            }
        }
        RECT note{body.left, body.bottom - Scale(18), body.right, body.bottom};
        DrawTextIn(dc, L"页面按钮不计入；Win / Menu 可能被系统占用", note, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    // 诊断：2x2 信息块。卡片高度严格按可用空间计算，不能设固定下限，
    // 否则矮窗口会把第二行画到面板/说明文字之外。
    const int gap = Scale(12);
    const int cw = (body.right - body.left - gap) / 2;
    const int cardsBottom = body.bottom - Scale(20);
    const int ch = std::max(0, (int)(cardsBottom - body.top - gap) / 2);
    RECT cards[4] = {
        {body.left, body.top, body.left + cw, body.top + ch},
        {body.left + cw + gap, body.top, body.right, body.top + ch},
        {body.left, body.top + ch + gap, body.left + cw, body.top + ch + gap + ch},
        {body.left + cw + gap, body.top + ch + gap, body.right,
         body.top + ch + gap + ch},
    };
    auto infoCard = [&](const RECT& rc, const std::wstring& title, const std::wstring& value,
                        const std::wstring& detail, COLORREF valueColor) {
        const int h = rc.bottom - rc.top;
        if (h < Scale(28)) return;
        FillRounded(dc, rc, Scale(6), kHover, kBorder);
        if (h < Scale(58)) {
            // 矮面板：只保留标题与数值两行，不画说明，避免文字被裁切/溢出卡片。
            const int mid = rc.top + h / 2;
            RECT t{rc.left + Scale(10), rc.top + Scale(2), rc.right - Scale(10), mid};
            DrawTextIn(dc, title, t, fontSmall_, kMuted,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT v{rc.left + Scale(10), mid, rc.right - Scale(10), rc.bottom - Scale(2)};
            DrawTextIn(dc, value, v, fontBold_, valueColor,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return;
        }
        RECT t{rc.left + Scale(10), rc.top + Scale(6), rc.right - Scale(10),
               rc.top + Scale(24)};
        DrawTextIn(dc, title, t, fontSmall_, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT v{rc.left + Scale(10), rc.top + Scale(24), rc.right - Scale(10),
               rc.top + Scale(48)};
        DrawTextIn(dc, value, v, fontBold_, valueColor,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT d{rc.left + Scale(10), rc.top + Scale(48), rc.right - Scale(10),
               rc.bottom - Scale(6)};
        DrawTextIn(dc, detail, d, fontSmall_, kSub, DT_LEFT | DT_TOP | DT_WORDBREAK);
    };

    const int down = keyboard_.DownCount();
    const int maxDown = keyboard_.MaxSimultaneous();
    infoCard(cards[0], L"N 键无冲",
             Fmt(L"当前 %d · 最大 %d", down, maxDown),
             maxDown >= 6 ? L"已同时按下 6 键以上，键盘支持多键无冲"
                          : L"同时按住更多键以测试无冲上限",
             maxDown >= 6 ? kGreen : kText);

    const int chatter = keyboard_.ChatterCount();
    infoCard(cards[1], L"连击诊断", Fmt(L"疑似连击 %d 次", chatter),
             L"同一键两次按下间隔 < 50 ms 记为疑似连击",
             chatter == 0 ? kGreen : kAmber);

    const int last = keyboard_.LastKeyIndex();
    const int interval = last >= 0 ? keyboard_.LastIntervalMs(last) : -1;
    infoCard(cards[2], L"最近按键",
             last >= 0 ? keyboard_.Def(last).name : L"—",
             interval >= 0 ? Fmt(L"同键间隔 %d ms", interval) : L"还没有重复按下同一键",
             kText);

    infoCard(cards[3], L"覆盖与状态",
             Fmt(L"%d / %d 键已测试", keyboard_.TestedCount(), keyboard_.KeyCount()),
             keyboardRawOk_ ? L"Raw Input 已连接；自动重复只保留一条 KeyDown"
                            : L"键盘 Raw Input 未连接",
             keyboardRawOk_ ? kGreen : kRed);

    RECT note{body.left, body.bottom - Scale(18), body.right, body.bottom};
    DrawTextIn(dc, L"Win / Menu 可能被系统占用；本页只读测试，不写入设备", note, fontSmall_,
               kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

std::wstring MouseTestView::NowClockMs() const {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Fmt(L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void MouseTestView::AppendKbLog(const wchar_t* key, const wchar_t* event) {
    KbLogEntry e;
    e.time = NowClockMs();
    e.key = key ? key : L"—";
    e.event = event ? event : L"—";
    kbLog_.push_back(std::move(e));
    if (kbLog_.size() > (size_t)keyboardtest::KeyboardTestEngine::kLogCapacity) {
        kbLog_.erase(kbLog_.begin());
    }
    dirty_ = true;
}

void MouseTestView::ClearKeyboard() {
    keyboard_.Reset();
    kbLog_.clear();
    lastApm_ = -1;
    kbHover_ = 0;
    kbBlurred_ = false;
    dirty_ = true;
    if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
}

void MouseTestView::DrawCard(HDC dc, const RECT& rc, const std::wstring& title) const {
    GlassPanel(dc, rc, Scale(8));
    DrawTextIn(dc, title, RECT{rc.left + Scale(16), rc.top + Scale(8), rc.right - Scale(16),
                               rc.top + Scale(30)},
               fontBold_, kSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void MouseTestView::DrawButtonsCard(HDC dc, const MouseTestSnapshot& s) {
    DrawCard(dc, rcBtnCard_, L"按键与滚轮");

    DrawMouseShape(dc, rcShape_);

    int x = rcShape_.right + Scale(8);
    int right = rcBtnCard_.right - Scale(16);
    // buttonDown[] 与 MouseButtonName() 都按 RAWMOUSE 的 BUTTON_1..5 排列：
    // 0=左键、1=右键、2=中键、3=后退、4=前进（微软把 BUTTON_2/3 定义为右/中键）。
    // DrawMouseShape 内部的 pressed_ 用图形顺序 0=左、1=中、2=右、3=后退、4=前进，
    // 两套编号不同，不能互相套用。
    int y = rcBtnCard_.top + Scale(44);
    for (int i = 0; i < 5; ++i) {
        RECT label{x, y, x + Scale(54), y + Scale(24)};
        DrawTextIn(dc, MouseButtonName(i), label, fontBody_, kSub,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        std::wstring n = std::to_wstring(s.buttonDown[i]);
        RECT val{x + Scale(54), y - Scale(2), right, y + Scale(26)};
        DrawTextIn(dc, n, val, fontBold_, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += Scale(28);
    }

    RECT dbl{x, y + Scale(4), right, y + Scale(28)};
    std::wstring dblText = Fmt(L"双击 %llu 次", (unsigned long long)s.doubleClicks);
    if (s.lastDoubleClickMs >= 0) dblText += Fmt(L" · 最近 %d ms", s.lastDoubleClickMs);
    DrawTextIn(dc, dblText, dbl, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT wheel{x, y + Scale(30), right, y + Scale(54)};
    std::wstring wheelText = Fmt(L"滚轮 上 %llu / 下 %llu · 最快 %.0f 格/秒",
                                 (unsigned long long)s.wheelUp,
                                 (unsigned long long)s.wheelDown, s.wheelPerSec);
    DrawTextIn(dc, wheelText, wheel, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (s.badDoubleClicks > 0) {
        // 双击/故障是五个按键的汇总数据，措辞不绑定具体按键，徽标横跨整张卡片。
        RECT warn{rcBtnCard_.left + Scale(16), rcBtnCard_.bottom - Scale(36),
                  rcBtnCard_.right - Scale(16), rcBtnCard_.bottom - Scale(12)};
        FillRounded(dc, warn, Scale(6), RGB(255, 247, 230), RGB(255, 247, 230));
        std::wstring text =
            Fmt(L"疑似故障双击 %llu 次", (unsigned long long)s.badDoubleClicks);
        if (s.minBadDoubleClickMs >= 0) text += Fmt(L"，最短间隔 %d ms", s.minBadDoubleClickMs);
        text += L"，建议再测 2–3 次确认";
        DrawTextIn(dc, text, warn, fontSmall_, kAmber,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void MouseTestView::DrawMouseShape(HDC dc, const RECT& rc) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int bodyW = std::min(w - Scale(20), Scale(150));
    // 底部还要放一行 9pt 提示文字（约 12–13px），比原来的 8pt 多留 4px 余量，
    // 否则提示会压在卡片下边框上被裁掉。
    int bodyH = std::min(h - Scale(46), Scale(210));
    RECT body{rc.left + (w - bodyW) / 2, rc.top + (h - bodyH) / 2,
              rc.left + (w - bodyW) / 2 + bodyW, rc.top + (h - bodyH) / 2 + bodyH};

    FillRounded(dc, body, Scale(18), RGB(252, 252, 253), RGB(201, 205, 213));
    int split = body.left + bodyW / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(201, 205, 213));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, split, body.top + Scale(6), nullptr);
    LineTo(dc, split, body.top + bodyH / 3);
    SelectObject(dc, old);
    DeleteObject(pen);

    ULONGLONG now = GetTickCount64();
    bool flash = pressed_ >= 0 && now - lastPressTick_ < 160;
    COLORREF leftFill = (flash && pressed_ == 0) ? kAccentSoft : RGB(252, 252, 253);
    COLORREF rightFill = (flash && pressed_ == 2) ? kAccentSoft : RGB(252, 252, 253);
    RECT leftHalf{body.left + Scale(2), body.top + Scale(2), split - Scale(1),
                  body.top + bodyH / 3};
    RECT rightHalf{split + Scale(1), body.top + Scale(2), body.right - Scale(2),
                   body.top + bodyH / 3};
    FillRounded(dc, leftHalf, Scale(14), leftFill, leftFill);
    FillRounded(dc, rightHalf, Scale(14), rightFill, rightFill);

    int wheelW = Scale(22);
    int wheelH = Scale(46);
    RECT wheel{split - wheelW / 2, body.top + Scale(24), split + wheelW / 2,
               body.top + Scale(24) + wheelH};
    COLORREF wheelColor = (flash && pressed_ == 1) ? kAccent : RGB(52, 58, 66);
    FillRounded(dc, wheel, wheelW / 2, wheelColor, wheelColor);

    // 侧键
    RECT back{body.left + Scale(4), body.top + Scale(58), body.left + Scale(10),
              body.top + Scale(86)};
    RECT fwd{body.left + Scale(4), body.top + Scale(92), body.left + Scale(10),
             body.top + Scale(120)};
    FillRounded(dc, back, Scale(3), (flash && pressed_ == 3) ? kAccent : RGB(214, 217, 222),
              RGB(214, 217, 222));
    FillRounded(dc, fwd, Scale(3), (flash && pressed_ == 4) ? kAccent : RGB(214, 217, 222),
              RGB(214, 217, 222));

    RECT hint{rc.left, body.bottom + Scale(6), rc.right, rc.bottom};
    DrawTextIn(dc, L"在鼠标图形上点击 / 滚动", hint, fontSmall_, kMuted,
               DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MouseTestView::DrawStatsCard(HDC dc, const MouseTestSnapshot& s) {
    DrawCard(dc, rcStatsCard_, L"按键统计");
    int inner = rcStatsCard_.left + Scale(16);
    int width = (rcStatsCard_.right - rcStatsCard_.left - Scale(32)) / 5;
    for (int i = 0; i < 5; ++i) {
        RECT label{inner + width * i, rcStatsCard_.top + Scale(34), inner + width * (i + 1),
                   rcStatsCard_.top + Scale(54)};
        DrawTextIn(dc, MouseButtonName(i), label, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT val{inner + width * i, rcStatsCard_.top + Scale(54), inner + width * (i + 1),
                 rcStatsCard_.top + Scale(84)};
        DrawTextIn(dc, std::to_wstring(s.buttonDown[i]), val, fontTitle_, kText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 双击/故障计数是五个按键的汇总，不能挂在「左键」列下；这里用整行跨度显示。
    const bool bad = s.badDoubleClicks > 0;
    std::wstring state = bad
                             ? Fmt(L"疑似故障双击 %llu 次", (unsigned long long)s.badDoubleClicks)
                             : L"各键计数正常";
    if (bad && s.minBadDoubleClickMs >= 0) {
        state += Fmt(L"，最短间隔 %d ms", s.minBadDoubleClickMs);
    }
    RECT row{inner, rcStatsCard_.top + Scale(84), rcStatsCard_.right - Scale(16),
             rcStatsCard_.bottom - Scale(8)};
    DrawTextIn(dc, state, row, fontSmall_, bad ? kAmber : kGreen,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MouseTestView::DrawTipsCard(HDC dc) {
    DrawCard(dc, rcTipsCard_, L"测试说明");
    const wchar_t* lines[] = {
        L"在左侧鼠标图形内点击/滚动；页面按钮不计入统计",
        L"双击：两次按下 ≤500 ms；间隔 <80 ms 记为疑似故障",
        L"回报率：取最近 4096 个移动报文的中位间隔",
        L"点击速度：只统计左键，可选 5s / 10s 窗口",
        L"轨迹：单次 |dx|+|dy| > 200 记为跳点",
    };
    int y = rcTipsCard_.top + Scale(34);
    for (const wchar_t* line : lines) {
        if (y + Scale(20) > rcTipsCard_.bottom - Scale(8)) break;
        RECT r{rcTipsCard_.left + Scale(16), y, rcTipsCard_.right - Scale(16),
               y + Scale(20)};
        DrawTextIn(dc, line, r, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += Scale(22);
    }
}

void MouseTestView::DrawPollingCard(HDC dc, const MouseTestSnapshot& s) {
    DrawCard(dc, rcPollCard_, L"回报率");
    RECT hz{rcPollCard_.left + Scale(16), rcPollCard_.top + Scale(38),
            rcPollCard_.left + Scale(240), rcPollCard_.top + Scale(86)};
    std::wstring hzText = s.pollingSamples ? Fmt(L"%.0f Hz", s.pollingHz) : L"-- Hz";
    DrawTextIn(dc, hzText, hz, fontBig_, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    COLORREF badgeBg = kHover;
    COLORREF badgeFg = kMuted;
    if (s.pollingSamples) {
        if (wcscmp(s.stability, L"稳定") == 0) {
            badgeBg = RGB(230, 255, 235);
            badgeFg = kGreen;
        } else if (wcscmp(s.stability, L"一般") == 0) {
            badgeBg = RGB(255, 247, 230);
            badgeFg = kAmber;
        } else if (wcscmp(s.stability, L"不稳定") == 0) {
            badgeBg = RGB(255, 241, 240);
            badgeFg = kRed;
        }
    }
    RECT badge{rcPollCard_.right - Scale(88), rcPollCard_.top + Scale(12),
               rcPollCard_.right - Scale(16), rcPollCard_.top + Scale(34)};
    DrawBadge(dc, badge, s.stability, fontSmall_, badgeBg, badgeFg);

    RECT info{rcPollCard_.left + Scale(16), rcPollCard_.top + Scale(88),
              rcPollCard_.right - Scale(16), rcPollCard_.top + Scale(108)};
    std::wstring infoText = s.pollingSamples
                                ? Fmt(L"中位 %.0f µs · %llu 次 · ±10%% 内 %.0f%%",
                                      s.medianIntervalUs, (unsigned long long)s.pollingSamples,
                                      s.within10Pct)
                                : L"快速移动鼠标以采样";
    DrawTextIn(dc, infoText, info, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT bar{rcPollCard_.left + Scale(16), rcPollCard_.bottom - Scale(34),
             rcPollCard_.right - Scale(16), rcPollCard_.bottom - Scale(22)};
    const COLORREF colors[5] = {RGB(255, 214, 102), RGB(255, 169, 64), kAccent,
                                RGB(64, 150, 255), RGB(22, 93, 255)};
    int total = bar.right - bar.left;
    if (!s.pollingSamples) {
        // 没有样本时不要用最后一段填满整条，避免误读为“100% 稳定”。
        FillRounded(dc, bar, Scale(3), RGB(238, 240, 243), RGB(238, 240, 243));
    } else {
        int x = bar.left;
        for (int i = 0; i < 5; ++i) {
            int w = (int)std::lround(total * s.bucketPct[i] / 100.0);
            if (i == 4) w = bar.right - x;
            RECT seg{x, bar.top, x + std::max(0, w), bar.bottom};
            if (seg.right > seg.left) {
                HBRUSH b = CreateSolidBrush(colors[i]);
                FillRect(dc, &seg, b);
                DeleteObject(b);
            }
            x += w;
        }
    }
    const wchar_t* labels[5] = {L"≤-20%", L"-10%", L"中位", L"+10%", L"≥+20%"};
    int lx = bar.left;
    for (int i = 0; i < 5; ++i) {
        RECT l{lx, bar.bottom + Scale(2), lx + total / 5, bar.bottom + Scale(18)};
        DrawTextIn(dc, labels[i], l, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        lx += total / 5;
    }
}

void MouseTestView::DrawClickCard(HDC dc, const MouseTestSnapshot& s) {
    DrawCard(dc, rcClickCard_, L"点击速度");

    // “当前 CPS”是滚动窗口值：停手超过窗口长度后必须归零，
    // 否则它会一直冻结在最后一次点击时的读数。
    double cps = s.cps;
    if (s.lastClickQpc > 0 && s.qpcFreq > 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double ageMs =
            (double)(now.QuadPart - s.lastClickQpc) * 1000.0 / (double)s.qpcFreq;
        if (ageMs > (double)s.clickWindowSec * 1000.0) cps = 0.0;
    }

    int cx = rcClickCard_.left + Scale(64);
    int cy = rcClickCard_.top + Scale(84);
    int radius = Scale(42);
    // 原 GDI Ellipse/Arc 的 PS_SOLID 描边在高 DPI 下呈阶梯状；改用 GDI+ 抗锯齿圆环。
    const int thickness = Scale(6);
    double maxCps = std::max(8.0, s.bestCps);
    double pct = std::min(100.0, cps / maxCps * 100.0);
    // DrawRing 的 radius 是圆环外沿；原画笔以 radius 为中心线、外沿为 radius + 半个
    // 线宽，因此这里加上 thickness / 2 保持原来的外径尺寸不变。
    svgicon::DrawRing(dc, cx, cy, radius + thickness / 2, thickness, RGB(235, 238, 242), kAccent,
                      (float)pct, -90.0f);

    RECT num{cx - radius, cy - Scale(16), cx + radius, cy + Scale(16)};
    DrawTextIn(dc, Fmt(L"%.0f", cps), num, fontBig_, kText,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT unit{cx - radius, cy + Scale(16), cx + radius, cy + Scale(32)};
    DrawTextIn(dc, L"次/秒", unit, fontSmall_, kMuted,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int x = cx + radius + Scale(20);
    int right = rcClickCard_.right - Scale(16);
    RECT winLabel{x, rcClickCard_.top + Scale(40), right, rcClickCard_.top + Scale(60)};
    DrawTextIn(dc, Fmt(L"本轮 %d s", windowSec_), winLabel, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT best{x, rcClickCard_.top + Scale(62), right, rcClickCard_.top + Scale(92)};
    DrawTextIn(dc, Fmt(L"最佳 %.1f", s.bestCps), best, fontTitle_, kText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT tip{x, rcClickCard_.top + Scale(96), right, rcClickCard_.bottom - Scale(12)};
    DrawTextIn(dc, L"只统计左键；页面按钮不计入", tip, fontSmall_, kMuted,
               DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void MouseTestView::DrawTrajectoryCard(HDC dc, const MouseTestSnapshot& s) {
    DrawCard(dc, rcTrajCard_, L"轨迹");

    RECT badge{rcTrajCard_.right - Scale(88), rcTrajCard_.top + Scale(12),
               rcTrajCard_.right - Scale(16), rcTrajCard_.top + Scale(34)};
    DrawBadge(dc, badge, Fmt(L"跳点 %llu", (unsigned long long)s.jumps), fontSmall_,
                s.jumps ? RGB(255, 247, 230) : kHover, s.jumps ? kAmber : kMuted);

    FillRounded(dc, rcTrajPlot_, Scale(6), RGB(250, 251, 252), kBorder);
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(236, 239, 243));
    HGDIOBJ old = SelectObject(dc, grid);
    for (int i = 1; i < 6; ++i) {
        int x = rcTrajPlot_.left + (rcTrajPlot_.right - rcTrajPlot_.left) * i / 6;
        MoveToEx(dc, x, rcTrajPlot_.top + 1, nullptr);
        LineTo(dc, x, rcTrajPlot_.bottom - 1);
        int y = rcTrajPlot_.top + (rcTrajPlot_.bottom - rcTrajPlot_.top) * i / 6;
        MoveToEx(dc, rcTrajPlot_.left + 1, y, nullptr);
        LineTo(dc, rcTrajPlot_.right - 1, y);
    }
    SelectObject(dc, old);
    DeleteObject(grid);

    const auto& pts = engine_.Trajectory();
    if (pts.size() < 2) {
        DrawTextIn(dc, L"移动鼠标以绘制轨迹", rcTrajPlot_, fontSmall_, kMuted,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        for (const auto& p : pts) {
            minX = std::min<int>(minX, p.x);
            maxX = std::max<int>(maxX, p.x);
            minY = std::min<int>(minY, p.y);
            maxY = std::max<int>(maxY, p.y);
        }
        int spanX = std::max(1, maxX - minX);
        int spanY = std::max(1, maxY - minY);
        int pad = Scale(10);
        int w = std::max(1, (int)((rcTrajPlot_.right - rcTrajPlot_.left) - pad * 2));
        int h = std::max(1, (int)((rcTrajPlot_.bottom - rcTrajPlot_.top) - pad * 2));
        // 等比缩放：X/Y 共用 min(w/spanX, h/spanY)，居中放置，避免轨迹被拉扁。
        const double scale =
            std::min(static_cast<double>(w) / spanX, static_cast<double>(h) / spanY);
        const int drawW = (int)std::lround(spanX * scale);
        const int drawH = (int)std::lround(spanY * scale);
        const int offX = (rcTrajPlot_.left + rcTrajPlot_.right - drawW) / 2;
        const int offY = (rcTrajPlot_.top + rcTrajPlot_.bottom - drawH) / 2;
        auto map = [&](POINT p) {
            POINT q{};
            q.x = offX + (int)std::lround((p.x - minX) * scale);
            q.y = offY + (int)std::lround((p.y - minY) * scale);
            return q;
        };

        HPEN path = CreatePen(PS_SOLID, std::max(1, Scale(1)), kAccent);
        old = SelectObject(dc, path);
        // step 上取整，保证最多约 2000 段；下取整时 pts.size()=3999 会画近 4000 段。
        size_t step = std::max<size_t>(1, (pts.size() + 1999) / 2000);
        bool first = true;
        POINT prev{};
        for (size_t i = 0; i < pts.size(); i += step) {
            POINT q = map(pts[i]);
            if (!first) {
                MoveToEx(dc, prev.x, prev.y, nullptr);
                LineTo(dc, q.x, q.y);
            }
            prev = q;
            first = false;
        }
        POINT last = map(pts.back());
        if (!first) {
            MoveToEx(dc, prev.x, prev.y, nullptr);
            LineTo(dc, last.x, last.y);
        }
        SelectObject(dc, old);
        DeleteObject(path);

        HBRUSH jumpBrush = CreateSolidBrush(kAmber);
        HGDIOBJ oldBrush = SelectObject(dc, jumpBrush);
        for (const auto& p : jumpPoints_) {
            if (p.x < minX || p.x > maxX || p.y < minY || p.y > maxY) continue;
            POINT q = map(p);
            Ellipse(dc, q.x - Scale(3), q.y - Scale(3), q.x + Scale(3), q.y + Scale(3));
        }
        SelectObject(dc, oldBrush);
        DeleteObject(jumpBrush);
    }

    RECT stats{rcTrajCard_.left + Scale(16), rcTrajCard_.bottom - Scale(32),
               rcClear_.left - Scale(8), rcTrajCard_.bottom - Scale(8)};
    std::wstring text = Fmt(L"报告 %llu · 路径 %llu · 跳点 %llu · 最长中断 %.1f ms",
                            (unsigned long long)s.reports, (unsigned long long)s.pathLength,
                            (unsigned long long)s.jumps, s.maxGapMs);
    DrawTextIn(dc, text, stats, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawButton(dc, rcClear_, L"清空轨迹", false, hover_ == 5);
}

void MouseTestView::DrawButton(HDC dc, const RECT& rc, const std::wstring& text, bool primary,
                               bool hover) const {
    COLORREF bg = primary ? (hover ? RGB(64, 140, 255) : kAccent) : (hover ? kHover : kCard);
    COLORREF border = primary ? bg : kBorder;
    COLORREF fg = primary ? RGB(255, 255, 255) : kText;
    FillRounded(dc, rc, Scale(6), bg, border);
    DrawTextIn(dc, text, rc, fontBody_, fg,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MouseTestView::DrawSegmented(HDC dc, const RECT& rc, const wchar_t* a, const wchar_t* b,
                                  int selected) const {
    FillRounded(dc, rc, Scale(6), kHover, kBorder);
    int half = (rc.right - rc.left) / 2;
    RECT ra{rc.left + Scale(2), rc.top + Scale(2), rc.left + half, rc.bottom - Scale(2)};
    RECT rb{rc.left + half, rc.top + Scale(2), rc.right - Scale(2), rc.bottom - Scale(2)};
    if (selected == 0) {
        FillRounded(dc, ra, Scale(4), kCard, kCard);
        DrawTextIn(dc, a, ra, fontBody_, kAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(dc, b, rb, fontBody_, kSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        FillRounded(dc, rb, Scale(4), kCard, kCard);
        DrawTextIn(dc, a, ra, fontBody_, kSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(dc, b, rb, fontBody_, kAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}


void MouseTestView::TrackCursor(int x, int y) {
    cursor_.x = x;
    cursor_.y = y;
    cursorValid_ = true;
}

bool MouseTestView::OnMouseMove(int x, int y) {
    TrackCursor(x, y);
    if (!trackLeave_ && parent_) {
        // 离开父窗口时由子类过程收到 WM_MOUSELEAVE；每次离开后需重新注册。
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = parent_;
        trackLeave_ = TrackMouseEvent(&tme) != FALSE;
    }
    POINT p{x, y};
    if (mode_ == Mode::Keyboard) {
        int hover = 0;
        if (PtInRect(&rcKbClear_, p)) hover = 12;
        else if (PtInRect(&rcKbLogTab_, p)) hover = 13;
        else if (PtInRect(&rcKbDiagTab_, p)) hover = 14;
        if (hover != kbHover_) {
            kbHover_ = hover;
            return true;
        }
        return false;
    }
    int hover = 0;
    if (PtInRect(&rcRedetect_, p)) hover = 1;
    else if (PtInRect(&rcWin5_, p)) hover = 2;
    else if (PtInRect(&rcWin10_, p)) hover = 3;
    else if (PtInRect(&rcReset_, p)) hover = 4;
    else if (PtInRect(&rcClear_, p)) hover = 5;
    if (hover != hover_) {
        hover_ = hover;
        return true;
    }
    return false;
}

void MouseTestView::OnMouseLeave() {
    trackLeave_ = false;
    // 光标离开窗口后 Raw Input 仍会到达；必须让坐标失效，否则会按最后一个
    // 内容区坐标继续累计移动/按键。
    cursorValid_ = false;
    if (mode_ == Mode::Keyboard) {
        if (kbHover_ == 0) return;
        kbHover_ = 0;
        dirty_ = true;
        if (parent_ && IsWindow(parent_)) InvalidateRect(parent_, &rcContent_, FALSE);
        return;
    }
    if (hover_ == 0) return;
    hover_ = 0;
    dirty_ = true;
    if (parent_ && IsWindow(parent_)) InvalidateRect(parent_, &rcContent_, FALSE);
}

void MouseTestView::OnBlur() {
    if (!visible_ || mode_ != Mode::Keyboard || kbBlurred_) return;
    kbBlurred_ = true;
    keyboard_.Blur((uint64_t)GetTickCount64());
    AppendKbLog(L"窗口", L"Blur");
}

void MouseTestView::OnFocus() {
    // 失焦去重只针对同一次失焦（WM_KILLFOCUS 与 WM_ACTIVATE 可能成对到达）。
    // 重新获得焦点后必须复位，否则下一次失焦不会记 Blur。
    if (mode_ == Mode::Keyboard) kbBlurred_ = false;
}

bool MouseTestView::IsOverPageButton(POINT p) const {
    // 页面按钮永不计入统计：顶栏按钮 + 「清空轨迹」（它位于轨迹卡内部）。
    // 原生目标下拉框也属于页面控件，且它自己接收鼠标消息，父窗口的
    // WM_MOUSEMOVE 不会更新 cursor_；必须显式排除。
    return PtInRect(&rcModeMouse_, p) || PtInRect(&rcModeKeyboard_, p) ||
           PtInRect(&rcRedetect_, p) || PtInRect(&rcWin5_, p) || PtInRect(&rcWin10_, p) ||
           PtInRect(&rcReset_, p) || PtInRect(&rcClear_, p) || PtInRect(&rcCombo_, p);
}

bool MouseTestView::OnClick(int x, int y) {
    POINT p{x, y};
    // 模式切换在任何模式下都有效。
    if (PtInRect(&rcModeMouse_, p)) {
        SetMode(Mode::Mouse);
        return true;
    }
    if (PtInRect(&rcModeKeyboard_, p)) {
        SetMode(Mode::Keyboard);
        if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
        return true;
    }
    if (mode_ == Mode::Keyboard) {
        if (PtInRect(&rcKbClear_, p)) {
            ClearKeyboard();
            return true;
        }
        if (PtInRect(&rcKbLogTab_, p)) {
            kbPanelTab_ = 0;
            dirty_ = true;
            return true;
        }
        if (PtInRect(&rcKbDiagTab_, p)) {
            kbPanelTab_ = 1;
            dirty_ = true;
            return true;
        }
        return false;
    }
    if (PtInRect(&rcRedetect_, p)) {
        // 规格要求：重新检测 = 重新匹配目标句柄，并清零全部统计（复用「重置」路径）。
        // 先重载下拉框：目标设备可能已经回到快照（重插/刷新），否则 targetLost_
        // 会让 MatchTarget 直接返回，用户永远无法用「重新检测」解除暂停。
        ResetAll();
        ReloadCombo();
        MatchTarget();
        return true;
    }
    if (PtInRect(&rcWin5_, p)) {
        windowSec_ = 5;
        engine_.SetClickWindow(5);
        dirty_ = true;
        return true;
    }
    if (PtInRect(&rcWin10_, p)) {
        windowSec_ = 10;
        engine_.SetClickWindow(10);
        dirty_ = true;
        return true;
    }
    if (PtInRect(&rcReset_, p)) {
        ResetAll();
        return true;
    }
    if (PtInRect(&rcClear_, p)) {
        engine_.ResetTrajectory();
        jumpPoints_.clear();
        hasLastTraj_ = false;
        lastTrajPoint_ = POINT{0, 0};
        dirty_ = true;
        return true;
    }
    return false;
}

void MouseTestView::OnTimer() {
    if (!visible_) return;
    if (mode_ == Mode::Keyboard) {
        // APM 会随时间窗口滑动而下降；即使没有新按键也要周期性重算。
        const ULONGLONG now = GetTickCount64();
        if (now - kbLastApmTick_ >= 250) {
            kbLastApmTick_ = now;
            const int apm = keyboard_.Apm((uint64_t)now);
            if (apm != lastApm_) {
                lastApm_ = apm;
                dirty_ = true;
            }
        }
        // 30s 折线每秒左移一格；只在 APM 数值变化时重绘会让折线停住不动。
        const ULONGLONG second = now / 1000;
        if (second != kbLastHistSec_) {
            kbLastHistSec_ = second;
            dirty_ = true;
        }
        // 键盘模式同样要清理悬停：光标移到导航栏时父窗口不再转发
        // WM_MOUSEMOVE，WM_MOUSELEAVE 也不会触发。
        if (kbHover_ != 0 && parent_) {
            POINT p{};
            if (GetCursorPos(&p) && ScreenToClient(parent_, &p) &&
                !PtInRect(&rcKbClear_, p) && !PtInRect(&rcKbLogTab_, p) &&
                !PtInRect(&rcKbDiagTab_, p)) {
                kbHover_ = 0;
                dirty_ = true;
            }
        }
        if (!dirty_) return;
        dirty_ = false;
        if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
        return;
    }
    // 光标移到导航栏等非内容区时 ui.cpp 不再转发 WM_MOUSEMOVE，WM_MOUSELEAVE 也不会触发
    // （光标仍在窗口内），这里在定时器里补一次判定，避免悬停高亮残留。
    if (hover_ != 0 && parent_) {
        POINT p{};
        if (GetCursorPos(&p) && ScreenToClient(parent_, &p) && !PtInRect(&rcContent_, p)) {
            OnMouseLeave();
        }
    }
    // 按键/滚轮高亮只维持 160 ms；没有新报文时也主动重绘一次，否则高亮会一直留在屏幕上。
    if (pressed_ >= 0 && GetTickCount64() - lastPressTick_ >= 160) {
        pressed_ = -1;
        dirty_ = true;
    }
    if (!dirty_) return;
    dirty_ = false;
    if (parent_) InvalidateRect(parent_, &rcContent_, FALSE);
}

void MouseTestView::HandleCommand(int id, int code) {
    if (mode_ != Mode::Mouse) return;
    if (id != kComboId || code != CBN_SELCHANGE) return;
    // CBN_SELCHANGE 只来自用户操作；记住“已显式选择”，避免 SetDevices 再把默认设备盖回去。
    targetChosen_ = true;
    comboIndex_ = (int)SendMessageW(combo_, CB_GETCURSEL, 0, 0);
    wchar_t buf[256] = {};
    SendMessageW(combo_, CB_GETLBTEXT, (WPARAM)comboIndex_, (LPARAM)buf);
    targetName_ = buf;
    // 手动改选即解除“已断开”状态；MatchTarget 会按新目标重新写入 id。
    targetDeviceId_.clear();
    targetLost_ = false;
    inputPaused_ = false;
    ResetAll();
    MatchTarget();
}

void MouseTestView::HandleRawInput(HRAWINPUT raw) {
    if (!visible_ || !raw) return;
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(raw, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
        return;
    }
    if (mode_ == Mode::Keyboard) {
        if (input.header.dwType == RIM_TYPEKEYBOARD) {
            HandleKeyboardInput(input.data.keyboard,
                                (input.data.keyboard.Flags & RI_KEY_BREAK) == 0);
        }
        return;
    }
    // 目标断开/句柄失效/demo：必须真的停止统计，不能因为 engine 的
    // “nullptr = 任意鼠标”而把其它设备的输入算进来。
    if (targetLost_ || inputPaused_) return;
    if (input.header.dwType != RIM_TYPEMOUSE) return;
    if (targetRaw_ && input.header.hDevice != targetRaw_) return;

    const RAWMOUSE& src = input.data.mouse;
    const bool isMove = (src.usFlags & MOUSE_MOVE_ABSOLUTE) || src.lLastX != 0 || src.lLastY != 0;
    const bool hasButtons = src.usButtonFlags != 0;

    // 高频 Raw Input 不在每条报文里调用 GetCursorPos；WM_MOUSEMOVE 已缓存光标位置。
    // 但光标移到原生下拉框（子窗口）时父窗口收不到 WM_MOUSEMOVE，缓存的坐标会
    // 一直停在进入子控件前的位置，导致点下拉框/页面控件仍被计入统计。
    // 按键/滚轮是低频事件，直接重新取一次；纯移动每 30 ms 同步一次。
    const ULONGLONG nowTick = GetTickCount64();
    if (hasButtons || nowTick - lastCursorSync_ >= 30) {
        POINT p{};
        if (parent_ && GetCursorPos(&p) && ScreenToClient(parent_, &p)) {
            TrackCursor(p.x, p.y);
        } else {
            cursorValid_ = false;
        }
        lastCursorSync_ = nowTick;
    }
    const POINT cursor = cursor_;

    // 按字段分别判定，不能整条报文放行：一条同时带移动和按键的报文若整体通过，
    // 点「清空轨迹」等页面按钮也会被算成点击。按键/滚轮只在鼠标图形区域内计数；
    // 移动在内容区内、顶栏与页面按钮之外计数。
    const bool allowButtons = cursorValid_ && PtInRect(&rcShape_, cursor) != FALSE;
    // 移动只在内容区计数；48px 内容头与工具行（模式切换/目标/按钮）整个排除，
    // 不能把“鼠标划过页面控件”算成被测鼠标的移动/回报率样本。
    const bool allowMove = cursorValid_ && PtInRect(&rcContent_, cursor) &&
                           !PtInRect(&rcHeader_, cursor) && !PtInRect(&rcTop_, cursor) &&
                           !IsOverPageButton(cursor);
    if (!(allowButtons && hasButtons) && !(allowMove && isMove)) return;

    RAWMOUSE m = src;
    if (!allowButtons) {
        m.usButtonFlags = 0;
        m.usButtonData = 0;
    }
    if (!allowMove) {
        m.lLastX = 0;
        m.lLastY = 0;
        m.usFlags = static_cast<USHORT>(m.usFlags & ~MOUSE_MOVE_ABSOLUTE);
    }

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    engine_.AddMouse(m, qpc.QuadPart, qpcFreq_);
    dirty_ = true;

    if (allowButtons) {
        if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            pressed_ = 0;
            lastPressTick_ = GetTickCount64();
        } else if (m.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) {
            pressed_ = 1;
            lastPressTick_ = GetTickCount64();
        } else if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) {
            pressed_ = 2;
            lastPressTick_ = GetTickCount64();
        } else if (m.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) {
            pressed_ = 3;
            lastPressTick_ = GetTickCount64();
        } else if (m.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) {
            pressed_ = 4;
            lastPressTick_ = GetTickCount64();
        } else if (m.usButtonFlags & RI_MOUSE_WHEEL) {
            // 滚轮没有“按下”事件，借 pressed_=1（图形中的滚轮位）短暂高亮一次。
            pressed_ = 1;
            lastPressTick_ = GetTickCount64();
        }
    }

    if (allowMove && isMove) {
        lastTrajPoint_.x += m.lLastX;
        lastTrajPoint_.y += m.lLastY;
        hasLastTraj_ = true;
        if (std::abs(m.lLastX) + std::abs(m.lLastY) > 200) {
            jumpPoints_.push_back(lastTrajPoint_);
            if (jumpPoints_.size() > 512) jumpPoints_.erase(jumpPoints_.begin());
        }
    }
}

void MouseTestView::HandleKeyboardInput(const RAWKEYBOARD& kb, bool down) {
    // 伪键/过载报文：VKey 0xFF 或 MakeCode 0xFF（KEYBOARD_OVERRUN_MAKE_CODE）。
    // 这类报文没有可靠的按下/释放配对，进入引擎会留下永久按住态。
    if (kb.VKey == 0xFF || kb.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return;
    const bool extended = (kb.Flags & RI_KEY_E0) != 0;
    const uint64_t now = (uint64_t)GetTickCount64();
    if (down) {
        const int idx = keyboard_.KeyDown(kb.VKey, kb.MakeCode, extended, now);
        if (idx >= 0) {
            kbBlurred_ = false;
            AppendKbLog(keyboard_.Def(idx).name, L"KeyDown");
        }
    } else {
        const int idx = keyboard_.KeyUp(kb.VKey, kb.MakeCode, extended, now);
        if (idx >= 0) AppendKbLog(keyboard_.Def(idx).name, L"KeyUp");
    }
}

