#include "ui.h"

#include "audio_center.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <timeapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#include "svg_icon.h"
#include "paint_region.h"
#include "ui_keyboard.h"
#include "ui_theme.h"

using namespace uitheme;

static HBRUSH brushCard_global = nullptr;
// 玻璃卡上的子窗口（DPI 滑块）用这个近似底色，不能纯白。
static HBRUSH brushField_global = nullptr;
static ULONG_PTR g_gdiplusToken = 0;

static void EnsureGdiplus() {
    static bool initialized = false;
    if (initialized) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok) {
        initialized = true;
    }
}

namespace {

constexpr UINT WM_SLIDER_SETRANGE = WM_APP + 30;
constexpr UINT WM_SLIDER_SETSTEP = WM_APP + 31;
constexpr UINT WM_SLIDER_SETVALUE = WM_APP + 32;
constexpr UINT WM_SLIDER_GETVALUE = WM_APP + 33;
constexpr UINT WM_SLIDER_SETLOG = WM_APP + 37;
constexpr double kPi = 3.14159265358979323846;
constexpr UINT WM_APP_SLIDER_CHANGED = WM_APP + 34;
constexpr UINT WM_APP_TRAY = WM_APP + 35;
// Slider 的补间跑在 MainWindow 的唯一 Animator 上；滑块通过该消息请求启动 16 ms 定时器。
constexpr UINT WM_APP_ANIM_START = WM_APP + 38;

bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    bool found = RegQueryValueExW(key, L"2.4GCenter", nullptr, &type, (LPBYTE)buf, &size) ==
                     ERROR_SUCCESS &&
                 type == REG_SZ;
    RegCloseKey(key);
    return found;
}

void SetAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --tray";
        RegSetValueExW(key, L"2.4GCenter", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"2.4GCenter");
    }
    RegCloseKey(key);
}

enum ControlId {
    ID_LIST = 2000,
    ID_REFRESH = 2001,
    ID_APPLY = 2002,
    ID_LOG_COMBO = 2004,
    ID_LOG_LIST = 2005,
    ID_LOG_FOLDER = 2006,
    ID_LOG_EXPORT = 2007,
    ID_LOG_DAYS = 2008,
    ID_SLIDER = 2100,
    ID_DPI_EDIT = 2101,
    ID_POLL = 2102,
    ID_DEB = 2103,
    ID_ANGLE = 2104,
    ID_VOL_DEV = 2105,
    ID_VOL_SYS = 2106,
    ID_SYS_ENDPOINT = 2107,
    ID_LOUD_OK = 2108,
    ID_STAGE_BASE = 2200,
    ID_MENU_OPEN = 2401,
    ID_MENU_REFRESH = 2402,
    ID_MENU_AUTOSTART = 2403,
    ID_MENU_LOG = 2404,
    ID_MENU_EXIT = 2405,
    ID_MENU_NOTIFY_SETTINGS = 2406,
    ID_MENU_TEST_NOTIFY = 2407,
    ID_MENU_EVENTS = 2408,
    ID_MENU_TEST = 2409,
    ID_MENU_AUDIO = 2410,
};

COLORREF BatteryColor(int pct, bool charging, bool stale) {
    if (stale || pct < 0) return RGB(156, 163, 175);
    if (charging) return kGreen;
    if (pct <= 20) return kRed;
    if (pct <= 49) return kAmber;
    return kGreen;
}

UINT GetDpiForHwnd(HWND hwnd) {
    using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
    static PFN_GetDpiForWindow fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (PFN_GetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
    }
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d) return d;
    }
    HDC dc = GetDC(hwnd);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return d ? d : 96;
}

// CW_USEDEFAULT 把初始窗口放在主显示器；用目标显示器的 DPI 计算初始尺寸，
// 避免 150% DPI 下按 96 DPI 创建后再被 WM_DPICHANGED 放大。
UINT GetDpiForMonitorCompat(HMONITOR mon) {
    using PFN_GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) fn = (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
        // 故意不 FreeLibrary：函数指针可能在进程生命周期内一直使用。
    }
    if (fn && mon) {
        UINT dx = 96, dy = 96;
        // MDT_EFFECTIVE_DPI = 0，避免额外包含 shellscalingapi.h。
        if (SUCCEEDED(fn(mon, 0, &dx, &dy)) && dx) return dx;
    }
    HDC screen = GetDC(nullptr);
    UINT d = screen ? (UINT)GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return d ? d : 96;
}

// 当前显示器的真实刷新率。DWM 的合成速率最准确（120/144 Hz 下与
// EnumDisplaySettings 的 dmDisplayFrequency 通常一致，但后者在部分驱动/虚拟
// 显示器上会给出 59 或 1）。结果缓存 ~2 秒，避免每帧扫描都调用 DWM。
int g_refreshHzCache = 0;
ULONGLONG g_refreshHzCachedAt = 0;
HMONITOR g_refreshHzCachedMon = nullptr;

void InvalidateRefreshCache() {
    g_refreshHzCache = 0;
    g_refreshHzCachedAt = 0;
    g_refreshHzCachedMon = nullptr;
}

static int MonitorRefreshHz(HWND hwnd) {
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    const ULONGLONG now = GetTickCount64();
    if (g_refreshHzCache > 0 && g_refreshHzCachedMon == mon &&
        now - g_refreshHzCachedAt < 2000) {
        return g_refreshHzCache;
    }

    int hz = 0;
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(hwnd, &info)) &&
        info.rateCompose.uiDenominator != 0) {
        const double rate =
            (double)info.rateCompose.uiNumerator / (double)info.rateCompose.uiDenominator;
        const int rounded = (int)std::lround(rate);
        if (rounded >= 30 && rounded <= 360) hz = rounded;
    }
    if (hz <= 0) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        // dmDisplayFrequency == 1 表示“硬件默认”，不可用。
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmDisplayFrequency > 1) {
            hz = (int)dm.dmDisplayFrequency;
        }
    }
    if (hz <= 0) hz = 60;

    g_refreshHzCache = hz;
    g_refreshHzCachedAt = now;
    g_refreshHzCachedMon = mon;
    return hz;
}

int DrawPill(HDC dc, int x, int y, const std::wstring& text, HFONT font, COLORREF bg,
             COLORREF fg, int height) {
    SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    int pad = height / 3;
    int w = sz.cx + pad * 2;
    RECT r{x, y, x + w, y + height};
    FillRounded(dc, r, height / 2, bg, bg);
    SelectObject(dc, font);
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return w;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h,
                  float radius) {
    float d = radius * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// 设备类型图形已迁移到 SVG 图标表（src/svg_icon.cpp）；这里只做 Kind→Id 映射，
// 保证几何定义只有一处来源，任意 DPI 下都走 GDI+ 抗锯齿渲染。
void DrawDeviceGlyph(HDC dc, Kind kind, const RECT& rc, COLORREF color, bool filled) {
    svgicon::Id id = svgicon::Id::Other;
    switch (kind) {
        case Kind::Mouse:
            id = svgicon::Id::Mouse;
            break;
        case Kind::Keyboard:
            id = svgicon::Id::Keyboard;
            break;
        case Kind::Headset:
            id = svgicon::Id::Headset;
            break;
        default:
            id = svgicon::Id::Other;  // Kind::Other / Kind::Unknown
            break;
    }
    svgicon::Draw(dc, id, rc, color, 0.0f, filled);
}

void DrawLightning(HDC dc, int x, int y, int w, int h, COLORREF color) {
    // Bolt 的 path 是闭合轮廓，必须 filled=true 才会填充，几何见 src/svg_icon.cpp。
    svgicon::Draw(dc, svgicon::Id::Bolt, RECT{x, y, x + w, y + h}, color, 0.0f, true);
}

// 键盘焦点链只收可见、可用且声明 WS_TABSTOP 的原生控件；隐藏控件绝不能
// 被 Tab 命中，否则焦点会落到看不见的窗口上。
void AddTabStop(std::vector<HWND>& chain, HWND h) {
    if (!h || !IsWindow(h)) return;
    if (!IsWindowVisible(h) || !IsWindowEnabled(h)) return;
    if ((GetWindowLongPtrW(h, GWL_STYLE) & WS_TABSTOP) == 0) return;
    if (std::find(chain.begin(), chain.end(), h) != chain.end()) return;
    chain.push_back(h);
}

BOOL CALLBACK CollectTabStopProc(HWND child, LPARAM lp) {
    AddTabStop(*(std::vector<HWND>*)lp, child);
    return TRUE;
}

void DrawRingGauge(HDC dc, int cx, int cy, int radius, int thickness, float pct, bool charging,
                   bool stale, HFONT font, const std::wstring& text, COLORREF textColor,
                   float pulse = 0.5f) {
    EnsureGdiplus();
    COLORREF color = BatteryColor(pct < 0 ? -1 : (int)lround(pct), charging, stale);
    float r = (float)std::max(1, radius - thickness / 2);
    float t = (float)std::max(2, thickness);
    Gdiplus::RectF rect((float)cx - r, (float)cy - r, r * 2.0f, r * 2.0f);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::Pen track(Gdiplus::Color(255, 229, 231, 235), t);
        g.DrawEllipse(&track, rect);

        if (pct > 0) {
            Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color),
                                            GetBValue(color)),
                             t);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            if (pct >= 100.0f) {
                g.DrawEllipse(&pen, rect);
            } else {
                float sweep = 3.6f * std::max(0.0f, std::min(100.0f, pct));
                g.DrawArc(&pen, rect, -90.0f, sweep);
            }
        }

        if (charging) {
            float pulseScale = 0.85f + 0.35f * std::max(0.0f, std::min(1.0f, pulse));
            float dot = std::max(4.0f, t * 1.2f) * pulseScale;
            float angle = -45.0f * 3.14159265358979323846f / 180.0f;
            float dx = (float)cx + r * std::cos(angle);
            float dy = (float)cy + r * std::sin(angle);
            Gdiplus::SolidBrush glow(Gdiplus::Color(
                (BYTE)(24 + 46 * pulse), GetRValue(color), GetGValue(color), GetBValue(color)));
            g.FillEllipse(&glow, dx - dot, dy - dot, dot * 2, dot * 2);
            Gdiplus::SolidBrush dotBrush(Gdiplus::Color(255, GetRValue(color),
                                                       GetGValue(color), GetBValue(color)));
            g.FillEllipse(&dotBrush, dx - dot / 2, dy - dot / 2, dot, dot);
            Gdiplus::Pen dotPen(Gdiplus::Color(255, 255, 255, 255), 2.0f);
            g.DrawEllipse(&dotPen, dx - dot / 2, dy - dot / 2, dot, dot);
        }
    }

    RECT tr{cx - radius, cy - radius, cx + radius, cy + radius};
    DrawTextIn(dc, text, tr, font, textColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSectionTitle(HDC dc, int x, int y, const std::wstring& title, HFONT font) {
    RECT r{x, y, x + 400, y + 24};
    DrawTextIn(dc, title, r, font, kSub, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

std::wstring DeviceStatusText(const Device& d) {
    if (!d.online) return L"离线";
    if (d.stale) return L"上次读数";
    return L"在线";
}

// 物理链路是 USB，但设备本身是无线型号时，必须与真正的有线设备区分开。
// 实现已提升到 protocols.cpp 的 DeviceConnLabel，供 --once 复用。

void DrawTransportBadge(HDC dc, const RECT& rc, Conn conn, HFONT font) {
    std::wstring text = conn == Conn::Bluetooth ? L"BT" : L"USB";
    COLORREF bg = conn == Conn::Bluetooth ? RGB(234, 242, 255) : RGB(243, 244, 246);
    COLORREF fg = conn == Conn::Bluetooth ? kAccent : RGB(100, 106, 115);
    FillRounded(dc, rc, std::max(3, (int)((rc.bottom - rc.top) / 4)), bg, bg);
    DrawTextIn(dc, text, rc, font, fg,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}


struct SliderState {
    int min = 100;
    int max = 30000;
    int step = 50;
    int value = 1000;
    double shownValue = 1000.0;
    bool dragging = false;
    bool hover = false;
    bool logScale = false;
    // 与 MainWindow 共享同一个 Animator：不再为滑块单独创建 16 ms 定时器。
    Animator* animator = nullptr;
    Animator::AnimId animId = 0;
};

void SliderStopTween(SliderState& st) {
    if (st.animator && st.animId) st.animator->Cancel(st.animId);
    st.animId = 0;
}

void SliderStartTween(HWND hwnd, SliderState& st, int target) {
    SliderStopTween(st);
    HWND parent = GetParent(hwnd);
    // 系统关闭动画、窗口隐藏或没有 Animator 时直接跳到终值，与电量环一致。
    if (!st.animator || !SystemAnimationsEnabled() || !IsWindowVisible(parent)) {
        st.shownValue = target;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    const float to = (float)target;
    st.animId = st.animator->Add(
        (float)st.shownValue, to, 300, Easing::Spring, [hwnd, to](float value) {
            SliderState* live = (SliderState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (!live) return;
            live->shownValue = value;
            if (value == to) live->animId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        });
    if (st.animator->Active()) {
        SendMessageW(parent, WM_APP_ANIM_START, 0, 0);
    } else {
        // Animator 自身关闭动画时 Add 会同步 apply 终值，不留下轨道。
        st.animId = 0;
    }
}

void SliderNotify(HWND hwnd, int value) {
    SendMessageW(GetParent(hwnd), WM_APP_SLIDER_CHANGED, (WPARAM)value, 0);
}

int SliderSnap(const SliderState& st, int v) {
    if (st.step <= 1) return ClampInt(v, st.min, st.max);
    int k = (v - st.min + st.step / 2) / st.step;
    int snapped = st.min + k * st.step;
    return ClampInt(snapped, st.min, st.max);
}

int SliderFractionValue(const SliderState& st, double value) {
    if (st.max <= st.min) return 0;
    double frac;
    if (st.logScale && st.min > 0) {
        frac = std::log(std::max(1.0, value) / st.min) / std::log((double)st.max / st.min);
    } else {
        frac = (value - st.min) / (double)(st.max - st.min);
    }
    return ClampInt((int)(frac * 100.0 + 0.5), 0, 100);
}

int SliderValueAtX(const SliderState& st, int x, int left, int width) {
    double t = width > 0 ? (double)(x - left) / width : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    double v;
    if (st.logScale && st.min > 0) {
        v = st.min * std::pow((double)st.max / st.min, t);
    } else {
        v = st.min + t * (st.max - st.min);
    }
    return SliderSnap(st, (int)(v + 0.5));
}

RECT SliderTrack(HWND hwnd, const SliderState&) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int pad = 14;
    int h = rc.bottom - rc.top;
    return {rc.left + pad, rc.top + h / 2 - 4, rc.right - pad, rc.top + h / 2 + 4};
}

void SliderSetFromX(HWND hwnd, SliderState& st, int x) {
    RECT track = SliderTrack(hwnd, st);
    int w = std::max(1, (int)(track.right - track.left));
    int v = SliderValueAtX(st, x, track.left, w);
    if (v != st.value) {
        st.value = v;
        st.shownValue = v;
        SliderStopTween(st);
        InvalidateRect(hwnd, nullptr, FALSE);
        SliderNotify(hwnd, v);
    }
}

LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SliderState* st = (SliderState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = (CREATESTRUCTW*)lp;
            auto* s = new SliderState();
            s->animator = (Animator*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
            return TRUE;
        }
        case WM_NCDESTROY:
            if (st) SliderStopTween(*st);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        case WM_SLIDER_SETRANGE:
            if (st) {
                SliderStopTween(*st);
                st->min = (int)wp;
                st->max = (int)lp;
                if (st->max < st->min) std::swap(st->min, st->max);
                st->value = ClampInt(st->value, st->min, st->max);
                st->shownValue = ClampInt((int)st->shownValue, st->min, st->max);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SLIDER_SETSTEP:
            if (st) st->step = std::max(1, (int)wp);
            return 0;
        case WM_SLIDER_SETLOG:
            if (st) {
                st->logScale = wp != 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SLIDER_SETVALUE:
            if (st) {
                int v = ClampInt((int)wp, st->min, st->max);
                st->value = v;
                if (!st->dragging && std::fabs((double)v - st->shownValue) > 0.5) {
                    SliderStartTween(hwnd, *st, v);
                } else {
                    SliderStopTween(*st);
                    st->shownValue = v;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SLIDER_GETVALUE:
            return st ? st->value : 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            // 滑块是子窗口，无法真正透明；用与玻璃卡接近的底色，
            // 避免在卡片上露出一条纯白硬边。
            HBRUSH bgBrush = brushField_global ? brushField_global : brushCard_global;
            if (bgBrush) FillRect(mem, &rc, bgBrush);
            if (st) {
                RECT track = SliderTrack(hwnd, *st);
                bool enabled = IsWindowEnabled(hwnd) != FALSE;
                FillRounded(mem, track, 4, kTrack, kTrack);
                int w = track.right - track.left;
                int frac = SliderFractionValue(*st, st->shownValue);
                RECT fill{track.left, track.top, track.left + w * frac / 100, track.bottom};
                if (frac > 0) {
                    FillRounded(mem, fill, 4, enabled ? kAccent : RGB(156, 163, 175),
                                enabled ? kAccent : RGB(156, 163, 175));
                }
                int thumbX = track.left + w * frac / 100;
                int thumbR = 8;
                // 拇指改用抗锯齿圆（GDI Ellipse 的边缘有阶梯），白描边保持 2px。
                const int cyThumb = rc.bottom / 2;
                RECT outer{thumbX - thumbR, cyThumb - thumbR, thumbX + thumbR,
                           cyThumb + thumbR};
                svgicon::FillRoundRect(mem, outer, thumbR, RGB(255, 255, 255),
                                       RGB(255, 255, 255));
                const int innerR = std::max(1, thumbR - 2);
                RECT inner{thumbX - innerR, cyThumb - innerR, thumbX + innerR,
                           cyThumb + innerR};
                COLORREF thumbColor = enabled ? kAccent : RGB(156, 163, 175);
                svgicon::FillRoundRect(mem, inner, innerR, thumbColor, thumbColor);
                // 键盘焦点：滑轨很细，把焦点框画在整个滑杆客户区内，Tab 后可见。
                if (GetFocus() == hwnd) {
                    RECT fr{2, 2, rc.right - 2, rc.bottom - 2};
                    DrawFocusRect(mem, &fr);
                }
            }
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (st) {
                SliderStopTween(*st);
                SetCapture(hwnd);
                st->dragging = true;
                SetFocus(hwnd);
                SliderSetFromX(hwnd, *st, GET_X_LPARAM(lp));
            }
            return 0;
        case WM_MOUSEMOVE:
            if (st && st->dragging) SliderSetFromX(hwnd, *st, GET_X_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            if (st) {
                st->dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            // 拖动中被别的窗口抢走 capture（Alt+Tab、弹窗等）时必须结束拖动，
            // 否则后续普通 WM_MOUSEMOVE 仍会改 DPI。
            if (st) {
                st->dragging = false;
                SliderStopTween(*st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (st) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? st->step : -st->step;
                int v = SliderSnap(*st, st->value + delta);
                if (v != st->value) {
                    st->value = v;
                    st->shownValue = v;
                    SliderStopTween(*st);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    SliderNotify(hwnd, v);
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (st) {
                int v = st->value;
                if (wp == VK_LEFT || wp == VK_DOWN) v -= st->step;
                else if (wp == VK_RIGHT || wp == VK_UP) v += st->step;
                else if (wp == VK_HOME) v = st->min;
                else if (wp == VK_END) v = st->max;
                else break;
                v = SliderSnap(*st, v);
                if (v != st->value) {
                    st->value = v;
                    st->shownValue = v;
                    SliderStopTween(*st);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    SliderNotify(hwnd, v);
                }
                return 0;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 细胶囊滚动条：无轨道、无箭头，浅灰圆角滑块。
void DrawThinBar(HDC dc, const RECT& thumb) {
    int w = thumb.right - thumb.left;
    int h = thumb.bottom - thumb.top;
    if (w <= 0 || h <= 0) return;
    EnsureGdiplus();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float radius = (float)std::min(w, h) / 2.0f;
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, (float)thumb.left, (float)thumb.top, (float)w, (float)h, radius);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 201, 205, 213));
    g.FillPath(&brush, &path);
    g.Flush();
}

}  // namespace

const wchar_t* MainWindow::ClassName() { return L"WirelessCenterMain"; }

bool MainWindow::Create(HINSTANCE instance, bool demo, bool startHidden) {
    instance_ = instance;
    demo_ = demo;
    startHidden_ = startHidden;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = ClassName();
    RegisterClassExW(&wc);

    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.style = CS_HREDRAW | CS_VREDRAW;
    sc.lpfnWndProc = SliderProc;
    sc.hInstance = instance;
    sc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    sc.lpszClassName = L"WirelessCenterSlider";
    RegisterClassExW(&sc);

    // 初始窗口按主显示器 DPI 缩放；工作区放不下时夹到工作区，
    // 避免 1080p@150% 下初始高度超过屏幕可用高度。
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT monDpi = GetDpiForMonitorCompat(mon);
    int w = MulDiv(1000, (int)monDpi, 96);
    int h = MulDiv(780, (int)monDpi, 96);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        int workW = mi.rcWork.right - mi.rcWork.left;
        int workH = mi.rcWork.bottom - mi.rcWork.top;
        if (workW > 0 && w > workW) w = workW;
        if (workH > 0 && h > workH) h = workH;
    }

    HWND hwnd = CreateWindowExW(0, ClassName(), L"2.4G 设备中心",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, w, h, nullptr, nullptr, instance, this);
    if (!hwnd) return false;
    hwnd_ = hwnd;
    if (!startHidden_) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return true;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (self) return self->Handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int MainWindow::Scale(int value) const { return MulDiv(value, (int)dpi_, 96); }

const Device* MainWindow::Selected() const {
    if (selectedIndex_ < 0 || selectedIndex_ >= (int)snapshot_.devices.size()) return nullptr;
    return &snapshot_.devices[selectedIndex_];
}

void MainWindow::CreateControls() {
    list_ = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED |
                                LBS_NOTIFY | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                            0, 0, 10, 10, hwnd_, (HMENU)ID_LIST, instance_, nullptr);
    btnRefresh_ = CreateWindowExW(0, L"BUTTON", L"刷新",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                                  10, hwnd_, (HMENU)ID_REFRESH, instance_, nullptr);
    // 日志视图控件：默认隐藏，切到日志时显示。
    logCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10,
                                200, hwnd_, (HMENU)ID_LOG_COMBO, instance_, nullptr);
    logDaysCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                    WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0,
                                    10, 200, hwnd_, (HMENU)ID_LOG_DAYS, instance_, nullptr);
    for (int d = 1; d <= 7; ++d) {
        wchar_t text[16] = {};
        swprintf(text, 16, L"%d 天", d);
        SendMessageW(logDaysCombo_, CB_ADDSTRING, 0, (LPARAM)text);
    }
    SendMessageW(logDaysCombo_, CB_SETCURSEL, 6, 0);  // 默认 7 天
    logList_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                   LVS_SHOWSELALWAYS,
                               0, 0, 10, 10, hwnd_, (HMENU)ID_LOG_LIST, instance_, nullptr);
    ListView_SetExtendedListViewStyle(logList_,
                                      LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    const wchar_t* cols[] = {L"时间", L"类型", L"设备", L"详情"};
    int widths[] = {126, 58, 150, 420};
    for (int i = 0; i < 4; ++i) {
        col.pszText = const_cast<LPWSTR>(cols[i]);
        col.cx = Scale(widths[i]);
        col.iSubItem = i;
        ListView_InsertColumn(logList_, i, &col);
    }
    logFolder_ = CreateWindowExW(0, L"BUTTON", L"打开文件夹",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                 (HMENU)ID_LOG_FOLDER, instance_, nullptr);
    logExport_ = CreateWindowExW(0, L"BUTTON", L"导出",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                 (HMENU)ID_LOG_EXPORT, instance_, nullptr);
    btnApply_ = CreateWindowExW(0, L"BUTTON", L"应用设置",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                hwnd_, (HMENU)ID_APPLY, instance_, nullptr);
    slider_ = CreateWindowExW(0, L"WirelessCenterSlider", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd_,
                              (HMENU)ID_SLIDER, instance_, &anim_);
    editDpi_ = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL |
                                   ES_CENTER | WS_BORDER,
                               0, 0, 10, 10, hwnd_, (HMENU)ID_DPI_EDIT, instance_, nullptr);
    // 音量卡的两个数字输入：默认隐藏，只有设备页显示音量卡时由 Layout 摆到
    // 百分比位置。ES_NUMBER 只允许数字，最终仍由 OnVolumeEditChanged 夹到 0..100。
    editVolDev_ = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL |
                                      ES_CENTER | WS_BORDER,
                                  0, 0, 10, 10, hwnd_, (HMENU)ID_VOL_DEV, instance_, nullptr);
    editVolSys_ = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL |
                                      ES_CENTER | WS_BORDER,
                                  0, 0, 10, 10, hwnd_, (HMENU)ID_VOL_SYS, instance_, nullptr);
    // 系统输出端点选择：第一项跟随系统默认，其余为活动播放端点。
    // 让耳机作为默认输出时，仍可把“系统输出”固定到扬声器等另一个端点。
    comboVolSys_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, 0, 10, 200, hwnd_, (HMENU)ID_SYS_ENDPOINT, instance_,
                                   nullptr);
    // 耳机听力保护：推荐音量不超过 60%，超过时先警告并显示此复选框。
    chkLoudOk_ = CreateWindowExW(0, L"BUTTON", L"我已了解风险，允许超过 60%",
                                 WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                 hwnd_, (HMENU)ID_LOUD_OK, instance_, nullptr);
    comboPoll_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                     WS_VSCROLL,
                                 0, 0, 10, 200, hwnd_, (HMENU)ID_POLL, instance_, nullptr);
    comboDeb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                0, 0, 10, 200, hwnd_, (HMENU)ID_DEB, instance_, nullptr);
    chkAngle_ = CreateWindowExW(0, L"BUTTON", L"角度捕捉",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                                hwnd_, (HMENU)ID_ANGLE, instance_, nullptr);

    SetFont(list_, fontBody_);
    SetFont(btnRefresh_, fontBodyBold_);
    SetFont(btnApply_, fontBodyBold_);
    SetFont(editDpi_, fontBodyBold_);
    SetFont(editVolDev_, fontBodyBold_);
    SetFont(editVolSys_, fontBodyBold_);
    SetFont(comboVolSys_, fontBody_);
    SetFont(chkLoudOk_, fontSmall_);
    SetFont(comboPoll_, fontBody_);
    SetFont(comboDeb_, fontBody_);
    SetFont(chkAngle_, fontBody_);
    SetFont(logCombo_, fontBody_);
    SetFont(logDaysCombo_, fontBody_);
    SetFont(logList_, fontBody_);
    SetFont(logFolder_, fontBodyBold_);
    SetFont(logExport_, fontBodyBold_);

    for (int i = 0; i < 8; ++i) {
        HWND b = CreateWindowExW(0, L"BUTTON", L"",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                 (HMENU)(INT_PTR)(ID_STAGE_BASE + i), instance_, nullptr);
        SetFont(b, fontBodyBold_);
        stageButtons_.push_back(b);
    }
}

void MainWindow::RecreateFonts() {
    auto del = [](HFONT& f) {
        if (f) DeleteObject(f);
        f = nullptr;
    };
    del(fontTitle_);
    del(fontBody_);
    del(fontBodyBold_);
    del(fontSmall_);
    del(fontRing_);
    del(fontRingSmall_);
    del(fontBadge_);
    fontTitle_ = MakeFont(dpi_, 16, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontBody_ = MakeFont(dpi_, 10, FW_NORMAL, L"Microsoft YaHei UI");
    fontBodyBold_ = MakeFont(dpi_, 10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    // 辅助字 9pt（规格值）：8pt 在 100% DPI 下只有 11px，中文偏细、观感像“低分辨率”。
    fontSmall_ = MakeFont(dpi_, 9, FW_NORMAL, L"Microsoft YaHei UI");
    fontRing_ = MakeFont(dpi_, 26, FW_SEMIBOLD, L"Segoe UI");
    // 100% 比 95% 多一个字符；小一号字体避免文字压到圆环。
    fontRingSmall_ = MakeFont(dpi_, 21, FW_SEMIBOLD, L"Segoe UI");
    fontBadge_ = MakeFont(dpi_, 9, FW_SEMIBOLD, L"Microsoft YaHei UI");
    if (list_) SetFont(list_, fontBody_);
    if (btnRefresh_) SetFont(btnRefresh_, fontBodyBold_);
    if (btnApply_) SetFont(btnApply_, fontBodyBold_);
    if (editDpi_) SetFont(editDpi_, fontBodyBold_);
    if (editVolDev_) SetFont(editVolDev_, fontBodyBold_);
    if (editVolSys_) SetFont(editVolSys_, fontBodyBold_);
    if (comboPoll_) SetFont(comboPoll_, fontBody_);
    if (comboDeb_) SetFont(comboDeb_, fontBody_);
    if (chkAngle_) SetFont(chkAngle_, fontBody_);
    if (logCombo_) SetFont(logCombo_, fontBody_);
    if (logDaysCombo_) SetFont(logDaysCombo_, fontBody_);
    if (logList_) SetFont(logList_, fontBody_);
    if (logFolder_) SetFont(logFolder_, fontBodyBold_);
    if (logExport_) SetFont(logExport_, fontBodyBold_);
    for (HWND b : stageButtons_) SetFont(b, fontBodyBold_);
}

void MainWindow::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int W = rc.right;
    int H = rc.bottom;
    int railW = Scale(76);
    int footerH = Scale(26);
    const bool audio = view_ == View::Device && audioExpanded_;
    bool showList = view_ == View::Device && !audioExpanded_;
    int listW = showList ? Scale(300) : 0;

    rcRail_ = {0, 0, railW, H};
    rcFooter_ = {0, H - footerH, W, H};
    rcListPane_ = {railW, 0, railW + listW, H - footerH};
    rcContent_ = {railW + listW, 0, W, H - footerH};
    rcContentHeader_ = {rcContent_.left, rcContent_.top, rcContent_.right,
                        rcContent_.top + Scale(48)};
    if (audio) {
        audiocenter::SetPanelDpi(dpi_);
        // 滑动期间面板位置由 Animator 回调独占，Layout 不能抢同一个 bounds。
        if (!audioSliding_) {
            audioPanelRect_ = {rcContent_.left, rcContentHeader_.bottom, rcContent_.right,
                               rcContent_.bottom};
            audiocenter::SetPanelBounds(audioPanelRect_);
            audioSlideT_ = 0.0f;
        }
    }

    // 导航项：设备 / 日志 / 测试。
    int navX = Scale(10);
    int navW = railW - Scale(20);
    int navY = Scale(84);
    int navH = Scale(52);
    int navGap = Scale(6);
    for (int i = 0; i < 3; ++i) {
        rcNavItems_[i] = {navX, navY + i * (navH + navGap), navX + navW,
                          navY + i * (navH + navGap) + navH};
    }
    if (!anim_.Active() || navIndicatorY_ <= 0.0f) {
        navIndicatorY_ = (float)rcNavItems_[(int)view_].top;
    }

    bool dev = view_ == View::Device && !audioExpanded_;
    bool log = view_ == View::Log;
    bool test = view_ == View::Test;

    ShowWindow(list_, showList ? SW_SHOW : SW_HIDE);
    ShowWindow(btnRefresh_, (dev || log) ? SW_SHOW : SW_HIDE);

    ShowWindow(logCombo_, log ? SW_SHOW : SW_HIDE);
    ShowWindow(logDaysCombo_, log ? SW_SHOW : SW_HIDE);
    ShowWindow(logList_, log ? SW_SHOW : SW_HIDE);
    ShowWindow(logFolder_, log ? SW_SHOW : SW_HIDE);
    ShowWindow(logExport_, log ? SW_SHOW : SW_HIDE);

    // 设备页控件是否可见由 UpdateControlsFromDevice 决定（取决于设备能力），
    // Layout 只负责在非设备页把它们全部隐藏，避免覆盖能力判断。
    if (!dev) {
        ShowWindow(btnApply_, SW_HIDE);
        ShowWindow(slider_, SW_HIDE);
        ShowWindow(editDpi_, SW_HIDE);
        ShowWindow(comboPoll_, SW_HIDE);
        ShowWindow(comboDeb_, SW_HIDE);
        ShowWindow(chkAngle_, SW_HIDE);
        ShowWindow(editVolDev_, SW_HIDE);
        ShowWindow(editVolSys_, SW_HIDE);
        ShowWindow(comboVolSys_, SW_HIDE);
        ShowWindow(chkLoudOk_, SW_HIDE);
        for (HWND h : stageButtons_) ShowWindow(h, SW_HIDE);
    }

    // 内容头右侧操作按钮。
    int bh = Scale(30);
    int right = rcContent_.right - Scale(20);
    MoveWindow(btnRefresh_, right - Scale(92), rcContentHeader_.top + Scale(9), Scale(92), bh,
               TRUE);
    MoveWindow(btnApply_, right - Scale(92) - Scale(8) - Scale(124),
               rcContentHeader_.top + Scale(9), Scale(124), bh, TRUE);

    if (showList) {
        RECT listBox{rcListPane_.left + Scale(8), rcListPane_.top + Scale(56),
                     rcListPane_.right - Scale(8), rcListPane_.bottom - Scale(8)};
        MoveWindow(list_, listBox.left, listBox.top, listBox.right - listBox.left,
                   listBox.bottom - listBox.top, TRUE);
    }

    if (test) {
        testView_.Layout({rcContent_.left, rcContent_.top, rcContent_.right,
                          rcContent_.bottom});
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (log) {
        int top = rcContentHeader_.top + Scale(9);
        int folderW = Scale(104);
        int exportW = Scale(76);
        int daysW = Scale(86);
        int comboW = Scale(200);
        int x = rcContent_.right - Scale(20) - Scale(92) - Scale(8);
        MoveWindow(logFolder_, x - folderW, top, folderW, bh, TRUE);
        x -= folderW + Scale(8);
        MoveWindow(logExport_, x - exportW, top, exportW, bh, TRUE);
        x -= exportW + Scale(12);
        MoveWindow(logDaysCombo_, x - daysW, top, daysW, Scale(220), TRUE);
        x -= daysW + Scale(8);
        MoveWindow(logCombo_, x - comboW, top, comboW, Scale(220), TRUE);

        int bodyTop = rcContentHeader_.bottom + Scale(10);
        int bodyBottom = rcContent_.bottom - Scale(16);
        int chartH = ClampInt((bodyBottom - bodyTop) * 52 / 100, Scale(220), Scale(340));
        rcLogChart_ = {rcContent_.left + Scale(20), bodyTop, rcContent_.right - Scale(20),
                       bodyTop + chartH};
        logListLabelY_ = rcLogChart_.bottom + Scale(8);
        int listTop = logListLabelY_ + Scale(24);
        int listH = bodyBottom - listTop;
        MoveWindow(logList_, rcContent_.left + Scale(20), listTop,
                   rcContent_.right - rcContent_.left - Scale(40), listH > 80 ? listH : 80, TRUE);
        int total = rcContent_.right - rcContent_.left - Scale(40);
        int wTime = Scale(126), wType = Scale(58), wDev = Scale(150);
        int wDetail = total - wTime - wType - wDev - Scale(90);
        ListView_SetColumnWidth(logList_, 0, wTime);
        ListView_SetColumnWidth(logList_, 1, wType);
        ListView_SetColumnWidth(logList_, 2, wDev);
        ListView_SetColumnWidth(logList_, 3, wDetail > 80 ? wDetail : 80);
        ShowScrollBar(logList_, SB_HORZ, FALSE);
        UpdateLogScrollbar();
        // 本分支在函数末尾的 intro 隐藏逻辑之前 return；日志页的原生列表/
        // 下拉/导出按钮必须在入场动画期间保持隐藏，否则半透明内容上会突然
        // 弹出全不透明的控件。
        if (introActive_) HideContentChildren();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (audio) {
        // 音频面板展开时设备页卡片不可见；清掉旧矩形，避免动画定时器/局部重绘
        // 继续对已隐藏的区域 InvalidateRect。
        rcBattery_ = {};
        rcMiniChart_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // 设备页：内容头 + 元信息条 + 2 列卡片。
    const Device* d = Selected();
    rcMeta_ = {rcContent_.left + Scale(20), rcContentHeader_.bottom + Scale(2),
               rcContent_.right - Scale(20), rcContentHeader_.bottom + Scale(30)};
    int bodyLeft = rcContent_.left + Scale(20);
    int bodyRight = rcContent_.right - Scale(20);
    int bodyTop = rcMeta_.bottom + Scale(10);
    int bodyBottom = rcContent_.bottom - Scale(16);
    int gap = Scale(12);
    int colW = (bodyRight - bodyLeft - gap) / 2;
    int row1H = Scale(190);
    int row2Top = bodyTop + row1H + gap;
    int row2H = bodyBottom - row2Top;
    if (row2H < Scale(160)) row2H = Scale(160);

    bool showBattery = d && d->batteryApplicable;
    // 耳机没有 DPI/轮询/去抖这些可写参数：有电量时把右上、左下两格换成
    // “连接”和“设备信息”，避免显示两张“此设备不可提供…”的空卡。
    const bool headsetWithBattery = showBattery && d && d->kind == Kind::Headset;
    rcBattery_ = {};
    rcConn_ = {};
    rcInfo_ = {};
    rcMiniChart_ = {};
    rcDpi_ = {bodyLeft + colW + gap, bodyTop, bodyRight, bodyTop + row1H};
    if (showBattery) {
        rcBattery_ = {bodyLeft, bodyTop, bodyLeft + colW, bodyTop + row1H};
        rcPerf_ = {bodyLeft, row2Top, bodyLeft + colW, row2Top + row2H};
        rcMiniChart_ = {bodyLeft + colW + gap, row2Top, bodyRight, row2Top + row2H};
        if (headsetWithBattery) {
            rcConn_ = rcDpi_;   // 右上：连接
            rcInfo_ = rcPerf_;  // 左下：设备信息
        }
    } else {
        rcConn_ = {bodyLeft, bodyTop, bodyLeft + colW, bodyTop + row1H};
        rcPerf_ = {bodyLeft, row2Top, bodyLeft + colW, row2Top + row2H};
        rcInfo_ = {bodyLeft + colW + gap, row2Top, bodyRight, row2Top + row2H};
    }
    rcDevice_ = {bodyLeft, bodyTop, bodyRight, bodyBottom};

    // 音量卡数字输入：放在每行百分比的位置，右侧静音胶囊保持不变。
    const bool audioCard = dev && AudioCardAvailable();
    if (audioCard) {
        const int editW = Scale(48);
        const int editH = Scale(22);
        const int ex = rcInfo_.right - Scale(16) - Scale(66) - editW;
        const int ey = rcInfo_.top + Scale(32);
        MoveWindow(editVolDev_, ex, ey, editW, editH, TRUE);
        MoveWindow(editVolSys_, ex, ey + Scale(48), editW, editH, TRUE);
        // 系统输出端点下拉：位于两行音量条之后、设备端点说明之前。
        const int comboX = rcInfo_.left + Scale(16);
        const int comboW = rcInfo_.right - Scale(16) - comboX;
        MoveWindow(comboVolSys_, comboX, rcInfo_.top + Scale(150), comboW, Scale(200), TRUE);
        ShowWindow(editVolDev_, audioDevVol_ >= 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(editVolSys_, audioSysVol_ >= 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(comboVolSys_, !audioEndpoints_.empty() ? SW_SHOW : SW_HIDE);
        EnableWindow(editVolDev_, audioDevVol_ >= 0 && !audioDevId_.empty());
        EnableWindow(editVolSys_, audioSysVol_ >= 0 && !audioSysId_.empty());
        EnableWindow(comboVolSys_, !audioEndpoints_.empty());
        // 听力保护复选框：仅耳机端点且已触发警告时显示，位置与 PaintAudioCard 对齐。
        const bool protect = HeadsetDeviceTarget();
        const int protectTop = rcInfo_.top + Scale(244);
        if (protect && headsetLoudWarn_) {
            const bool twoLines = !headsetLoudOk_;
            const int chkW = rcInfo_.right - Scale(16) - (rcInfo_.left + Scale(16));
            MoveWindow(chkLoudOk_, rcInfo_.left + Scale(16),
                       protectTop + Scale(twoLines ? 34 : 20), chkW, Scale(24), TRUE);
            ShowWindow(chkLoudOk_, SW_SHOW);
        } else {
            ShowWindow(chkLoudOk_, SW_HIDE);
        }
    } else {
        ShowWindow(editVolDev_, SW_HIDE);
        ShowWindow(editVolSys_, SW_HIDE);
        ShowWindow(comboVolSys_, SW_HIDE);
        ShowWindow(chkLoudOk_, SW_HIDE);
    }

    // DPI 卡控件。
    int cx = rcDpi_.left + Scale(16);
    int cw = rcDpi_.right - rcDpi_.left - Scale(32);
    MoveWindow(editDpi_, rcDpi_.right - Scale(112), rcDpi_.top + Scale(34), Scale(88),
               Scale(28), TRUE);
    MoveWindow(slider_, cx, rcDpi_.top + Scale(88), cw, Scale(34), TRUE);
    int stageGap = Scale(6);
    int stageW = std::max(Scale(40), (cw - stageGap * 3) / 4);
    int stageTop = rcDpi_.top + Scale(120);
    for (int i = 0; i < (int)stageButtons_.size(); ++i) {
        int row = i / 4;
        int col = i % 4;
        MoveWindow(stageButtons_[i], cx + col * (stageW + stageGap),
                   stageTop + row * (Scale(28) + Scale(4)), stageW, Scale(28), TRUE);
    }

    // 性能卡控件（纵向排列，适配 2 列宽度）。
    int px = rcPerf_.left + Scale(16);
    int pw = std::min(Scale(160), (int)(rcPerf_.right - rcPerf_.left - Scale(32)));
    MoveWindow(comboPoll_, px, rcPerf_.top + Scale(52), pw, Scale(200), TRUE);
    MoveWindow(comboDeb_, px, rcPerf_.top + Scale(104), pw, Scale(200), TRUE);
    MoveWindow(chkAngle_, px, rcPerf_.top + Scale(148), Scale(140), Scale(24), TRUE);
    // 入场动画期间保持原生子控件隐藏：快照刷新 / WM_SIZE 也会走 Layout，
    // 不能让它们在内容还半透明时提前出现。
    if (introActive_) HideContentChildren();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EnsurePaintBuffer(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (paintMem_ && paintW_ == w && paintH_ == h) return;
    if (!paintMem_) {
        paintMem_ = CreateCompatibleDC(dc);
        if (!paintMem_) return;
    }
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    if (!bmp) return;  // 分配失败时继续用旧缓冲，至少还能画出旧尺寸内容
    if (paintBmp_) {
        SelectObject(paintMem_, paintOld_);
        DeleteObject(paintBmp_);
    }
    paintOld_ = (HBITMAP)SelectObject(paintMem_, bmp);
    paintBmp_ = bmp;
    paintW_ = w;
    paintH_ = h;
    // 新位图里没有上一帧内容：增量路径会漏掉未重画的区域，必须整帧一次。
    paintFresh_ = true;
}

void MainWindow::EnsureIntroBuffer(HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (introMem_ && introW_ == w && introH_ == h) return;
    if (!introMem_) {
        introMem_ = CreateCompatibleDC(dc);
        if (!introMem_) return;
    }
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    if (!bmp) return;
    if (introBmp_) {
        SelectObject(introMem_, introOld_);
        DeleteObject(introBmp_);
    }
    introOld_ = (HBITMAP)SelectObject(introMem_, bmp);
    introBmp_ = bmp;
    introW_ = w;
    introH_ = h;
}

void MainWindow::ReleaseDrawBuffers() {
    auto release = [](HDC& mem, HBITMAP& bmp, HBITMAP& old, int& w, int& h) {
        if (mem) {
            if (bmp) {
                SelectObject(mem, old);
                DeleteObject(bmp);
            }
            DeleteDC(mem);
        }
        mem = nullptr;
        bmp = nullptr;
        old = nullptr;
        w = 0;
        h = 0;
    };
    release(paintMem_, paintBmp_, paintOld_, paintW_, paintH_);
    release(introMem_, introBmp_, introOld_, introW_, introH_);
    ReleaseAudioSnapshot();
}

void MainWindow::Paint(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    Paint(dc, rc);
}

void MainWindow::Paint(HDC dc, const RECT& dirtyIn) {
    EnsureGdiplus();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    // 动画最高 120 Hz：复用持久缓冲，不再每帧 CreateCompatibleBitmap/DC。
    EnsurePaintBuffer(dc, rc.right, rc.bottom);
    // 缓冲区创建失败时尺寸不匹配，继续画会从越界位图取像素，直接放弃本帧。
    if (!paintMem_ || paintW_ != rc.right || paintH_ != rc.bottom) return;

    RECT dirty = dirtyIn;
    const bool haveDirty = paintregion::ClampToClient(dirty, rc);
    // 入场与音频面板滑动都要重新合成整块内容（离屏 alpha / 快照），
    // 缓冲刚重建时位图里没有旧像素：这几种情况只能整帧。
    const bool compositing = introActive_ || audioSliding_;
    const bool full = !haveDirty || paintFresh_ || compositing ||
                      paintregion::Decide(dirty, rc) == paintregion::Mode::Full;
    paintFresh_ = false;
    if (full) {
        ++paintFull_;
        PaintFullFrame(dc);
        return;
    }
    if (PaintDirty(dc, dirty)) {
        ++paintDirty_;
        const unsigned long long px =
            (unsigned long long)(dirty.right - dirty.left) * (dirty.bottom - dirty.top);
        paintDirtyPx_ += px;
        if (px < paintDirtyMinPx_) paintDirtyMinPx_ = px;
        if (px > paintDirtyMaxPx_) paintDirtyMaxPx_ = px;
        return;
    }
    // 背景缓存不可用时不能增量：半块背景上重画会露出上一帧内容。
    ++paintFull_;
    PaintFullFrame(dc);
}

void MainWindow::PaintFullFrame(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HDC mem = paintMem_;
    if (!mem || paintW_ != rc.right || paintH_ != rc.bottom) return;
    // 毛玻璃的“景深”底：渐变 + 柔光色斑。玻璃卡片都是半透明的，必须画在它前面。
    PaintBackdrop(mem, rc);
    // 整帧复用一个 GDI+ 上下文：玻璃卡片、图标、圆环一帧会画上百个路径，
    // 每个都 new Graphics 会让 CPU 飙到单核 70%+。
    svgicon::BeginFrame(mem);

    PaintNav(mem);
    const bool audio = view_ == View::Device && audioExpanded_;
    const bool dev = view_ == View::Device && !audioExpanded_;
    if (dev) PaintListPanel(mem);

    // introT_ 来自 Animator，Add 时已按 EmphasizedDecelerate 缓动，可直接当 eased 用。
    const float eased = std::max(0.0f, std::min(1.0f, introT_));
    const bool intro = introActive_ && eased < 1.0f;
    if (intro) EnsureIntroBuffer(dc, rc.right, rc.bottom);

    if (intro && introMem_ && !IsRectEmpty(&rcContent_)) {
        const int cw = rcContent_.right - rcContent_.left;
        const int ch = rcContent_.bottom - rcContent_.top;
        // a. 先把内容矩形里的真实背景（渐变 + 柔光色斑）拷入离屏，
        //    半透明卡片因此在离屏内合成出与直接绘制一致的底色。
        BitBlt(introMem_, rcContent_.left, rcContent_.top, cw, ch, mem,
               rcContent_.left, rcContent_.top, SRCCOPY);
        // b. 内容只画到离屏缓冲。
        PaintContent(introMem_);
        // c. 裁剪到内容矩形，用恒定 alpha 整块混合回主缓冲，并以 offset 上浮到位。
        const int alpha = ClampInt((int)std::lround(eased * 255.0f), 0, 255);
        const int offset = (int)std::lround((1.0f - eased) * (float)Scale(18));
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = (BYTE)alpha;
        bf.AlphaFormat = 0;
        int savedDc = SaveDC(mem);
        IntersectClipRect(mem, rcContent_.left, rcContent_.top, rcContent_.right,
                          rcContent_.bottom);
        AlphaBlend(mem, rcContent_.left, rcContent_.top + offset, cw, ch, introMem_,
                   rcContent_.left, rcContent_.top, cw, ch, bf);
        RestoreDC(mem, savedDc);
    } else {
        PaintContent(mem);
    }
    // 音频面板过渡：真实面板已隐藏，快照画在内容区之上并随 Animator 滑动/淡出。
    PaintAudioSnapshot(mem);
    PaintFooter(mem);
    svgicon::EndFrame();

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
}

// 增量路径：只重画 ps.rcPaint 覆盖的绘制块，再把这一块贴回窗口。
// 顺序与整帧路径完全一致（导航栏 → 设备列表 → 内容 → 页脚），并统一裁剪到脏区，
// 所以脏区内每个像素的合成结果与整帧重画相同；脏区外的像素本来就是上一帧结果。
bool MainWindow::PaintDirty(HDC dc, const RECT& dirty) {
    HDC mem = paintMem_;
    // 背景缓存里是整窗渐变：按脏区贴回一块，就是该区域正确的底。
    if (!mem || !PaintBackdropRegion(mem, dirty)) return false;

    const int saved = SaveDC(mem);
    IntersectClipRect(mem, dirty.left, dirty.top, dirty.right, dirty.bottom);
    // GDI+ 上下文按构造时的 HDC 裁剪区工作，裁剪必须在 BeginFrame 之前设好。
    svgicon::BeginFrame(mem);
    // 卡片阴影 3 px + 焦点框 2 px 会画到矩形外，判定时外扩，避免交界处留残影。
    const int pad = Scale(6);
    const bool audio = view_ == View::Device && audioExpanded_;
    const bool dev = view_ == View::Device && !audioExpanded_;
    if (paintregion::Touches(dirty, rcRail_, pad)) PaintNav(mem);
    if (dev && paintregion::Touches(dirty, rcListPane_, pad)) PaintListPanel(mem);
    if (paintregion::Touches(dirty, rcContent_, pad)) PaintContent(mem);
    if (paintregion::Touches(dirty, rcFooter_, pad)) PaintFooter(mem);
    svgicon::EndFrame();
    RestoreDC(mem, saved);

    BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, mem,
           dirty.left, dirty.top, SRCCOPY);
    return true;
}

// 内容区（导航/列表/页脚之外）的唯一绘制入口：整帧、增量与入场离屏都走这里。
void MainWindow::PaintContent(HDC dc) {
    PaintContentHeader(dc);
    if (view_ == View::Device && audioExpanded_) return;  // 音频面板是子窗口，自己绘制内容区
    if (view_ == View::Test) {
        testView_.Paint(dc);
        return;
    }
    if (view_ == View::Log) {
        PaintLogView(dc);
        return;
    }
    const Device* d = Selected();
    if (d) {
        PaintDeviceMeta(dc, *d);
        const bool headset = d->kind == Kind::Headset;
        // 耳机/匹配到播放端点的设备用“音量与音效”卡替代“设备信息”卡。
        auto infoOrAudio = [&]() {
            if (AudioCardAvailable()) {
                PaintAudioCard(dc, *d);
            } else {
                PaintInfoCard(dc, *d);
            }
        };
        if (d->batteryApplicable) {
            PaintBatteryCard(dc, *d);
        } else {
            PaintConnCard(dc, *d);
        }
        if (headset && d->batteryApplicable) {
            // 耳机专用排布：电量 | 连接 / 音量与音效 | 电量曲线。
            PaintConnCard(dc, *d);
            infoOrAudio();
            PaintMiniChart(dc);
        } else {
            PaintDpiCard(dc, *d);
            PaintPerfCard(dc, *d);
            if (d->batteryApplicable) {
                PaintMiniChart(dc);
            } else {
                infoOrAudio();
            }
        }
    } else {
        PaintEmptyState(dc);
    }
}

void MainWindow::PaintNav(HDC dc) {
    // 导航栏也做成玻璃：半透明白叠在渐变底上，和内容卡片保持同一种材质。
    GlassFill(dc, rcRail_, 0, 168);
    HPEN border = CreatePen(PS_SOLID, 1, RGB(228, 232, 238));
    HGDIOBJ oldPen = SelectObject(dc, border);
    MoveToEx(dc, rcRail_.right - 1, 0, nullptr);
    LineTo(dc, rcRail_.right - 1, rcRail_.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    RECT logo{rcRail_.left + Scale(20), Scale(18), rcRail_.left + Scale(56), Scale(54)};
    FillRounded(dc, logo, Scale(9), kAccent, kAccent);
    DrawTextIn(dc, L"2.4G", logo, fontBadge_, RGB(255, 255, 255),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const wchar_t* labels[3] = {L"设备", L"日志", L"测试"};
    // 选中胶囊用独立动画轨道平移，避免切换视图时突然跳变。
    int indTop = (int)std::lround(navIndicatorY_);
    int indH = rcNavItems_[0].bottom - rcNavItems_[0].top;
    // 果冻拉伸以中心为基准，上下各扩 stretch/2，胶囊不会整体偏移。
    const int stretch = (int)std::lround(navStretch_);
    RECT ind{rcNavItems_[0].left, indTop - stretch / 2, rcNavItems_[0].right,
             indTop + indH + stretch / 2};
    if (indTop > 0) {
        FillRounded(dc, ind, Scale(8), kAccentSoft, kAccentSoft);
        RECT bar{ind.left + Scale(2), ind.top + Scale(12), ind.left + Scale(5),
                 ind.bottom - Scale(12)};
        FillRounded(dc, bar, Scale(2), kAccent, kAccent);
    }
    for (int i = 0; i < 3; ++i) {
        const RECT& r = rcNavItems_[i];
        bool selected = (int)view_ == i;
        if (!selected && navHover_[i] > 0.01f) {
            // 悬停做 120 ms 状态层渐变，指针扫过时不会闪烁。
            COLORREF hv = LerpColor(RGB(247, 248, 250), kHover, navHover_[i]);
            FillRounded(dc, r, Scale(8), hv, hv);
        }
        COLORREF fg = selected ? kAccent : kSub;
        int iconSize = Scale(20);
        int ix = r.left + (r.right - r.left - iconSize) / 2;
        int iy = r.top + Scale(6);
        RECT icon{ix, iy, ix + iconSize, iy + iconSize};
        if (i == 0) {
            svgicon::Draw(dc, svgicon::Id::NavDevice, icon, fg, 0.0f, selected);
        } else if (i == 1) {
            svgicon::Draw(dc, svgicon::Id::NavLog, icon, fg, 0.0f, selected);
        } else {
            svgicon::Draw(dc, svgicon::Id::NavTest, icon, fg, 0.0f, selected);
        }
        RECT label{r.left, r.top + Scale(28), r.right, r.bottom - Scale(2)};
        DrawTextIn(dc, labels[i], label, selected ? fontBodyBold_ : fontSmall_, fg,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // 键盘焦点停在导航栏时给当前项画系统虚线框；只在主窗口自身持有焦点时显示，
    // 焦点进入原生控件后由 WM_KILLFOCUS / PARENTNOTIFY 重绘去掉。
    if (GetFocus() == hwnd_ && focusNav_ >= 0 && focusNav_ < 3) {
        RECT fr = rcNavItems_[focusNav_];
        InflateRect(&fr, -Scale(2), -Scale(2));
        DrawFocusRect(dc, &fr);
    }
}

void MainWindow::PaintListPanel(HDC dc) {
    // 列表底板比卡片更实一些：设备清单是 ListBox 自绘，底色必须接近实际值才不露缝。
    GlassFill(dc, rcListPane_, 0, 236);
    HPEN border = CreatePen(PS_SOLID, 1, RGB(228, 232, 238));
    HGDIOBJ oldPen = SelectObject(dc, border);
    MoveToEx(dc, rcListPane_.right - 1, 0, nullptr);
    LineTo(dc, rcListPane_.right - 1, rcListPane_.bottom);
    MoveToEx(dc, rcListPane_.left, Scale(48), nullptr);
    LineTo(dc, rcListPane_.right, Scale(48));
    SelectObject(dc, oldPen);
    DeleteObject(border);

    RECT title{rcListPane_.left + Scale(16), Scale(10), rcListPane_.right - Scale(16),
               Scale(38)};
    DrawTextIn(dc, L"设备", title, fontTitle_, kText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::wstring count = std::to_wstring(snapshot_.devices.size());
    int tw = (int)count.size() * Scale(7) + Scale(12);
    RECT badge{rcListPane_.right - Scale(16) - tw, Scale(16), rcListPane_.right - Scale(16),
               Scale(34)};
    FillRounded(dc, badge, Scale(9), kHover, kHover);
    DrawTextIn(dc, count, badge, fontBadge_, kSub,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void MainWindow::PaintContentHeader(HDC dc) {
    HBRUSH panel = CreateSolidBrush(kCard);
    FillRect(dc, &rcContentHeader_, panel);
    DeleteObject(panel);
    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(dc, border);
    // 分隔线只画在卡片网格的宽度上：原来从内容区最左画到最右，
    // 比卡片左右各多出 20px，视觉上就是“条子没对齐”。
    const int gridL = rcContent_.left + Scale(20);
    const int gridR = rcContent_.right - Scale(20);
    MoveToEx(dc, gridL, rcContentHeader_.bottom - 1, nullptr);
    LineTo(dc, gridR, rcContentHeader_.bottom - 1);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    std::wstring title;
    std::wstring sub;
    if (view_ == View::Log) {
        title = L"日志与电量曲线";
        sub = Fmt(L"最近 %d 天 · 1–7 天尺度 · 底部滚动条左右查看", logDays_);
    } else if (view_ == View::Test) {
        title = testView_.title();
        sub = testView_.status_note();
    } else if (audioExpanded_) {
        title = L"音频控制";
        const Device* d = Selected();
        sub = d ? Fmt(L"当前设备：%s · 输出/输入/应用音量", d->name.c_str())
                : L"系统输出 · 应用音量 · 麦克风";
        if (demo_) sub += L" · 演示模式";
    } else {
        const Device* d = Selected();
        title = d ? d->name : L"设备";
        if (d) {
            // 蓝牙音频没有 HID VID/PID，显示 0000/0000 会让人误以为设备无标识。
            sub = d->proto == Proto::BluetoothAudio
                      ? L"蓝牙音频设备 · A2DP/HFP · 已连接"
                      : Fmt(L"VID_%04X  PID_%04X · %s", d->vid, d->pid,
                            FriendlyProtocolName(*d).c_str());
        } else {
            sub = L"选择左侧设备查看详情";
        }
    }
    RECT tr{rcContentHeader_.left + Scale(20), rcContentHeader_.top + Scale(5),
            rcContentHeader_.right - Scale(260), rcContentHeader_.top + Scale(29)};
    DrawTextIn(dc, title, tr, fontTitle_, kText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr{rcContentHeader_.left + Scale(20), rcContentHeader_.top + Scale(29),
            view_ == View::Test ? testView_.header_status_right()
                                : rcContentHeader_.right - Scale(260),
            rcContentHeader_.bottom - Scale(2)};
    DrawTextIn(dc, sub, sr, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MainWindow::PaintDeviceMeta(HDC dc, const Device& d) {
    int x = rcMeta_.left;
    int y = rcMeta_.top + Scale(1);
    int bh = Scale(20);
    x += DrawPill(dc, x, y, KindName(d.kind), fontBadge_, kAccentSoft, kAccent, bh) + Scale(6);
    x += DrawPill(dc, x, y, DeviceConnLabel(d), fontBadge_, RGB(236, 253, 245), kGreen, bh) +
         Scale(6);
    x += DrawPill(dc, x, y, FriendlyProtocolName(d), fontBadge_, RGB(243, 244, 246), kMuted,
                  bh) +
         Scale(6);
    COLORREF sc = d.online ? (d.stale ? kAmber : kGreen) : RGB(156, 163, 175);
    x += DrawPill(dc, x, y, DeviceStatusText(d), fontBadge_, RGB(249, 250, 251), sc, bh);
    RECT note{x + Scale(12), rcMeta_.top, rcMeta_.right, rcMeta_.bottom};
    if (!d.note.empty()) {
        DrawTextIn(dc, d.note, note, fontSmall_, kMuted,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

bool MainWindow::AudioCardAvailable() const {
    const Device* d = Selected();
    if (!d) return false;
    // 耳机一定有这张卡；其它设备只在名称匹配到播放端点时显示。
    if (d->kind != Kind::Headset && !audioMatched_) return false;
    return !audioDevId_.empty() || !audioSysId_.empty();
}

void MainWindow::RefreshAudioState() {
    audioEndpoints_ = audio::ListRenderEndpoints();
    audioMatched_ = false;
    audioDevId_.clear();
    audioDevVol_ = -1;
    audioDevMute_ = false;
    audioSysId_.clear();
    audioSysVol_ = -1;
    audioSysMute_ = false;

    // “系统输出”默认跟随系统默认端点；用户在下拉里固定了端点时优先使用固定值。
    int sysIdx = audio::ResolveOutputEndpoint(audioEndpoints_, audioSysPinnedId_);
    if (sysIdx >= 0 && !audioSysPinnedId_.empty() &&
        audioEndpoints_[sysIdx].id != audioSysPinnedId_) {
        // 固定端点已消失（拔出/停用）：回到跟随系统默认，不保留失效 id。
        audioSysPinnedId_.clear();
        sysIdx = audio::ResolveOutputEndpoint(audioEndpoints_, audioSysPinnedId_);
    }
    if (sysIdx >= 0) {
        audioSysId_ = audioEndpoints_[sysIdx].id;
        audioSysVol_ = audioEndpoints_[sysIdx].volume;
        audioSysMute_ = audioEndpoints_[sysIdx].mute;
    }

    const Device* d = Selected();
    if (!d) return;
    const int idx = audio::MatchEndpoint(d->name, d->product, audioEndpoints_);
    if (idx >= 0) {
        audioMatched_ = true;
        audioDevId_ = audioEndpoints_[idx].id;
        audioDevVol_ = audioEndpoints_[idx].volume;
        audioDevMute_ = audioEndpoints_[idx].mute;
    } else if (d->kind == Kind::Headset) {
        // 耳机没匹配到端点（型号与端点名差异大）时退回系统默认输出，
        // 同时把“未匹配”的状态显示出来，避免用户以为调错了设备。
        audioDevId_ = audioSysId_;
        audioDevVol_ = audioSysVol_;
        audioDevMute_ = audioSysMute_;
    }
    RebuildSysEndpointCombo();
    // 当前已经在推荐值以上（外部设置或切换设备）时，直接显示警告与同意框。
    if (HeadsetDeviceTarget() && !headsetLoudOk_) {
        const bool systemLoud = HeadsetSystemTarget() &&
                                audioSysVol_ > audio::kRecommendedHeadsetVolume;
        if (audioDevVol_ > audio::kRecommendedHeadsetVolume || systemLoud) {
            headsetLoudWarn_ = true;
        }
    }
    SyncVolumeEdits();
}

void MainWindow::RebuildSysEndpointCombo() {
    if (!comboVolSys_) return;
    // 下拉正在展开时不重建，避免 5 s 刷新把用户正在选的列表关掉。
    if (SendMessageW(comboVolSys_, CB_GETDROPPEDSTATE, 0, 0)) return;

    std::wstring signature;
    for (const auto& e : audioEndpoints_) {
        signature += e.id;
        signature += L'\x1f';
        signature += e.name;
        signature += e.isDefault ? L'1' : L'0';
        signature += L'\x1e';
    }
    signature += L'#';
    signature += audioSysPinnedId_;
    if (signature == audioSysComboSig_) return;
    audioSysComboSig_ = signature;

    std::wstring defaultName;
    for (const auto& e : audioEndpoints_) {
        if (e.isDefault) {
            defaultName = e.name;
            break;
        }
    }
    if (defaultName.empty() && !audioEndpoints_.empty()) {
        defaultName = audioEndpoints_.front().name;
    }

    updatingControls_ = true;
    SendMessageW(comboVolSys_, CB_RESETCONTENT, 0, 0);
    audioSysChoices_.clear();

    std::wstring follow = L"跟随系统默认";
    if (!defaultName.empty()) follow += L" · " + defaultName;
    SendMessageW(comboVolSys_, CB_ADDSTRING, 0, (LPARAM)follow.c_str());
    audioSysChoices_.push_back(std::wstring());

    int selection = 0;
    for (const auto& e : audioEndpoints_) {
        std::wstring label = e.name;
        if (e.isDefault) label += L"（系统默认）";
        SendMessageW(comboVolSys_, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        audioSysChoices_.push_back(e.id);
        if (!audioSysPinnedId_.empty() && e.id == audioSysPinnedId_) {
            selection = (int)audioSysChoices_.size() - 1;
        }
    }
    SendMessageW(comboVolSys_, CB_SETCURSEL, (WPARAM)selection, 0);
    updatingControls_ = false;
}

void MainWindow::ApplyVolumeDrag(int x) {
    RECT bar = volDrag_ == 1 ? rcVolDev_ : rcVolSys_;
    const int w = bar.right - bar.left;
    if (w <= 0) return;
    const int pct = audio::ClampVolume((int)((x - bar.left) * 100.0 / w + 0.5));

    int target = pct;
    bool limited = false;
    if (volDrag_ == 1) {
        if (HeadsetDeviceTarget()) {
            target = audio::LimitHeadsetVolume(audioDevVol_, pct, headsetLoudOk_);
            limited = target != pct;
            if (limited) ShowHeadsetLoudWarning(true, pct);
        }
        if (audioDevVol_ == target && !limited) return;
        if (!limited) headsetLoudPending_ = -1;
        audioDevVol_ = target;
        audioDevMute_ = false;  // 调音量时自动取消静音，符合系统音量条的行为
        audio::SetVolume(audioDevId_, target);
        audio::SetMute(audioDevId_, false);
        if (audioDevId_ == audioSysId_) {
            audioSysVol_ = target;
            audioSysMute_ = false;
        }
    } else {
        if (HeadsetSystemTarget()) {
            target = audio::LimitHeadsetVolume(audioSysVol_, pct, headsetLoudOk_);
            limited = target != pct;
            if (limited) ShowHeadsetLoudWarning(false, pct);
        }
        if (audioSysVol_ == target && !limited) return;
        if (!limited) headsetLoudPending_ = -1;
        audioSysVol_ = target;
        audioSysMute_ = false;
        audio::SetVolume(audioSysId_, target);
        audio::SetMute(audioSysId_, false);
        if (audioDevId_ == audioSysId_) {
            audioDevVol_ = target;
            audioDevMute_ = false;
        }
    }
    SyncVolumeEdits();
    InvalidateRect(hwnd_, &rcInfo_, FALSE);
}

void MainWindow::OnVolumeEditChanged(bool device) {
    if (updatingControls_) return;
    HWND edit = device ? editVolDev_ : editVolSys_;
    const std::wstring id = device ? audioDevId_ : audioSysId_;
    if (!edit || id.empty()) return;

    wchar_t buf[16] = {};
    GetWindowTextW(edit, buf, ARRAYSIZE(buf));
    if (!buf[0]) return;  // 清空输入框时不要立刻归零，等失焦/Enter 归一化
    const int fallback = device ? audioDevVol_ : audioSysVol_;
    int v = audio::ParseVolumePercent(buf, fallback);
    if (v < 0) return;

    int target = v;
    bool limited = false;
    if (device && HeadsetDeviceTarget()) {
        target = audio::LimitHeadsetVolume(audioDevVol_, v, headsetLoudOk_);
        limited = target != v;
        if (limited) ShowHeadsetLoudWarning(true, v);
    } else if (!device && HeadsetSystemTarget()) {
        target = audio::LimitHeadsetVolume(audioSysVol_, v, headsetLoudOk_);
        limited = target != v;
        if (limited) ShowHeadsetLoudWarning(false, v);
    }

    if (device) {
        if (audioDevVol_ == target && !limited) return;
        if (!limited) headsetLoudPending_ = -1;
        audioDevVol_ = target;
        audioDevMute_ = false;
        audio::SetVolume(id, target);
        audio::SetMute(id, false);
        if (id == audioSysId_) {
            audioSysVol_ = target;
            audioSysMute_ = false;
        }
    } else {
        if (audioSysVol_ == target && !limited) return;
        if (!limited) headsetLoudPending_ = -1;
        audioSysVol_ = target;
        audioSysMute_ = false;
        audio::SetVolume(id, target);
        audio::SetMute(id, false);
        if (id == audioDevId_) {
            audioDevVol_ = target;
            audioDevMute_ = false;
        }
    }
    if (limited) {
        // 输入框正在编辑，SyncVolumeEdits 不会覆盖它；这里立即显示实际写入值。
        updatingControls_ = true;
        SetWindowTextW(edit, std::to_wstring(target).c_str());
        updatingControls_ = false;
        SendMessageW(edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    }
    SyncVolumeEdits();
    InvalidateRect(hwnd_, &rcInfo_, FALSE);
}

void MainWindow::NormalizeVolumeEdit(bool device) {
    HWND edit = device ? editVolDev_ : editVolSys_;
    if (!edit) return;
    const int vol = device ? audioDevVol_ : audioSysVol_;
    updatingControls_ = true;
    SetWindowTextW(edit, vol >= 0 ? std::to_wstring(vol).c_str() : L"");
    updatingControls_ = false;
}

void MainWindow::SyncVolumeEdits() {
    if (!editVolDev_ || !editVolSys_) return;
    const HWND focused = GetFocus();
    updatingControls_ = true;
    // 正在输入时不要被 5 s 设备刷新/拖动覆盖光标与半截数字；失焦时再归一化。
    if (focused != editVolDev_) {
        SetWindowTextW(editVolDev_,
                       audioDevVol_ >= 0 ? std::to_wstring(audioDevVol_).c_str() : L"");
    }
    if (focused != editVolSys_) {
        SetWindowTextW(editVolSys_,
                       audioSysVol_ >= 0 ? std::to_wstring(audioSysVol_).c_str() : L"");
    }
    EnableWindow(editVolDev_, audioDevVol_ >= 0 && !audioDevId_.empty());
    EnableWindow(editVolSys_, audioSysVol_ >= 0 && !audioSysId_.empty());
    if (comboVolSys_) EnableWindow(comboVolSys_, !audioEndpoints_.empty());
    updatingControls_ = false;
}

bool MainWindow::HeadsetDeviceTarget() const {
    const Device* d = Selected();
    return d && d->kind == Kind::Headset && audioMatched_ && !audioDevId_.empty();
}

bool MainWindow::HeadsetSystemTarget() const {
    return HeadsetDeviceTarget() && !audioSysId_.empty() && audioSysId_ == audioDevId_;
}

void MainWindow::ResetHeadsetLoudConsent() {
    headsetLoudOk_ = false;
    headsetLoudWarn_ = false;
    headsetLoudPending_ = -1;
    headsetLoudPendingSys_ = false;
    if (chkLoudOk_) {
        updatingControls_ = true;
        SendMessageW(chkLoudOk_, BM_SETCHECK, BST_UNCHECKED, 0);
        updatingControls_ = false;
    }
}

void MainWindow::ShowHeadsetLoudWarning(bool deviceRow, int attempted) {
    const bool firstTime = !headsetLoudWarn_;
    headsetLoudWarn_ = true;
    headsetLoudPending_ = audio::ClampVolume(attempted);
    headsetLoudPendingSys_ = !deviceRow;
    if (chkLoudOk_ && !headsetLoudOk_) {
        updatingControls_ = true;
        SendMessageW(chkLoudOk_, BM_SETCHECK, BST_UNCHECKED, 0);
        updatingControls_ = false;
    }
    // 首次出现警告时需要显示并摆放同意复选框；后续拖动不再重复布局。
    if (firstTime) Layout();
}

void MainWindow::ApplyHeadsetLoudPending() {
    if (headsetLoudPending_ < 0) return;
    const int value = audio::ClampVolume(headsetLoudPending_);
    const bool systemRow = headsetLoudPendingSys_;
    headsetLoudPending_ = -1;

    if (systemRow) {
        if (!HeadsetSystemTarget() || audioSysId_.empty()) return;
        audioSysVol_ = value;
        audioSysMute_ = false;
        audio::SetVolume(audioSysId_, value);
        audio::SetMute(audioSysId_, false);
        if (audioDevId_ == audioSysId_) {
            audioDevVol_ = value;
            audioDevMute_ = false;
        }
    } else {
        if (!HeadsetDeviceTarget() || audioDevId_.empty()) return;
        audioDevVol_ = value;
        audioDevMute_ = false;
        audio::SetVolume(audioDevId_, value);
        audio::SetMute(audioDevId_, false);
        if (audioDevId_ == audioSysId_) {
            audioSysVol_ = value;
            audioSysMute_ = false;
        }
    }
    SyncVolumeEdits();
    InvalidateRect(hwnd_, &rcInfo_, FALSE);
}

void MainWindow::PaintAudioCard(HDC dc, const Device& d) {
    (void)d;
    GlassPanel(dc, rcInfo_, Scale(8));
    DrawSectionTitle(dc, rcInfo_.left + Scale(16), rcInfo_.top + Scale(8), L"音量与音效",
                     fontBodyBold_);

    const int left = rcInfo_.left + Scale(16);
    const int right = rcInfo_.right - Scale(16);
    int y = rcInfo_.top + Scale(34);

    auto row = [&](const std::wstring& label, int vol, bool mute, RECT& bar, RECT& pill) {
        RECT lr{left, y, right - Scale(66), y + Scale(20)};
        DrawTextIn(dc, label, lr, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        // 0..100 的数值由 overlay EDIT 显示（可键盘输入），这里只在没有端点时
        // 画 "--"，避免自绘文字和原生编辑框叠在一起。
        if (vol < 0) {
            RECT pr{left, y, right - Scale(66), y + Scale(20)};
            DrawTextIn(dc, L"--", pr, fontBodyBold_, kText,
                       DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        pill = {right - Scale(56), y - Scale(2), right, y + Scale(20)};
        FillRounded(dc, pill, Scale(10), mute ? kAccentSoft : kHover,
                    mute ? kAccentSoft : kHover);
        DrawTextIn(dc, mute ? L"已静音" : L"静音", pill, fontSmall_,
                   mute ? kAccent : kSub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        y += Scale(24);
        bar = {left, y, right, y + Scale(10)};
        FillRounded(dc, bar, Scale(5), kTrack, kTrack);
        if (vol > 0) {
            RECT fill{bar.left, bar.top,
                      bar.left + (bar.right - bar.left) * audio::ClampVolume(vol) / 100,
                      bar.bottom};
            if (fill.right > fill.left) {
                FillRounded(dc, fill, Scale(5), kAccent, kAccent);
            }
        }
        y += Scale(24);
    };

    row(L"设备音量 (%)", audioDevVol_, audioDevMute_, rcVolDev_, rcMuteDev_);
    row(L"系统输出 (%)", audioSysVol_, audioSysMute_, rcVolSys_, rcMuteSys_);

    // 原生 ComboBox 覆盖在“系统输出端点”标题下方（位置见 Layout）。
    RECT sysEpLabel{left, y, right, y + Scale(18)};
    DrawTextIn(dc, L"系统输出端点", sysEpLabel, fontSmall_, kMuted,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += Scale(50);

    std::wstring epName;
    for (const auto& e : audioEndpoints_) {
        if (e.id == audioDevId_) {
            epName = e.name;
            break;
        }
    }
    std::wstring epLabel;
    if (!audioMatched_) {
        epLabel = L"设备端点：未匹配（回退系统默认）";
    } else if (epName.empty()) {
        epLabel = L"设备端点：未找到";
    } else {
        epLabel = L"设备端点：" + epName;
    }
    RECT ep{left, y, right, y + Scale(20)};
    DrawTextIn(dc, epLabel, ep, fontSmall_, kSub,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += Scale(24);

    RECT note{left, y, right, y + Scale(34)};
    const std::wstring hint =
        audioMatched_ ? L"设备音量写入该设备端点；系统输出写入上方选定的端点。"
                      : L"未匹配到该设备的音频端点，设备音量控制系统默认输出。";
    DrawTextIn(dc, hint, note, fontSmall_, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    y += Scale(40);

    // 耳机听力保护：推荐 ≤60%；超过时显示警告与同意复选框。
    const bool protect = HeadsetDeviceTarget();
    if (protect) {
        std::wstring text;
        COLORREF color = kSub;
        if (headsetLoudWarn_) {
            color = kAmber;
            text = headsetLoudOk_
                       ? L"已允许超过 60%：请控制聆听时长。"
                       : L"超过 60% 可能损伤听力；勾选下方同意后可继续调高。";
        } else {
            text = L"听力保护：建议耳机音量不超过 60%。";
        }
        const bool twoLines = headsetLoudWarn_ && !headsetLoudOk_;
        RECT pr{left, y, right, y + Scale(twoLines ? 34 : 20)};
        DrawTextIn(dc, text, pr, fontSmall_, color, DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += Scale(twoLines ? 34 : 20);
        // 复选框占位（具体位置见 Layout）。
        y += Scale(headsetLoudWarn_ ? 26 : 6);
    }

    rcAudioCenter_ = {left, y, left + (right - left) / 2 - Scale(6), y + Scale(24)};
    rcSoundLink_ = {rcAudioCenter_.right + Scale(12), y, right, y + Scale(24)};
    DrawTextIn(dc, L"音频控制台 ›", rcAudioCenter_, fontSmall_, kAccent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(dc, L"Windows 声音设置 ›", rcSoundLink_, fontSmall_, kAccent,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void MainWindow::PaintInfoCard(HDC dc, const Device& d) {
    GlassPanel(dc, rcInfo_, Scale(8));
    DrawSectionTitle(dc, rcInfo_.left + Scale(16), rcInfo_.top + Scale(8), L"设备信息",
                     fontBodyBold_);

    int ifaces = 0;
    const HidInterface* rep = nullptr;
    for (const auto& h : d.ifaces) {
        if (h.vid != d.vid || h.pid != d.pid) continue;
        ++ifaces;
        if (!rep || h.outLen + h.featLen > rep->outLen + rep->featLen) rep = &h;
    }

    int x = rcInfo_.left + Scale(16);
    int y = rcInfo_.top + Scale(38);
    int w = rcInfo_.right - Scale(16) - x;
    auto row = [&](const std::wstring& label, const std::wstring& value) {
        RECT l{x, y, x + Scale(64), y + Scale(22)};
        RECT v{x + Scale(68), y, x + w, y + Scale(22)};
        DrawTextIn(dc, label, l, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(dc, value, v, fontBody_, kText,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += Scale(24);
    };
    row(L"连接", DeviceConnLabel(d));
    row(L"厂商", d.manufacturer.empty() ? L"未提供" : d.manufacturer);
    if (d.proto == Proto::BluetoothAudio) {
        // 蓝牙音频没有 HID VID/PID；显示 0000/0000 会被误读成异常设备。
        row(L"标识", L"蓝牙音频（A2DP/HFP）");
        row(L"接口", L"无 HID 接口");
    } else {
        row(L"VID/PID", Fmt(L"%04X / %04X", d.vid, d.pid));
        // demo 设备没有真实 HID 接口；显示 0 会让人误以为设备异常。
        row(L"接口", ifaces > 0 ? Fmt(L"%d 个 HID 接口", ifaces)
                                : (demo_ ? L"演示数据未提供接口" : L"0 个 HID 接口"));
    }
    if (rep) {
        std::wstring usage = Fmt(L"page 0x%04X usage 0x%04X", rep->usagePage, rep->usage);
        row(L"用途", usage);
        row(L"报文", Fmt(L"in %u / out %u / feature %u", rep->inLen, rep->outLen,
                          rep->featLen));
    }
    // 接口列表：蓝牙/有线调试时最需要的信息（哪个 mi/col、用途、报文长度）。
    int listY = y + Scale(8);
    // 没有接口时不要留一个空标题。
    if (ifaces > 0 && listY + Scale(24) < rcInfo_.bottom - Scale(8)) {
        RECT label{x, listY, rcInfo_.right - Scale(16), listY + Scale(18)};
        DrawTextIn(dc, L"接口列表", label, fontSmall_, kMuted,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        listY += Scale(20);
        int shown = 0;
        for (const auto& h : d.ifaces) {
            if (h.vid != d.vid || h.pid != d.pid) continue;
            if (listY + Scale(18) > rcInfo_.bottom - Scale(8) || shown >= 6) break;
            std::wstring line =
                Fmt(L"mi=%d col=%d  %04X:%04X  in %u/out %u/feat %u", h.interfaceNumber,
                    h.collection, h.usagePage, h.usage, h.inLen, h.outLen, h.featLen);
            RECT r{x, listY, rcInfo_.right - Scale(16), listY + Scale(18)};
            DrawTextIn(dc, line, r, fontSmall_, kSub,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            listY += Scale(18);
            ++shown;
        }
    }
}


void MainWindow::PaintConnCard(HDC dc, const Device& d) {
    GlassPanel(dc, rcConn_, Scale(8));
    DrawSectionTitle(dc, rcConn_.left + Scale(16), rcConn_.top + Scale(8), L"连接",
                     fontBodyBold_);
    std::wstring transport = DeviceConnLabel(d);
    RECT big{rcConn_.left + Scale(16), rcConn_.top + Scale(38), rcConn_.right - Scale(16),
             rcConn_.top + Scale(72)};
    DrawTextIn(dc, transport, big, fontTitle_, kText,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int ifaces = 0;
    for (const auto& h : d.ifaces) {
        if (h.vid == d.vid && h.pid == d.pid) ++ifaces;
    }
    std::wstring line1 = d.manufacturer.empty() ? std::wstring(L"厂商未提供")
                                                : d.manufacturer;
    if (ifaces > 0) line1 += Fmt(L" · %d 个 HID 接口", ifaces);
    RECT l1{rcConn_.left + Scale(16), rcConn_.top + Scale(78), rcConn_.right - Scale(16),
            rcConn_.top + Scale(102)};
    DrawTextIn(dc, line1, l1, fontBody_, kSub,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    std::wstring hint;
    if (d.proto == Proto::BluetoothAudio) {
        // 电量可能来自 Windows 蓝牙属性或耳机私有 SPP（只读）。直接使用协议层
        // 生成的 note，避免这里固定写“Windows 未提供电量”与真实读数矛盾。
        hint = d.note.empty() ? L"蓝牙音频设备（A2DP/HFP）" : d.note;
    } else if (d.conn == Conn::Bluetooth) {
        hint = d.batteryValid ? L"蓝牙 HID 连接 · 电量由设备或系统电池服务提供"
                              : L"蓝牙设备 · 电量由系统电池服务提供时才显示";
    } else if (d.conn == Conn::Wired) {
        hint = d.wirelessCapable
                   ? L"无线设备的有线模式 · 仍按无线设备显示电量与充电状态"
                   : L"USB 有线设备 · 无电池，不显示电量卡与电量曲线";
    } else {
        // 无线耳机/键盘也会显示这张卡，不能再套用“有线模式”的文案。
        hint = L"2.4G 无线连接 · 电量与充电状态取自设备自身的无线通道";
    }
    RECT l2{rcConn_.left + Scale(16), rcConn_.top + Scale(108), rcConn_.right - Scale(16),
            rcConn_.top + Scale(158)};
    DrawTextIn(dc, hint, l2, fontSmall_, kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void MainWindow::PaintBatteryCard(HDC dc, const Device& d) {
    GlassPanel(dc, rcBattery_, Scale(12));
    DrawSectionTitle(dc, rcBattery_.left + Scale(16), rcBattery_.top + Scale(8), L"电量",
                     fontBodyBold_);

    int cx = rcBattery_.left + Scale(72);
    int cy = rcBattery_.top + Scale(102);
    int radius = Scale(44);
    float displayPct = -1.0f;
    if (d.batteryValid) {
        double shown = ringShown_ >= 0 ? ringShown_ : (double)d.battery;
        // 弹簧过冲只体现在圆弧上；数字保持单调，避免出现 62→63→62 的假跳变。
        if (ringAnimating_) {
            shown = (ringTo_ >= ringFrom_) ? std::min(shown, ringTo_)
                                           : std::max(shown, ringTo_);
        }
        displayPct = std::max(0.0f, std::min(100.0f, (float)shown));
    }
    int displayInt = displayPct >= 0 ? (int)lround(displayPct) : -1;
    std::wstring ringText = displayInt >= 0 ? Fmt(L"%d%%", displayInt) : L"--";
    float pulse = 0.5f;
    if (SystemAnimationsEnabled() && d.charging) {
        double phase = std::fmod(NowMs(), 1400.0) / 1400.0;
        pulse = 0.5f + 0.5f * (float)std::sin(phase * 2.0 * 3.14159265358979323846);
    }
    HFONT ringFont = displayInt >= 100 ? fontRingSmall_ : fontRing_;
    DrawRingGauge(dc, cx, cy, radius, Scale(10), displayPct, d.charging,
                  d.stale || !d.batteryValid, ringFont, ringText, kText, pulse);

    int x = rcBattery_.left + Scale(152);
    int y = rcBattery_.top + Scale(38);
    std::wstring state;
    if (!d.batteryValid) state = d.stale ? L"上次读数" : L"等待电量读数";
    else if (d.charging) state = d.battery >= 100 ? L"已充满" : L"充电中";
    else if (!d.chargingKnown) state = L"充电状态未知";
    else if (d.stale) state = L"上次读数";
    else state = L"电池供电";
    RECT st{x, y, rcBattery_.right - Scale(16), y + Scale(28)};
    DrawTextIn(dc, state, st, fontBodyBold_,
               d.charging ? kGreen : (d.chargingKnown ? kText : kMuted),
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (d.charging) {
        SIZE sz{};
        SelectObject(dc, fontBodyBold_);
        GetTextExtentPoint32W(dc, state.c_str(), (int)state.size(), &sz);
        DrawLightning(dc, x + sz.cx + Scale(8), y + Scale(6), Scale(12), Scale(16), kGreen);
    }

    int infoY = y + Scale(34);
    std::wstring voltage = d.millivolts > 0 ? Fmt(L"%d mV", d.millivolts) : L"电压未提供";
    RECT vr{x, infoY, rcBattery_.right - Scale(16), infoY + Scale(24)};
    DrawTextIn(dc, voltage, vr, fontBody_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    std::wstring extra;
    if (d.stale) extra = L"设备休眠时的缓存值，移动鼠标可刷新";
    else if (d.charging) extra = L"正在通过 USB 充电";
    else if (!d.chargingKnown) extra = L"设备未提供充电状态，电量计可能冻结";
    else if (d.batteryValid) extra = L"实时读取 · 圆形电量仪表";
    else extra = L"设备休眠或电量暂不可读";

    RECT ex{x, infoY + Scale(30), rcBattery_.right - Scale(16), infoY + Scale(54)};
    // 9pt 后这行在电量卡里放不下，加省略号，避免从字符中间被硬裁掉。
    DrawTextIn(dc, extra, ex, fontSmall_, kMuted,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void MainWindow::PaintDpiCard(HDC dc, const Device& d) {
    GlassPanel(dc, rcDpi_, Scale(12));
    DrawSectionTitle(dc, rcDpi_.left + Scale(16), rcDpi_.top + Scale(8), L"DPI",
                     fontBodyBold_);
    if (!d.dpi.valid) {
        RECT r{rcDpi_.left + Scale(16), rcDpi_.top + Scale(46), rcDpi_.right - Scale(16),
               rcDpi_.top + Scale(104)};
        std::wstring msg = !d.dpi.note.empty()
                               ? d.dpi.note
                               : L"此设备不提供可读 DPI（键盘/耳机或协议未公开）";
        DrawTextIn(dc, msg, r, fontBody_, kMuted, DT_LEFT | DT_WORDBREAK);
        return;
    }
    std::wstring value = d.dpi.currentKnown ? std::to_wstring(dpiValue_) : L"档位表";
    RECT big{rcDpi_.left + Scale(16), rcDpi_.top + Scale(28), rcDpi_.left + Scale(220),
             rcDpi_.top + Scale(64)};
    DrawTextIn(dc, value, big, fontTitle_, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (d.dpi.currentKnown) {
        RECT rng{rcDpi_.left + Scale(16), rcDpi_.top + Scale(64), rcDpi_.right - Scale(118),
                 rcDpi_.top + Scale(82)};
        std::wstring range = d.dpi.min == d.dpi.max
                                 ? L"单档"
                                 : Fmt(L"%d – %d", d.dpi.min, d.dpi.max);
        DrawTextIn(dc, range, rng, fontSmall_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    } else {
        RECT note{rcDpi_.left + Scale(16), rcDpi_.top + Scale(66), rcDpi_.right - Scale(16),
                  rcDpi_.top + Scale(112)};
        DrawTextIn(dc, d.dpi.note.empty() ? L"当前档位不可读，仅显示设备内档位表" : d.dpi.note,
                   note, fontSmall_, kMuted, DT_LEFT | DT_WORDBREAK);
    }
    if (!d.dpi.canWrite && d.dpi.currentKnown) {
        RECT note{rcDpi_.left + Scale(16), rcDpi_.top + Scale(66), rcDpi_.right - Scale(16),
                  rcDpi_.top + Scale(112)};
        std::wstring msg = d.dpi.note.empty()
                               ? L"只读：该协议未提供安全的 DPI 写入路径"
                               : d.dpi.note;
        DrawTextIn(dc, msg, note, fontSmall_, kMuted, DT_LEFT | DT_WORDBREAK);
    }
    if (d.dpi.currentKnown && d.dpi.canWrite) {
        RECT label{rcDpi_.right - Scale(116), rcDpi_.top + Scale(10), rcDpi_.right - Scale(24),
                   rcDpi_.top + Scale(28)};
        DrawTextIn(dc, L"精确值", label, fontSmall_, kMuted,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
}

void MainWindow::PaintPerfCard(HDC dc, const Device& d) {
    GlassPanel(dc, rcPerf_, Scale(12));
    DrawSectionTitle(dc, rcPerf_.left + Scale(16), rcPerf_.top + Scale(8), L"性能与按键",
                     fontBodyBold_);
    bool any = d.polling.valid || d.debounce.valid || d.angle.valid;
    if (!any) {
        RECT r{rcPerf_.left + Scale(16), rcPerf_.top + Scale(40), rcPerf_.right - Scale(16),
               rcPerf_.top + Scale(86)};
        DrawTextIn(dc, L"此设备没有可调整的轮询率/按键参数", r, fontBody_, kMuted,
                   DT_LEFT | DT_WORDBREAK);
        return;
    }
    if (d.polling.valid) {
        RECT l{rcPerf_.left + Scale(16), rcPerf_.top + Scale(34), rcPerf_.right - Scale(16),
               rcPerf_.top + Scale(52)};
        DrawTextIn(dc, L"轮询率", l, fontSmall_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    if (d.debounce.valid) {
        RECT l{rcPerf_.left + Scale(16), rcPerf_.top + Scale(86), rcPerf_.right - Scale(16),
               rcPerf_.top + Scale(104)};
        DrawTextIn(dc, L"按键去抖", l, fontSmall_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
}

void MainWindow::PaintFooter(HDC dc) {
    // 页脚同样走玻璃：比卡片稍实，避免状态文字压在色斑上看不清。
    GlassFill(dc, rcFooter_, 0, 225);
    RECT line{0, rcFooter_.top, rcFooter_.right, rcFooter_.top + 1};
    HBRUSH lb = CreateSolidBrush(RGB(228, 232, 238));
    FillRect(dc, &line, lb);
    DeleteObject(lb);

    bool toastActive = !toast_.empty() && GetTickCount64() < toastUntil_;
    std::wstring left = toastActive ? toast_ : snapshot_.status;
    COLORREF color = toastActive ? (toastOk_ ? kGreen : kRed) : kMuted;
    RECT lr{Scale(76) + Scale(20), rcFooter_.top, rcFooter_.right - Scale(250),
            rcFooter_.bottom};
    if (toastActive) {
        // toast 进场：文字从下方 3px 滑入，颜色从页脚底色过渡到绿/红。
        const float v = std::max(0.0f, std::min(1.0f, toastShown_));
        const int dy = (int)std::lround((1.0f - v) * (float)Scale(3));
        lr.top += dy;
        lr.bottom += dy;
        color = LerpColor(kCard, color, v);
    }
    DrawTextIn(dc, left, lr, fontSmall_, color, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT rr{rcFooter_.right - Scale(250), rcFooter_.top, rcFooter_.right - Scale(18),
            rcFooter_.bottom};
    std::wstring right = notificationsEnabled_
                             ? Fmt(L"轮询 %d 秒 · 设置需手动应用", manager_.PollSeconds())
                             : Fmt(L"轮询 %d 秒 · 系统通知已关闭", manager_.PollSeconds());
    DrawTextIn(dc, right, rr, fontSmall_, notificationsEnabled_ ? kMuted : kAmber,
               DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

void MainWindow::PaintEmptyState(HDC dc) {
    RECT area{rcDevice_.left, rcDevice_.top, rcDevice_.right, rcPerf_.bottom};
    GlassPanel(dc, area, Scale(12));
    RECT icon{area.left + (area.right - area.left) / 2 - Scale(30), area.top + Scale(70),
              area.left + (area.right - area.left) / 2 + Scale(30), area.top + Scale(130)};
    DrawDeviceGlyph(dc, Kind::Mouse, icon, RGB(148, 163, 184), false);
    RECT t1{area.left, area.top + Scale(150), area.right, area.top + Scale(184)};
    DrawTextIn(dc, L"未发现兼容设备", t1, fontTitle_, kText,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT t2{area.left + Scale(30), area.top + Scale(188), area.right - Scale(30),
            area.top + Scale(250)};
    DrawTextIn(dc,
               L"请插入 2.4G 接收器/USB 有线设备，或打开蓝牙设备后重试。\n"
               L"支持 Logitech、ASUS ROG、Razer、Rapoo、Compx、ATK/VXE、LAMZU、G-Wolves 等。",
               t2, fontBody_, kMuted, DT_CENTER | DT_WORDBREAK);
}

// ---------------------------------------------------------------------------
// 日志视图（内嵌在主界面）
// ---------------------------------------------------------------------------

namespace {

const wchar_t* LogTypeLabel(const std::wstring& type) {
    if (type == L"CONNECT") return L"接入";
    if (type == L"DISCONNECT") return L"断开";
    if (type == L"BATTERY") return L"电量";
    if (type == L"CHARGE") return L"充电";
    if (type == L"SAMPLE") return L"采样";
    return type.c_str();
}

std::wstring LogSummary(const std::vector<BatterySample>& series, int days) {
    if (series.empty()) return L"";
    if (series.size() < 2) return Fmt(L"当前 %d%%", series.back().pct);
    int delta = series.back().pct - series.front().pct;
    std::wstring changed = delta >= 0 ? Fmt(L"+%d%%", delta) : Fmt(L"%d%%", delta);
    std::wstring text =
        Fmt(L"当前 %d%% · 最近 %d 天 %s", series.back().pct, days, changed.c_str());

    bool charging = false;
    for (const auto& sp : series) {
        if (sp.charging) {
            charging = true;
            break;
        }
    }
    if (!charging && series.size() >= 3) {
        double hours = (double)(series.back().epoch - series.front().epoch) / 3600.0;
        int drop = series.front().pct - series.back().pct;
        if (hours >= 2.0 && drop > 0) {
            double rate = (double)drop / hours;
            if (rate > 0.05) {
                int left = (int)((double)series.back().pct / rate);
                if (left > 0 && left < 999) text += Fmt(L" · 预计续航约 %d 小时", left);
            }
        }
    }
    return text;
}

const COLORREF kChartPalette[] = {kAccent, kGreen, kAmber, RGB(147, 51, 234)};

struct ChartSeries {
    std::wstring name;
    std::vector<BatterySample> points;
    COLORREF color = kAccent;
};

void ComputeYRange(const std::vector<ChartSeries>& seriesList, int64_t start, int64_t end,
                   int& yLo, int& yHi) {
    yLo = 0;
    yHi = 100;
    int lo = 101, hi = -1;
    for (const auto& s : seriesList) {
        for (const auto& p : s.points) {
            if (p.epoch < start || p.epoch > end) continue;
            lo = std::min(lo, p.pct);
            hi = std::max(hi, p.pct);
        }
    }
    if (hi < 0) return;
    int pad = std::max(2, (hi - lo) / 10);
    int lowV = std::max(0, lo - pad);
    int highV = std::min(100, hi + pad);
    yLo = (lowV / 5) * 5;
    yHi = ((highV + 4) / 5) * 5;
    if (yHi - yLo < 20) {
        int mid = (yLo + yHi) / 2;
        yLo = ((mid - 10) / 5) * 5;
        if (yLo < 0) yLo = 0;
        yHi = yLo + 20;
        if (yHi > 100) {
            yHi = 100;
            yLo = 80;
        }
    }
}

std::vector<ChartSeries> BuildChartSeries(const std::vector<EventRecord>& records,
                                          const std::vector<EventDevice>& devices,
                                          const std::wstring& identity) {
    std::vector<ChartSeries> seriesList;
    if (!identity.empty()) {
        ChartSeries s;
        s.points = BuildBatterySeries(records, identity);
        for (const auto& dev : devices) {
            if (dev.identity == identity) {
                s.name = dev.name;
                break;
            }
        }
        seriesList.push_back(std::move(s));
    } else {
        // 全部设备：最多画 4 条线，颜色区分。
        for (const auto& dev : devices) {
            if (seriesList.size() >= 4) break;
            ChartSeries s;
            s.name = dev.name;
            s.points = BuildBatterySeries(records, dev.identity);
            s.color = kChartPalette[seriesList.size() % ARRAYSIZE(kChartPalette)];
            seriesList.push_back(std::move(s));
        }
    }
    return seriesList;
}

// 日志视图曲线：按天数尺度绘制，内容超宽时由底部滚动条平移。
// 图表数据在滚动/重绘时会被反复请求；只有记录/设备/筛选变化时才重建。
struct ChartSeriesCache {
    const EventRecord* records = nullptr;
    size_t recordCount = 0;
    const EventDevice* devices = nullptr;
    size_t deviceCount = 0;
    std::wstring identity;
    std::vector<ChartSeries> series;
    bool valid = false;
};

ChartSeriesCache g_chartCache;

const std::vector<ChartSeries>& BuildChartSeriesCached(const std::vector<EventRecord>& records,
                                                       const std::vector<EventDevice>& devices,
                                                       const std::wstring& identity) {
    bool hit = g_chartCache.valid && g_chartCache.records == records.data() &&
               g_chartCache.recordCount == records.size() &&
               g_chartCache.devices == devices.data() &&
               g_chartCache.deviceCount == devices.size() &&
               g_chartCache.identity == identity;
    if (!hit) {
        g_chartCache.records = records.data();
        g_chartCache.recordCount = records.size();
        g_chartCache.devices = devices.data();
        g_chartCache.deviceCount = devices.size();
        g_chartCache.identity = identity;
        g_chartCache.series = BuildChartSeries(records, devices, identity);
        g_chartCache.valid = true;
    }
    return g_chartCache.series;
}


void DrawLogChart(HDC dc, const RECT& rc, UINT dpi, HFONT fontBold, HFONT fontSmall,
                  const std::vector<EventRecord>& records,
                  const std::vector<EventDevice>& devices, const std::wstring& identity,
                  int days, int scrollX) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    if (days < 1) days = 1;
    if (days > 7) days = 7;
    auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    GlassPanel(dc, rc, S(12));

    RECT title{rc.left + S(16), rc.top + S(8), rc.right - S(16), rc.top + S(34)};
    DrawTextIn(dc, Fmt(L"电量曲线 · 最近 %d 天", days), title, fontBold, kSub,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const std::vector<ChartSeries>& seriesList =
        BuildChartSeriesCached(records, devices, identity);

    {  // 摘要 / 图例
        if (!identity.empty()) {
            std::wstring summary = seriesList.empty() ? L"" : LogSummary(seriesList[0].points, days);
            if (!summary.empty()) {
                RECT sr{rc.left + S(300), rc.top + S(8), rc.right - S(16), rc.top + S(34)};
                DrawTextIn(dc, summary, sr, fontSmall, kMuted,
                           DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        } else if (!seriesList.empty()) {
            int x = rc.right - S(16);
            for (int i = (int)seriesList.size() - 1; i >= 0; --i) {
                SelectObject(dc, fontSmall);
                SIZE sz{};
                GetTextExtentPoint32W(dc, seriesList[i].name.c_str(),
                                      (int)seriesList[i].name.size(), &sz);
                int chipW = sz.cx + S(18);
                x -= chipW;
                if (x < rc.left + S(240)) break;
                HBRUSH b = CreateSolidBrush(seriesList[i].color);
                RECT dot{x + S(2), rc.top + S(15), x + S(10), rc.top + S(23)};
                FillRect(dc, &dot, b);
                DeleteObject(b);
                RECT tr{x + S(13), rc.top + S(8), x + chipW, rc.top + S(30)};
                DrawTextIn(dc, seriesList[i].name, tr, fontSmall, kMuted,
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                x -= S(8);
            }
        }
    }

    // 底部留出日期标签 + 横向滚动条的位置。
    RECT plot{rc.left + S(46), rc.top + S(44), rc.right - S(16), rc.bottom - S(46)};
    if (plot.right <= plot.left || plot.bottom <= plot.top) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    int64_t todayStart = EventEpoch(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
    int64_t start = todayStart - (int64_t)(days - 1) * 86400;
    int64_t end = todayStart + 86400;
    int viewW = plot.right - plot.left;
    int contentW = std::max(viewW, days * S(220));
    double pxPerSec = (double)contentW / ((double)days * 86400.0);
    auto X = [&](int64_t t) -> int {
        return plot.left - scrollX + (int)((double)(t - start) * pxPerSec);
    };

    int yLo = 0, yHi = 100;
    ComputeYRange(seriesList, start, end, yLo, yHi);
    auto Y = [&](int pct) -> int {
        int v = ClampInt(pct, yLo, yHi);
        return plot.bottom - (int)((double)(v - yLo) / (double)(yHi - yLo) *
                                   (plot.bottom - plot.top));
    };

    for (int i = 0; i <= 4; ++i) {
        int pct = yLo + (yHi - yLo) * i / 4;
        int y = plot.bottom - (plot.bottom - plot.top) * i / 4;
        HPEN pen = CreatePen(PS_SOLID, 1, i == 0 ? kBorder : RGB(238, 241, 245));
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, plot.left, y, nullptr);
        LineTo(dc, plot.right, y);
        SelectObject(dc, old);
        DeleteObject(pen);
        std::wstring label = Fmt(L"%d", pct);
        RECT lr{rc.left + S(8), y - S(9), plot.left - S(6), y + S(9)};
        DrawTextIn(dc, label, lr, fontSmall, kMuted,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    // 每天一条竖线；内容宽裕时补 6 小时辅助线。
    int bandW = (int)(contentW / days);
    bool minor = bandW >= S(320);
    for (int i = 0; i <= days; ++i) {
        int64_t t = start + (int64_t)i * 86400;
        int x = X(t);
        HPEN pen = CreatePen(PS_SOLID, 1, i == 0 ? kBorder : RGB(238, 241, 245));
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, x, plot.top, nullptr);
        LineTo(dc, x, plot.bottom);
        SelectObject(dc, old);
        DeleteObject(pen);
        if (i >= days) continue;  // 右边界不标日期
        if (minor) {
            for (int h = 6; h < 24; h += 6) {
                int mx = X(t + (int64_t)h * 3600);
                HPEN mp = CreatePen(PS_DOT, 1, RGB(244, 246, 249));
                HPEN mo = (HPEN)SelectObject(dc, mp);
                MoveToEx(dc, mx, plot.top, nullptr);
                LineTo(dc, mx, plot.bottom);
                SelectObject(dc, mo);
                DeleteObject(mp);
            }
        }
        int y = 0, m = 0, d = 0;
        if (!EventDateFromEpoch(t, y, m, d)) continue;
        if (bandW < S(44)) continue;
        RECT lr{x, plot.bottom + S(6), x + bandW, plot.bottom + S(24)};
        if (lr.left < plot.left) lr.left = plot.left;
        if (lr.right > plot.right) lr.right = plot.right;
        if (lr.right - lr.left < S(30)) continue;
        DrawTextIn(dc, Fmt(L"%02d/%02d", m, d), lr, fontSmall, kMuted,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    bool anyVisible = false;
    for (const auto& s : seriesList) {
        for (const auto& p : s.points) {
            if (p.epoch >= start && p.epoch <= end) {
                anyVisible = true;
                break;
            }
        }
        if (anyVisible) break;
    }
    if (!anyVisible) {
        RECT empty{plot.left, plot.top, plot.right, plot.bottom};
        DrawTextIn(dc, Fmt(L"最近 %d 天暂无电量记录", days), empty, fontBold, kMuted,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, plot.left, plot.top, plot.right, plot.bottom);
    EnsureGdiplus();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetClip(Gdiplus::Rect(plot.left, plot.top, plot.right - plot.left,
                            plot.bottom - plot.top));
    for (const auto& s : seriesList) {
        for (size_t i = 1; i < s.points.size(); ++i) {
            const BatterySample& a = s.points[i - 1];
            const BatterySample& b = s.points[i];
            if (b.epoch < start || a.epoch > end) continue;
            COLORREF color = s.color;
            if (identity.empty()) {
                // 全部设备模式用设备颜色区分，不再单独标充电段。
            } else if (a.charging || b.charging) {
                color = kGreen;
            }
            Gdiplus::Color c(255, GetRValue(color), GetGValue(color), GetBValue(color));
            Gdiplus::Pen pen(c, 2.0f);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pen, X(a.epoch), Y(a.pct), X(b.epoch), Y(b.pct));
        }
        for (const auto& sp : s.points) {
            if (sp.epoch < start || sp.epoch > end) continue;
            COLORREF color = identity.empty() || !sp.charging ? s.color : kGreen;
            Gdiplus::Color c(255, GetRValue(color), GetGValue(color), GetBValue(color));
            Gdiplus::SolidBrush brush(c);
            float x = (float)X(sp.epoch);
            float y = (float)Y(sp.pct);
            g.FillEllipse(&brush, x - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }
    g.Flush();
    RestoreDC(dc, saved);
}

// 设备页底部的 7 天迷你曲线（面积图）。
void DrawMiniChart(HDC dc, const RECT& rc, UINT dpi, HFONT fontBold, HFONT fontSmall,
                   const std::vector<BatterySample>& points) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    auto S = [&](int v) { return MulDiv(v, (int)dpi, 96); };
    GlassPanel(dc, rc, S(12));

    // 与同排的性能卡保持同一内边距（16/8），否则两张卡的标题会错开 2–4px。
    RECT title{rc.left + S(16), rc.top + S(8), rc.right - S(16), rc.top + S(30)};
    DrawTextIn(dc, L"电量曲线 · 最近 7 天", title, fontBold, kSub,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (!points.empty()) {
        RECT cr{rc.left + S(160), rc.top + S(8), rc.right - S(16), rc.top + S(30)};
        DrawTextIn(dc, Fmt(L"当前 %d%%", points.back().pct), cr, fontSmall, kMuted,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    RECT plot{rc.left + S(34), rc.top + S(32), rc.right - S(16), rc.bottom - S(8)};
    if (plot.right <= plot.left || plot.bottom <= plot.top) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    int64_t todayStart = EventEpoch(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
    int64_t start = todayStart - 6 * 86400;
    int64_t end = todayStart + 86400;

    ChartSeries tmp;
    tmp.points = points;
    int yLo = 0, yHi = 100;
    ComputeYRange({tmp}, start, end, yLo, yHi);
    auto X = [&](int64_t t) -> int {
        return plot.left + (int)((double)(t - start) / ((double)7 * 86400) *
                                 (plot.right - plot.left));
    };
    auto Y = [&](int pct) -> int {
        int v = ClampInt(pct, yLo, yHi);
        return plot.bottom - (int)((double)(v - yLo) / (double)(yHi - yLo) *
                                   (plot.bottom - plot.top));
    };

    for (int i = 0; i <= 2; ++i) {
        int pct = yLo + (yHi - yLo) * i / 2;
        int y = plot.bottom - (plot.bottom - plot.top) * i / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, i == 0 ? kBorder : RGB(238, 241, 245));
        HPEN old = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, plot.left, y, nullptr);
        LineTo(dc, plot.right, y);
        SelectObject(dc, old);
        DeleteObject(pen);
        RECT lr{rc.left + S(4), y - S(7), plot.left - S(4), y + S(7)};
        DrawTextIn(dc, Fmt(L"%d", pct), lr, fontSmall, kMuted,
                   DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    bool any = false;
    for (const auto& p : points) {
        if (p.epoch >= start && p.epoch <= end) {
            any = true;
            break;
        }
    }
    if (!any) {
        RECT empty{plot.left, plot.top, plot.right, plot.bottom};
        DrawTextIn(dc, L"最近 7 天暂无电量记录", empty, fontSmall, kMuted,
                   DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, plot.left, plot.top, plot.right, plot.bottom);
    EnsureGdiplus();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetClip(Gdiplus::Rect(plot.left, plot.top, plot.right - plot.left,
                            plot.bottom - plot.top));

    std::vector<BatterySample> visible;
    for (const auto& p : points) {
        if (p.epoch >= start && p.epoch <= end) visible.push_back(p);
    }
    if (!visible.empty()) {
        Gdiplus::GraphicsPath path;
        path.StartFigure();
        path.AddLine((float)X(visible.front().epoch), (float)Y(visible.front().pct),
                     (float)X(visible.front().epoch), (float)Y(visible.front().pct));
        for (size_t i = 1; i < visible.size(); ++i) {
            path.AddLine((float)X(visible[i - 1].epoch), (float)Y(visible[i - 1].pct),
                         (float)X(visible[i].epoch), (float)Y(visible[i].pct));
        }
        path.AddLine((float)X(visible.back().epoch), (float)plot.bottom,
                     (float)X(visible.front().epoch), (float)plot.bottom);
        path.CloseFigure();
        Gdiplus::SolidBrush fill(Gdiplus::Color(40, 37, 99, 235));
        g.FillPath(&fill, &path);

        for (size_t i = 1; i < visible.size(); ++i) {
            const BatterySample& a = visible[i - 1];
            const BatterySample& b = visible[i];
            COLORREF color = (a.charging || b.charging) ? kGreen : kAccent;
            Gdiplus::Color c(255, GetRValue(color), GetGValue(color), GetBValue(color));
            Gdiplus::Pen pen(c, 2.0f);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&pen, X(a.epoch), Y(a.pct), X(b.epoch), Y(b.pct));
        }
        Gdiplus::Color last(255, GetRValue(kAccent), GetGValue(kAccent), GetBValue(kAccent));
        Gdiplus::SolidBrush dot(last);
        g.FillEllipse(&dot, (float)X(visible.back().epoch) - 3.0f,
                      (float)Y(visible.back().pct) - 3.0f, 6.0f, 6.0f);
    }
    g.Flush();
    RestoreDC(dc, saved);
}

}  // namespace

void MainWindow::PaintLogView(HDC dc) {
    int m = rcContent_.left + Scale(20);
    DrawLogChart(dc, rcLogChart_, dpi_, fontBodyBold_, fontSmall_, logRecords_, logDevices_,
                 logIdentity_, logDays_, logScrollX_);
    RECT listLabel{m, logListLabelY_, rcContent_.right - Scale(20), logListLabelY_ + Scale(22)};
    std::wstring text = Fmt(L"鼠标活动日志 · 最近 %d 天（%d 条）", logDays_, logRowCount_);
    DrawTextIn(dc, text, listLabel, fontBodyBold_, kText,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (LogMaxScroll() > 0) DrawThinBar(dc, LogThumbRect());
}

void MainWindow::ReloadLogs() {
    // 用户正在展开筛选下拉框时不要 CB_RESETCONTENT：会强制关闭下拉并重置选择。
    // 下一个 5s 快照会再次尝试，关闭后再重建。
    if ((logCombo_ && SendMessageW(logCombo_, CB_GETDROPPEDSTATE, 0, 0)) ||
        (logDaysCombo_ && SendMessageW(logDaysCombo_, CB_GETDROPPEDSTATE, 0, 0))) {
        return;
    }
    int sel = (int)SendMessageW(logDaysCombo_, CB_GETCURSEL, 0, 0);
    logDays_ = ClampInt(sel >= 0 ? sel + 1 : 7, 1, 7);

    logRecords_ = LoadEventRecords(7);
    logDevices_ = CollectEventDevices(logRecords_);
    SendMessageW(logCombo_, CB_RESETCONTENT, 0, 0);
    int allIdx = (int)SendMessageW(logCombo_, CB_ADDSTRING, 0, (LPARAM)L"全部设备");
    if (allIdx >= 0) SendMessageW(logCombo_, CB_SETITEMDATA, allIdx, (LPARAM)-1);

    int select = 0;
    for (size_t i = 0; i < logDevices_.size(); ++i) {
        int idx = (int)SendMessageW(logCombo_, CB_ADDSTRING, 0,
                                    (LPARAM)logDevices_[i].name.c_str());
        if (idx >= 0) SendMessageW(logCombo_, CB_SETITEMDATA, idx, (LPARAM)i);
        if (!logIdentity_.empty() && logDevices_[i].identity == logIdentity_) {
            select = (int)i + 1;
        }
    }
    if (!logFilterReady_) {
        // 仅首次进入时匹配主界面当前设备；之后必须保留用户选择（包括“全部设备”），
        // 否则 10s 自动重载会把“全部设备”重置回当前设备。
        logFilterReady_ = true;
        if (logIdentity_.empty()) {
            if (const Device* d = Selected()) {
                std::wstring id = d->identity.empty() ? d->id : d->identity;
                for (size_t i = 0; i < logDevices_.size(); ++i) {
                    if (logDevices_[i].identity == id) {
                        select = (int)i + 1;
                        break;
                    }
                }
            }
        }
    }
    SendMessageW(logCombo_, CB_SETCURSEL, select, 0);
    ApplyLogSelection();
    logReloadAt_ = GetTickCount64();
}

void MainWindow::ApplyLogSelection() {
    int sel = (int)SendMessageW(logCombo_, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    int data = (int)SendMessageW(logCombo_, CB_GETITEMDATA, sel, 0);
    if (data < 0 || data >= (int)logDevices_.size()) {
        logIdentity_.clear();  // 全部设备（item 0 的 data 也是 -1）
    } else {
        logIdentity_ = logDevices_[(size_t)data].identity;
    }
    FillLogList();
    UpdateLogScrollbar();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::FillLogList() {
    if (!logList_) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int64_t todayStart = EventEpoch(st.wYear, st.wMonth, st.wDay, 0, 0, 0);

    // 每 10s 重载会整表重建 ListView；先记住当前首行与选中行的内容键，
    // 重建后按内容恢复，避免用户滚动/选中被跳回顶部。
    auto itemText = [&](int item, int col) {
        wchar_t buf[512] = {};
        ListView_GetItemText(logList_, item, col, buf, ARRAYSIZE(buf));
        return std::wstring(buf);
    };
    auto rowKey = [&](int item) {
        return itemText(item, 0) + L"|" + itemText(item, 1) + L"|" + itemText(item, 2) + L"|" +
               itemText(item, 3);
    };
    const int oldTop = ListView_GetTopIndex(logList_);
    const int oldSel = ListView_GetNextItem(logList_, -1, LVNI_SELECTED);
    // 空 ListView 的 GetTopIndex 可能返回 0；此时 rowKey(0) 是空串，不能当作
    // 真实顶部键，否则第一次重建会误判“顶部行已消失”。
    const bool hasOldRows = ListView_GetItemCount(logList_) > 0;
    const std::wstring topKey = (hasOldRows && oldTop >= 0) ? rowKey(oldTop) : std::wstring();
    const std::wstring selKey = (hasOldRows && oldSel >= 0) ? rowKey(oldSel) : std::wstring();

    SendMessageW(logList_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(logList_);
    int row = 0;
    int newTop = -1;
    int newSel = -1;
    for (int i = (int)logRecords_.size() - 1; i >= 0; --i) {
        const EventRecord& r = logRecords_[(size_t)i];
        if (!logIdentity_.empty() && r.identity != logIdentity_) continue;
        // SAMPLE 只用于曲线；活动日志只显示接入/断开/电量/充电。
        if (r.type == L"SAMPLE") continue;
        if (!EventWithinDays(r.epoch, logDays_, todayStart)) continue;
        std::wstring time = Fmt(L"%02d-%02d %02d:%02d:%02d", r.month, r.day, r.hour, r.minute,
                                r.second);
        const wchar_t* type = LogTypeLabel(r.type);
        std::wstring key = time + L"|" + type + L"|" + r.name + L"|" + r.detail;
        if (!topKey.empty() && newTop < 0 && key == topKey) newTop = row;
        if (!selKey.empty() && newSel < 0 && key == selKey) newSel = row;
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<LPWSTR>(time.c_str());
        int idx = (int)SendMessageW(logList_, LVM_INSERTITEMW, 0, (LPARAM)&item);
        ListView_SetItemText(logList_, idx, 1, const_cast<LPWSTR>(type));
        ListView_SetItemText(logList_, idx, 2, const_cast<LPWSTR>(r.name.c_str()));
        ListView_SetItemText(logList_, idx, 3, const_cast<LPWSTR>(r.detail.c_str()));
        ++row;
    }
    logRowCount_ = row;
    SendMessageW(logList_, WM_SETREDRAW, TRUE, 0);
    // 只恢复选中高亮，不恢复 LVIS_FOCUSED：设置焦点时 comctl32 会异步把
    // 选中行滚入视野，即使用户当前视口在别处（实测 top 25 被拉回 39）。
    if (newSel >= 0) {
        ListView_SetItemState(logList_, newSel, LVIS_SELECTED, LVIS_SELECTED);
    }
    // 再用 EnsureVisible + 逐行校正恢复顶部；内容消失时保持默认（顶部）。
    // 必须包含 newTop==0：旧视口在第 0 行时，恢复选中行（例如第 23 行）会让
    // comctl32 把视口滚到 14 行；原条件 newTop>0 会跳过第 0 行的恢复。
    if (newTop >= 0) {
        ListView_EnsureVisible(logList_, newTop, FALSE);
        for (int guard = 0; guard < 64; ++guard) {
            const int top = ListView_GetTopIndex(logList_);
            if (top == newTop) break;
            const WPARAM code = top < newTop ? SB_LINEDOWN : SB_LINEUP;
            SendMessageW(logList_, WM_VSCROLL, code, 0);
        }
    }
    // 行内容变化后 ListView 可能重新显示水平滚动条；列宽已留余量，这里强制隐藏。
    ShowScrollBar(logList_, SB_HORZ, FALSE);
    InvalidateRect(logList_, nullptr, TRUE);
}

// 仅供 SetView 内部使用：外部（导航、托盘菜单、迷你曲线）必须走 SetView，
// 否则导航胶囊不会启动补间。
void MainWindow::SetLogView(bool on) {
    if (on && view_ == View::Test) {
        testView_.Show(hwnd_, false);
        KillTimer(hwnd_, 3);
    }
    view_ = on ? View::Log : View::Device;
    if (on) {
        // 进入日志视图时优先匹配主界面当前选中的设备（“在主界面上匹配查看”）。
        // 启动初期快照还没到时，退回第一个设备，避免默认落在“全部设备”。
        const Device* match = Selected();
        if (!match && !snapshot_.devices.empty()) match = &snapshot_.devices.front();
        logIdentity_ = match ? (match->identity.empty() ? match->id : match->identity)
                             : std::wstring();
        logFilterReady_ = false;
        logScrollX_ = 1 << 30;  // 默认滚到最右（最近数据），UpdateLogScrollbar 收敛
        // 先布局再重载：LogMaxScroll() 依赖 rcLogChart_，首次进入日志视图时几何尚未建立，
        // 若此时重载会被错误夹到 0，导致默认停在最旧数据、最近数据落在画幅外。
        Layout();
        ReloadLogs();
    } else {
        UpdateMiniSeries();
        // 从日志页返回设备页：恢复按设备能力决定控件可见性。
        UpdateControlsFromDevice();
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// 仅供 SetView 内部使用，原因同上。
void MainWindow::SetTestView(bool on) {
    if (on) {
        view_ = View::Test;
        testView_.SetDpi(dpi_);
        // 演示模式不注册 Raw Input，避免真实鼠标输入污染演示数据。
        testView_.SetDemoMode(demo_);
        testView_.Show(hwnd_, true);
        testView_.SetDevices(snapshot_.devices, selectedId_);
        SetTimer(hwnd_, 3, 50, nullptr);
    } else {
        testView_.Show(hwnd_, false);
        KillTimer(hwnd_, 3);
        view_ = View::Device;
        // 恢复设备页控件；只读设备的「应用设置」由 UpdateControlsFromDevice 决定。
        UpdateControlsFromDevice();
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ExpandAudio() {
    if (testView_.visible()) {
        testView_.Show(hwnd_, false);
        KillTimer(hwnd_, 3);
        view_ = View::Device;
    }
    if (view_ == View::Log) SetLogView(false);
    view_ = View::Device;
    // 内容入场未结束时先收敛，避免和音频面板的布局/快照抢占同一帧。
    if (introActive_) FinishContentIntro();
    if (audioExpanded_ && !audioSliding_) return;

    audioExpanded_ = true;
    // 先阻止 Layout 重设面板位置，等确定展开态内容区后再处理快照。
    audioSliding_ = true;
    Layout();
    audioPanelRect_ = {rcContent_.left, rcContentHeader_.bottom, rcContent_.right,
                       rcContent_.bottom};

    if (audioSnapValid_) {
        // 退场中再次展开：快照仍然有效，从当前进度反向滑动/淡入。
        StartAudioSlide(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    audiocenter::Context ctx;
    if (const Device* d = Selected()) {
        ctx.deviceName = d->name;
        ctx.product = d->product;
    }
    ctx.renderEndpointId = audioDevId_;
    ctx.demo = demo_;

    // 首次展开：面板先放到目标位置截图，再立即隐藏；PrintWindow 对窗口外
    // 的子窗口可能返回黑图，所以不能先摆到 client.right 外再截。
    if (!audiocenter::ShowPanel(hwnd_, audioPanelRect_, ctx)) {
        // 子窗口创建失败时不能留下“audioExpanded_=true 但内容区空白、
        // 也没有返回按钮”的状态；回滚到设备页。
        audioExpanded_ = false;
        audioSliding_ = false;
        Layout();
        UpdateControlsFromDevice();
        ShowToast(L"无法打开音频面板", false);
        return;
    }
    const bool captured = CaptureAudioPanelSnapshot();
    audiocenter::SetPanelVisible(false);
    if (!captured) {
        // 快照失败时不做动画，直接把真实面板放到目标位置。
        audioSliding_ = false;
        audioSlideT_ = 0.0f;
        audioSlideAlpha_ = 1.0f;
        audiocenter::SetPanelBounds(audioPanelRect_);
        audiocenter::SetPanelVisible(true);
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    audioSlideT_ = 1.0f;
    audioSlideAlpha_ = 0.0f;
    StartAudioSlide(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CollapseAudio() {
    if (!audioExpanded_ && !audioSliding_) return;
    if (audioSliding_ && audioSnapValid_) {
        // 进场中/已在退场：沿用快照，从当前进度继续滑出；设备页同时淡入。
        audioExpanded_ = false;
        Layout();
        StartContentIntro();
        InvalidateRect(hwnd_, nullptr, FALSE);
        StartAudioSlide(false);
        return;
    }
    if (!audiocenter::PanelVisible()) {
        audioExpanded_ = false;
        audioSliding_ = false;
        ReleaseAudioSnapshot();
        Layout();
        UpdateControlsFromDevice();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // 稳态展开：先截快照、隐藏真实面板，再恢复设备页布局；快照滑出时逐步露出。
    // 设备页同时走 420 ms 内容入场（原生控件隐藏，92% 时恢复），避免原生控件
    // 在半透明快照上方“穿透”。
    const bool captured = CaptureAudioPanelSnapshot();
    audiocenter::SetPanelVisible(false);
    audioExpanded_ = false;
    audioSliding_ = true;
    Layout();
    StartContentIntro();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!captured) {
        FinishAudioSlide(false);
        return;
    }
    StartAudioSlide(false);
}

void MainWindow::InvalidateAudioSlide() {
    RECT invalid{};
    UnionRect(&invalid, &audioPanelRect_, &rcContent_);
    if (IsRectEmpty(&invalid)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        InvalidateRect(hwnd_, &invalid, FALSE);
    }
}

void MainWindow::StartAudioSlide(bool expanding) {
    anim_.Cancel(audioSlideAnim_);
    audioSlideAnim_ = 0;
    const float to = expanding ? 0.0f : 1.0f;
    if (!SystemAnimationsEnabled() || std::fabs(audioSlideT_ - to) < 0.001f) {
        audioSlideT_ = to;
        audioSlideAlpha_ = 1.0f - to;
        FinishAudioSlide(expanding);
        return;
    }
    audioSliding_ = true;
    const int duration = expanding ? 360 : 280;
    const Easing easing =
        expanding ? Easing::EmphasizedDecelerate : Easing::EmphasizedAccelerate;
    audioSlideAnim_ = anim_.Add(audioSlideT_, to, duration, easing,
                                [this, expanding](float v) {
                                    audioSlideT_ = v;
                                    audioSlideAlpha_ = 1.0f - v;
                                    InvalidateAudioSlide();
                                    if ((expanding && v <= 0.001f) ||
                                        (!expanding && v >= 0.999f)) {
                                        FinishAudioSlide(expanding);
                                    }
                                });
    StartAnimTimer();
    InvalidateAudioSlide();
}

void MainWindow::FinishAudioSlide(bool expanded) {
    // 回调与显式收尾可能在同一帧内到达；已经收尾时不要重复显示/释放。
    if (!audioSliding_ && audioSlideAnim_ == 0) return;
    audioSliding_ = false;
    audioSlideAnim_ = 0;
    audioSlideT_ = 0.0f;
    audioSlideAlpha_ = expanded ? 1.0f : 0.0f;
    ReleaseAudioSnapshot();
    if (expanded) {
        if (audioPanelRect_.right > audioPanelRect_.left) {
            audiocenter::SetPanelBounds(audioPanelRect_);
        }
        audiocenter::SetPanelVisible(true);
        Layout();
    } else {
        audiocenter::SetPanelVisible(false);
        // 设备页内容入场还在跑时不要 Layout，否则会提前显示原生控件，
        // 在半透明快照上方穿透；FinishContentIntro 会统一恢复可见性。
        if (!introActive_) Layout();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::CaptureAudioPanelSnapshot() {
    HWND panel = audiocenter::PanelHwnd();
    if (!panel || !IsWindow(panel)) return false;
    RECT pr{};
    GetClientRect(panel, &pr);
    const int w = pr.right - pr.left;
    const int h = pr.bottom - pr.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(hwnd_);
    if (!screen) return false;
    if (audioSnapMem_ && (audioSnapW_ != w || audioSnapH_ != h)) {
        ReleaseAudioSnapshot();
    }
    if (!audioSnapMem_) {
        audioSnapMem_ = CreateCompatibleDC(screen);
        if (audioSnapMem_) {
            audioSnapBmp_ = CreateCompatibleBitmap(screen, w, h);
            if (audioSnapBmp_) {
                audioSnapOld_ = SelectObject(audioSnapMem_, audioSnapBmp_);
                audioSnapW_ = w;
                audioSnapH_ = h;
            } else {
                DeleteDC(audioSnapMem_);
                audioSnapMem_ = nullptr;
            }
        }
    }
    ReleaseDC(hwnd_, screen);
    if (!audioSnapMem_ || !audioSnapBmp_) return false;

    // 清空后截图；PrintWindow 失败时不会沿用上一张旧快照。
    RECT all{0, 0, w, h};
    FillRect(audioSnapMem_, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    audioSnapValid_ = PrintWindow(panel, audioSnapMem_, 1 | 2) != FALSE;
    return audioSnapValid_;
}

void MainWindow::ReleaseAudioSnapshot() {
    if (audioSnapMem_) {
        if (audioSnapBmp_) {
            SelectObject(audioSnapMem_, audioSnapOld_);
            DeleteObject(audioSnapBmp_);
        }
        DeleteDC(audioSnapMem_);
    }
    audioSnapMem_ = nullptr;
    audioSnapBmp_ = nullptr;
    audioSnapOld_ = nullptr;
    audioSnapW_ = 0;
    audioSnapH_ = 0;
    audioSnapValid_ = false;
}

void MainWindow::PaintAudioSnapshot(HDC dc) {
    if (!audioSnapValid_ || !audioSnapMem_ || !audioSliding_) return;
    const int targetW = audioPanelRect_.right - audioPanelRect_.left;
    const int targetH = audioPanelRect_.bottom - audioPanelRect_.top;
    if (targetW <= 0 || targetH <= 0) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int x = audioPanelRect_.left +
                  (int)((client.right - audioPanelRect_.left) * audioSlideT_ + 0.5f);
    const int alpha = ClampInt((int)std::lround(audioSlideAlpha_ * 255.0f), 0, 255);
    if (alpha <= 0) return;

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = (BYTE)alpha;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, audioPanelRect_.left, audioPanelRect_.top, client.right,
                      audioPanelRect_.bottom);
    AlphaBlend(dc, x, audioPanelRect_.top, targetW, targetH, audioSnapMem_, 0, 0,
               audioSnapW_, audioSnapH_, bf);
    RestoreDC(dc, saved);
}

void MainWindow::SetView(View v) {
    focusNav_ = ClampInt((int)v, 0, 2);
    if (v == view_) {
        if (audioExpanded_) CollapseAudio();
        return;
    }
    if (audioExpanded_) CollapseAudio();
    if (rcNavItems_[0].right > 0) {
        float from = navIndicatorY_;
        float to = (float)rcNavItems_[(int)v].top;
        if (from <= 0.0f) from = to;
        navIndicatorY_ = from;
        anim_.Cancel(navAnim_);
        // Material 3 Expressive 弹簧：位移到位前约 8% 过冲，再回稳。
        navAnim_ = anim_.Add(from, to, 340, Easing::Spring, [this](float y) {
            navIndicatorY_ = y;
            InvalidateRect(hwnd_, &rcRail_, FALSE);
        });
        // 0→1→0 包络：移动中胶囊轻微拉长，回稳时收回，形成柔和的果冻感。
        anim_.Add(0.0f, 1.0f, 340, Easing::SpringSoft, [this](float v) {
            const float envelope = std::sin(std::max(0.0f, v) * 3.14159265f);
            navStretch_ = envelope * (float)Scale(3);
            InvalidateRect(hwnd_, &rcRail_, FALSE);
        });
        StartAnimTimer();
    }
    if (v == View::Test) {
        SetTestView(true);
        StartContentIntro();
        return;
    }
    if (view_ == View::Test) {
        testView_.Show(hwnd_, false);
        KillTimer(hwnd_, 3);
        view_ = View::Device;
    }
    SetLogView(v == View::Log);
    StartContentIntro();
}

// 入场动画只覆盖自绘内容；原生子控件无法半透明，必须先隐藏，
// 等动画结束后由 Layout/UpdateControlsFromDevice 统一恢复。
void MainWindow::HideContentChildren() {
    HWND children[] = {list_,      logList_,  logCombo_,   logDaysCombo_, logFolder_,
                       logExport_, btnRefresh_, btnApply_, slider_,      editDpi_,
                       comboPoll_, comboDeb_,  chkAngle_,   editVolDev_,   editVolSys_,
                       comboVolSys_, chkLoudOk_};
    for (HWND h : children) {
        if (h) ShowWindow(h, SW_HIDE);
    }
    for (HWND h : stageButtons_) {
        if (h) ShowWindow(h, SW_HIDE);
    }
    // 测试页的目标设备下拉框由 MouseTestView 自己创建，不在上面的清单里。
    testView_.SetChildrenHidden(true);
}

void MainWindow::StartContentIntro() {
    if (!hwnd_) return;
    // 视图再次切换时取消旧轨道并重新开始；子控件保持隐藏，避免闪一下再消失。
    anim_.Cancel(introAnim_);
    introAnim_ = 0;
    if (!SystemAnimationsEnabled()) {
        introT_ = 1.0f;
        introActive_ = true;  // 让 Finish 走统一的“恢复可见性”路径
        FinishContentIntro();
        return;
    }
    introT_ = 0.0f;
    introActive_ = true;
    HideContentChildren();
    // 420 ms EmphasizedDecelerate：内容从下方 18px 淡入到位（Apple Store 式）。
    // introT_ 存的是缓动后的 alpha（0..1），Paint 用它同时决定透明度和偏移。
    introAnim_ = anim_.Add(0.0f, 1.0f, 420, Easing::EmphasizedDecelerate, [this](float v) {
        introT_ = v;
        InvalidateRect(hwnd_, &rcContent_, FALSE);
    });
    StartAnimTimer();
    InvalidateRect(hwnd_, &rcContent_, FALSE);
}

void MainWindow::FinishContentIntro() {
    if (!introActive_) return;
    introActive_ = false;
    introT_ = 1.0f;
    // 92% 收尾后旧轨道可能还在跑；显式取消，避免继续空转重绘。
    anim_.Cancel(introAnim_);
    introAnim_ = 0;
    // 用常规路径恢复原生控件可见性，不在这里逐控件决定显示/隐藏。
    if (view_ == View::Device && !audioExpanded_) {
        UpdateControlsFromDevice();  // 末尾会调用 Layout()
    } else {
        Layout();
    }
    // 测试页下拉框由 MouseTestView 管理，Layout 不会恢复它。
    testView_.SetChildrenHidden(false);
    InvalidateRect(hwnd_, &rcContent_, FALSE);
}

void MainWindow::SetNavHover(int nav) {
    if (nav == hoverNav_) return;
    hoverNav_ = nav;
    if (!SystemAnimationsEnabled()) {
        for (int i = 0; i < 3; ++i) {
            anim_.Cancel(navHoverAnim_[i]);
            navHoverAnim_[i] = 0;
            navHover_[i] = (i == nav) ? 1.0f : 0.0f;
        }
        InvalidateRect(hwnd_, &rcRail_, FALSE);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const float to = (i == nav) ? 1.0f : 0.0f;
        // 必须先取消在飞的轨道再判断“是否已经到位”：轨道刚 Add、还没被 Tick 时
        // navHover_[i] 仍是旧值 0，按旧值 continue 会留下一条永不反向的轨道——
        // 表现就是快速划过导航项后鼠标已离开，该导航项一直保持高亮。
        anim_.Cancel(navHoverAnim_[i]);
        navHoverAnim_[i] = 0;
        if (std::fabs(navHover_[i] - to) < 0.005f) {
            navHover_[i] = to;
            continue;
        }
        navHoverAnim_[i] = anim_.Add(navHover_[i], to, 120, Easing::Standard,
                                     [this, i](float v) {
                                         navHover_[i] = v;
                                         InvalidateRect(hwnd_, &rcRail_, FALSE);
                                     });
    }
    StartAnimTimer();
    InvalidateRect(hwnd_, &rcRail_, FALSE);
}

void MainWindow::OnNavClick(int x, int y) {
    POINT p{x, y};
    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&rcNavItems_[i], p)) {
            focusNav_ = i;
            // 点击导航后把键盘焦点收回主窗口，后续方向键/Enter 直接操作导航栏。
            SetFocus(hwnd_);
            SetView((View)i);
            InvalidateNavFocus();
            return;
        }
    }
}

std::vector<HWND> MainWindow::BuildFocusChain() const {
    std::vector<HWND> chain;
    if (view_ == View::Device && !audioExpanded_) {
        // 阅读顺序：设备列表（主对象）→ 应用/刷新 → DPI → 性能卡。
        AddTabStop(chain, list_);
        AddTabStop(chain, btnApply_);
        AddTabStop(chain, btnRefresh_);
        // 音量卡的数字输入排在刷新之后、DPI 之前；不可见/禁用时会被过滤。
        AddTabStop(chain, editVolDev_);
        AddTabStop(chain, editVolSys_);
        AddTabStop(chain, comboVolSys_);
        AddTabStop(chain, chkLoudOk_);
        AddTabStop(chain, editDpi_);
        AddTabStop(chain, slider_);
        for (HWND b : stageButtons_) AddTabStop(chain, b);
        AddTabStop(chain, comboPoll_);
        AddTabStop(chain, comboDeb_);
        AddTabStop(chain, chkAngle_);
    } else if (view_ == View::Log) {
        // 内容头从左到右：设备筛选 → 天数 → 导出 → 打开文件夹 → 刷新；最后是列表。
        AddTabStop(chain, logCombo_);
        AddTabStop(chain, logDaysCombo_);
        AddTabStop(chain, logExport_);
        AddTabStop(chain, logFolder_);
        AddTabStop(chain, btnRefresh_);
        AddTabStop(chain, logList_);
    } else {
        // 测试页/音频面板的控件是嵌套子窗口，由下面的递归枚举补齐；刷新按钮
        // 在测试页隐藏，在音频面板展开时也隐藏，AddTabStop 会自动过滤。
        AddTabStop(chain, btnRefresh_);
    }
    // 追加嵌套原生控件（测试页目标下拉、音频面板列表/输出/输入下拉）。
    // EnumChildWindows 递归遍历所有后代，按 z 序返回，已加入的会被去重。
    EnumChildWindows(hwnd_, CollectTabStopProc, (LPARAM)&chain);
    return chain;
}

bool MainWindow::HandleTabKey(bool backward) {
    const std::vector<HWND> chain = BuildFocusChain();
    HWND focused = GetFocus();
    int current = uikey::kFocusNav;
    auto it = std::find(chain.begin(), chain.end(), focused);
    if (it != chain.end()) current = (int)(it - chain.begin());

    const int next = uikey::NextFocusSlot(current, (int)chain.size(), !backward);
    if (next == uikey::kFocusNav) {
        if (chain.empty()) {
            // 当前页面没有原生控件（例如键盘测试模式只有自绘按钮）时，
            // Tab 在导航项之间循环，键盘仍然可用，不会“卡死”。
            focusNav_ = uikey::NextIndex(focusNav_, 3, backward ? -1 : 1);
        } else {
            // 从控件链回到导航栏时停在当前页面，符合分区导航的直觉。
            focusNav_ = ClampInt((int)view_, 0, 2);
        }
        SetFocus(hwnd_);
        InvalidateNavFocus();
        return true;
    }
    SetFocus(chain[next]);
    InvalidateNavFocus();
    return true;
}

void MainWindow::SetNavFocus(int index, bool activate) {
    focusNav_ = ClampInt(index, 0, 2);
    SetFocus(hwnd_);
    InvalidateNavFocus();
    if (activate) SetView((View)focusNav_);
}

void MainWindow::InvalidateNavFocus() {
    if (hwnd_) InvalidateRect(hwnd_, &rcRail_, FALSE);
}

bool MainWindow::PreTranslateMessage(const MSG& msg) {
    if (!hwnd_ || !msg.hwnd) return false;
    // 只接管主窗口及其子控件；组合框弹出的下拉列表是独立窗口，交给系统处理。
    if (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd)) return false;
    // 只接管普通键盘消息。Alt+Tab / Alt+F4 / Win 组合键是系统快捷键，
    // 必须交给 DefWindowProc；否则会吞掉系统的窗口切换与最小化行为。
    if (msg.message != WM_KEYDOWN) return false;
    // 用 GetAsyncKeyState 读取物理修饰键状态：GetKeyState 依赖线程消息队列，
    // 窗口刚激活或自动化注入时可能还没同步，导致 Ctrl/Shift 组合键误判。
    if (GetAsyncKeyState(VK_MENU) & 0x8000) return false;

    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // 音量数字输入按 Enter 立即归一化（ES_NUMBER 下不会产生默认按钮行为）。
    // 拖动/失焦/设备刷新同样会归一化，Enter 只是让键盘用户有明确的提交动作。
    if (msg.wParam == VK_RETURN) {
        HWND focused = GetFocus();
        if (focused == editVolDev_) {
            NormalizeVolumeEdit(true);
            return true;
        }
        if (focused == editVolSys_) {
            NormalizeVolumeEdit(false);
            return true;
        }
    }

    if (msg.wParam == VK_TAB) {
        if (ctrl) {
            // Ctrl+Tab / Ctrl+Shift+Tab 在三个页面之间循环，焦点停在导航栏。
            SetNavFocus(uikey::NextIndex((int)view_, 3, shift ? -1 : 1), true);
        } else {
            HandleTabKey(shift);
        }
        return true;
    }
    if (msg.wParam == VK_F5) {
        manager_.RequestRefresh();
        if (view_ == View::Log) {
            ReloadLogs();
            ShowToast(L"日志已刷新", true, 2);
        } else {
            ShowToast(L"正在刷新…", true, 2);
        }
        return true;
    }
    if (ctrl && msg.wParam >= '1' && msg.wParam <= '3') {
        SetNavFocus((int)(msg.wParam - '1'), true);
        return true;
    }
    if (msg.wParam == VK_ESCAPE && audioExpanded_) {
        CollapseAudio();
        return true;
    }
    return false;
}

void MainWindow::ExportLogs() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int64_t todayStart = EventEpoch(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
    // 这里保存记录副本而不是指针：保存对话框是模态的，期间 OnSnapshot 的 10s
    // 定时重载会重建 logRecords_，指针会悬空并在导出时崩溃（0xc0000005）。
    std::vector<EventRecord> rows;
    for (const auto& r : logRecords_) {
        if (!logIdentity_.empty() && r.identity != logIdentity_) continue;
        if (r.type == L"SAMPLE") continue;
        if (!EventWithinDays(r.epoch, logDays_, todayStart)) continue;
        rows.push_back(r);
    }
    if (rows.empty()) {
        ShowToast(Fmt(L"最近 %d 天没有可导出的记录", logDays_), false, 4);
        return;
    }

    wchar_t file[MAX_PATH] = {};
    swprintf(file, MAX_PATH, L"2.4GCenter-events-%04d%02d%02d.csv", st.wYear, st.wMonth, st.wDay);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0文本文件 (*.txt)\0*.txt\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;

    std::wstring path = file;
    std::wstring lower = Lower(path);
    bool txt = lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".txt") == 0;
    std::wstring content = txt ? L"" : L"\uFEFF";
    if (!txt) content += L"时间,类型,设备,identity,详情\r\n";
    for (const EventRecord& r : rows) {
        content += txt ? EventRecordTextLine(r) : EventRecordCsvLine(r);
        content += L"\r\n";
    }

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        ShowToast(L"导出失败：无法写入文件", false, 5);
        return;
    }
    std::string utf8 = WideToUtf8(content);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
    ShowToast(Fmt(L"已导出 %d 条记录", (int)rows.size()), true, 4);
}

void MainWindow::UpdateMiniSeries() {
    miniSeries_.clear();
    const Device* d = Selected();
    // 无电量属性的设备（有线/蓝牙无电池）不读取、不绘制电量曲线。
    if (d && d->batteryApplicable) {
        std::wstring id = d->identity.empty() ? d->id : d->identity;
        miniSeries_ = BuildBatterySeries(logRecords_, id);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::LogMaxScroll() const {
    int viewW = (rcLogChart_.right - Scale(16)) - (rcLogChart_.left + Scale(46));
    if (viewW <= 0) return 0;
    int contentW = std::max(viewW, logDays_ * Scale(220));
    return contentW - viewW;
}

void MainWindow::UpdateLogScrollbar() {
    logScrollX_ = ClampInt(logScrollX_, 0, LogMaxScroll());
}

RECT MainWindow::LogThumbRect() const {
    int maxScroll = LogMaxScroll();
    if (maxScroll <= 0) return {};
    int left = rcLogChart_.left + Scale(46);
    int right = rcLogChart_.right - Scale(16);
    int viewW = right - left;
    int contentW = viewW + maxScroll;
    int thumbW = std::max(Scale(30), (int)((int64_t)viewW * viewW / contentW));
    if (thumbW > viewW) thumbW = viewW;
    int x = left + (int)((int64_t)(viewW - thumbW) * logScrollX_ / maxScroll);
    int y = rcLogChart_.bottom - Scale(12);
    return {x, y, x + thumbW, y + Scale(6)};
}

void MainWindow::PaintMiniChart(HDC dc) {
    if (IsRectEmpty(&rcMiniChart_)) return;
    DrawMiniChart(dc, rcMiniChart_, dpi_, fontBodyBold_, fontSmall_, miniSeries_);
}

void MainWindow::OnSnapshot() {
    Snapshot s = manager_.GetSnapshot();
    std::wstring keep = selectedId_;
    snapshot_ = std::move(s);
    notificationsEnabled_ = SystemNotificationsEnabled();

    int idx = -1;
    if (!keep.empty()) {
        for (int i = 0; i < (int)snapshot_.devices.size(); ++i) {
            if (snapshot_.devices[i].id == keep) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0 && !snapshot_.devices.empty()) {
        idx = 0;
        for (int i = 0; i < (int)snapshot_.devices.size(); ++i) {
            if (snapshot_.devices[i].online) {
                idx = i;
                break;
            }
        }
    }
    bool selectionChanged = idx != selectedIndex_ || keep != selectedId_;
    selectedIndex_ = idx;
    selectedId_ = idx >= 0 ? snapshot_.devices[idx].id : L"";

    RebuildListIfNeeded();
    if (testView_.visible()) testView_.SetDevices(snapshot_.devices, selectedId_);
    bool battNow = selectedIndex_ >= 0 && snapshot_.devices[selectedIndex_].batteryApplicable;
    bool batteryModeChanged = battNow != lastBatteryApplicable_;
    lastBatteryApplicable_ = battNow;
    if (!dirty_ || selectionChanged || batteryModeChanged) UpdateControlsFromDevice();
    // 音频面板展开时，麦克风/输出端点上下文来自当前设备；选中设备变化后
    // 必须把新 Context 推给面板，否则会继续显示旧设备的麦克风匹配结果。
    if (selectionChanged && audioExpanded_) {
        if (Selected()) {
            ExpandAudio();
        } else {
            CollapseAudio();
        }
    }
    RefreshTray();
    NotifyBatteryEvents();
    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && view_ == View::Log) {
        // 日志视图每 10s 重新读一次，保持列表/曲线接近实时。
        if (GetTickCount64() - logReloadAt_ >= 10000) ReloadLogs();
    } else if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
        // 设备页迷你曲线：日志最多每 15s 读一次，选中变化时用缓存重算。
        if (GetTickCount64() - logReloadAt_ >= 15000) {
            logRecords_ = LoadEventRecords(7);
            logReloadAt_ = GetTickCount64();
        }
        UpdateMiniSeries();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnApplyResult() {
    Snapshot s = manager_.GetSnapshot();
    if (s.applySerial == lastApplySerial_) return;
    lastApplySerial_ = s.applySerial;
    toast_ = s.applyMessage;
    toastOk_ = s.applyOk;
    toastUntil_ = GetTickCount64() + 7000;
    if (s.applyOk) dirty_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!s.applyOk) {
        MessageBeep(MB_ICONWARNING);
    }
}

void MainWindow::RebuildListIfNeeded() {
    if (!list_) return;
    std::vector<std::wstring> ids;
    ids.reserve(snapshot_.devices.size());
    for (const auto& d : snapshot_.devices) {
        ids.push_back(d.id + L"|" + d.name + L"|" + DeviceConnLabel(d) + L"|" +
                      DeviceStatusText(d) + L"|" + std::to_wstring(d.battery) + L"|" +
                      std::to_wstring(d.batteryValid ? 1 : 0));
    }
    rebuildingList_ = true;
    if (ids != listedIds_) {
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& d : snapshot_.devices) {
            SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)d.name.c_str());
        }
        listedIds_ = ids;
    }
    if (selectedIndex_ >= 0 && selectedIndex_ < (int)snapshot_.devices.size()) {
        SendMessageW(list_, LB_SETCURSEL, selectedIndex_, 0);
    } else {
        SendMessageW(list_, LB_SETCURSEL, (WPARAM)-1, 0);
    }
    rebuildingList_ = false;
}

void MainWindow::UpdateControlsFromDevice() {
    updatingControls_ = true;
    const Device* d = Selected();
    bool has = d != nullptr;
    bool dev = view_ == View::Device && !audioExpanded_;
    bool showDpi = has && d->dpi.valid;
    bool dpiWritable = showDpi && d->dpi.canWrite && d->dpi.currentKnown;
    ShowWindow(slider_, (dpiWritable && dev) ? SW_SHOW : SW_HIDE);
    ShowWindow(editDpi_, (dpiWritable && dev) ? SW_SHOW : SW_HIDE);
    EnableWindow(slider_, dpiWritable);
    EnableWindow(editDpi_, dpiWritable);

    if (showDpi) {
        int value = d->dpi.currentKnown ? d->dpi.x : d->dpi.min;
        if (d->dpi.currentKnown && !d->dpi.stages.empty() && d->dpi.activeStage >= 0 &&
            d->dpi.activeStage < (int)d->dpi.stages.size()) {
            value = d->dpi.stages[d->dpi.activeStage];
        }
        dpiValue_ = value;
        stageIndex_ = d->dpi.activeStage >= 0 ? d->dpi.activeStage : 0;
        SendMessageW(slider_, WM_SLIDER_SETLOG, TRUE, 0);
        SendMessageW(slider_, WM_SLIDER_SETRANGE, (WPARAM)d->dpi.min, (LPARAM)d->dpi.max);
        SendMessageW(slider_, WM_SLIDER_SETSTEP, (WPARAM)std::max(1, d->dpi.step), 0);
        SendMessageW(slider_, WM_SLIDER_SETVALUE, (WPARAM)dpiValue_, 0);
        std::wstring txt = d->dpi.currentKnown ? std::to_wstring(value) : L"";
        SetWindowTextW(editDpi_, txt.c_str());
        EnableWindow(editDpi_, dpiWritable);
    }

    size_t nStages = showDpi ? std::min<size_t>(d->dpi.stages.size(), stageButtons_.size()) : 0;
    for (size_t i = 0; i < stageButtons_.size(); ++i) {
        if (i < nStages) {
            std::wstring label = std::to_wstring(d->dpi.stages[i]);
            SetWindowTextW(stageButtons_[i], label.c_str());
            ShowWindow(stageButtons_[i], (i < nStages && dev) ? SW_SHOW : SW_HIDE);
            // Read-only protocols still allow browsing the stored stages.
            EnableWindow(stageButtons_[i], d->online);
        } else {
            ShowWindow(stageButtons_[i], SW_HIDE);
        }
    }

    bool showPoll = has && d->polling.valid;
    ShowWindow(comboPoll_, (showPoll && dev) ? SW_SHOW : SW_HIDE);
    EnableWindow(comboPoll_, showPoll && d->polling.canWrite && d->online);
    if (showPoll) {
        SendMessageW(comboPoll_, CB_RESETCONTENT, 0, 0);
        int sel = -1;
        for (size_t i = 0; i < d->polling.options.size(); ++i) {
            int v = d->polling.options[i];
            std::wstring s = Fmt(L"%d Hz", v);
            SendMessageW(comboPoll_, CB_ADDSTRING, 0, (LPARAM)s.c_str());
            if (v == d->polling.currentHz) sel = (int)i;
        }
        SendMessageW(comboPoll_, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
    }

    bool showDeb = has && d->debounce.valid;
    ShowWindow(comboDeb_, (showDeb && dev) ? SW_SHOW : SW_HIDE);
    EnableWindow(comboDeb_, showDeb && d->debounce.canWrite && d->online);
    if (showDeb) {
        SendMessageW(comboDeb_, CB_RESETCONTENT, 0, 0);
        int sel = -1;
        for (size_t i = 0; i < d->debounce.options.size(); ++i) {
            int v = d->debounce.options[i];
            std::wstring s = Fmt(L"%d ms", v);
            SendMessageW(comboDeb_, CB_ADDSTRING, 0, (LPARAM)s.c_str());
            if (v == d->debounce.currentMs) sel = (int)i;
        }
        SendMessageW(comboDeb_, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
    }

    bool showAngle = has && d->angle.valid;
    ShowWindow(chkAngle_, (showAngle && dev) ? SW_SHOW : SW_HIDE);
    EnableWindow(chkAngle_, showAngle && d->angle.canWrite && d->online);
    if (showAngle) {
        SendMessageW(chkAngle_, BM_SETCHECK, d->angle.on ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    bool canWrite = has && d->online && (d->dpi.canWrite || d->polling.canWrite ||
                                         d->debounce.canWrite || d->angle.canWrite);
    ShowWindow(btnApply_, (canWrite && dev) ? SW_SHOW : SW_HIDE);
    EnableWindow(btnApply_, canWrite);

    dirty_ = false;
    dpiDirty_ = pollingDirty_ = debounceDirty_ = angleDirty_ = stageDirty_ = false;
    updatingControls_ = false;
    // 设备页每次刷新时重读播放端点与音量（不做后台轮询）。
    RefreshAudioState();
    Layout();
    if (d) AnimateBattery(*d);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnSelectIndex(int index) {
    // 切换设备后重新确认听力保护；同一设备刷新不重置。
    if (selectedIndex_ != index) ResetHeadsetLoudConsent();
    if (index < 0 || index >= (int)snapshot_.devices.size()) {
        selectedIndex_ = -1;
        selectedId_.clear();
        UpdateControlsFromDevice();
        UpdateMiniSeries();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    selectedIndex_ = index;
    selectedId_ = snapshot_.devices[index].id;
    dirty_ = false;
    UpdateControlsFromDevice();
    UpdateMiniSeries();  // 切换设备时立即更新底部曲线
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnSliderChanged(int value) {
    if (updatingControls_) return;
    const Device* d = Selected();
    if (!d) return;
    dpiValue_ = value;
    dpiDirty_ = true;
    dirty_ = true;
    updatingControls_ = true;
    SetWindowTextW(editDpi_, std::to_wstring(value).c_str());
    updatingControls_ = false;
    InvalidateRect(hwnd_, &rcDpi_, FALSE);
}

void MainWindow::OnDpiEditChanged() {
    if (updatingControls_) return;
    const Device* d = Selected();
    if (!d) return;
    wchar_t buf[32] = {};
    GetWindowTextW(editDpi_, buf, ARRAYSIZE(buf));
    int v = _wtoi(buf);
    if (v <= 0) return;
    v = ClampInt(v, d->dpi.min, d->dpi.max);
    if (d->dpi.step > 1) {
        int k = (v - d->dpi.min + d->dpi.step / 2) / d->dpi.step;
        v = ClampInt(d->dpi.min + k * d->dpi.step, d->dpi.min, d->dpi.max);
    }
    dpiValue_ = v;
    dpiDirty_ = true;
    dirty_ = true;
    SendMessageW(slider_, WM_SLIDER_SETVALUE, (WPARAM)v, 0);
    InvalidateRect(hwnd_, &rcDpi_, FALSE);
}

void MainWindow::OnStageClicked(int index) {
    const Device* d = Selected();
    if (!d || index < 0 || index >= (int)d->dpi.stages.size()) return;
    stageIndex_ = index;
    // 只读设备仍可点档位按钮浏览数值，但不能置 dirty_：
    // “应用设置”按钮对只读设备不可见，dirty_ 一旦置位就永远无法清除，
    // OnSnapshot 之后会永久跳过 UpdateControlsFromDevice（音频卡/电量环不再刷新）。
    if (d->dpi.canWrite && d->online) {
        stageDirty_ = true;
        dirty_ = true;
    }
    dpiValue_ = d->dpi.stages[index];
    updatingControls_ = true;
    SetWindowTextW(editDpi_, std::to_wstring(dpiValue_).c_str());
    SendMessageW(slider_, WM_SLIDER_SETVALUE, (WPARAM)dpiValue_, 0);
    updatingControls_ = false;
    InvalidateRect(hwnd_, &rcDpi_, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplySettings() {
    const Device* d = Selected();
    if (!d) return;
    ApplyRequest req;
    req.deviceId = d->id;
    if (d->dpi.valid && d->dpi.canWrite && d->dpi.currentKnown &&
        (dpiDirty_ || stageDirty_)) {
        req.setDpi = true;
        req.dpiX = dpiValue_;
        req.dpiY = dpiValue_;
        req.stageIndex = stageIndex_;
        req.selectStage = stageDirty_ && stageIndex_ >= 0;
    }
    if (d->polling.valid && d->polling.canWrite && pollingDirty_) {
        int sel = (int)SendMessageW(comboPoll_, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)d->polling.options.size()) {
            req.setPolling = true;
            req.pollingHz = d->polling.options[sel];
        }
    }
    if (d->debounce.valid && d->debounce.canWrite && debounceDirty_) {
        int sel = (int)SendMessageW(comboDeb_, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)d->debounce.options.size()) {
            req.setDebounce = true;
            req.debounceMs = d->debounce.options[sel];
        }
    }
    if (d->angle.valid && d->angle.canWrite && angleDirty_) {
        req.setAngle = true;
        req.angleOn = SendMessageW(chkAngle_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (!req.setDpi && !req.setPolling && !req.setDebounce && !req.setAngle) {
        ShowToast(L"该设备没有可写入的设置", false);
        return;
    }
    bool isZaopin = d->proto == Proto::Compx && d->vid == 0x3554 &&
                    (d->pid == 0xF527 || d->pid == 0xF526 || d->pid == 0xF524 ||
                     d->pid == 0xF50F || d->pid == 0xF50D);
    if (!demo_ && isZaopin && req.setDpi) {
        if (MessageBoxW(hwnd_,
                        L"将把 DPI 写入 Zaopin Z4D 的闪存配置。\n\n"
                        L"程序会先做一次原值回写自检，再写入新值并读回校验；\n"
                        L"失败会尝试恢复原值。官方固件没有事务回滚，\n"
                        L"建议先用 Mouse Drive 记录当前 DPI。\n\n继续写入？",
                        L"写入 Zaopin 闪存", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
            IDYES) {
            return;
        }
    }
    manager_.Apply(req);
    ShowToast(L"正在写入设备…", true, 3);
}

void MainWindow::RefreshTray() {
    // 托盘图标可能在 Explorer 未就绪时 Add 失败；后续刷新要重试 NIM_ADD，
    // 而不是只对不存在的图标调用 NIM_MODIFY。
    if (!tray_.Added() && trayIcon_) {
        tray_.Add(hwnd_, WM_APP_TRAY, trayIcon_, L"2.4G 设备中心");
        lastTrayKey_.clear();
    }
    int idx = PrimaryDeviceIndex();
    const Device* d = idx >= 0 ? &snapshot_.devices[idx] : nullptr;
    int pct = d && d->batteryApplicable && d->batteryValid ? d->battery : -1;
    bool charging = d && d->charging;
    bool stale = !d || d->stale || !d->batteryValid;
    bool transportIcon = d && !d->batteryApplicable;

    std::wstring tip = L"2.4G 设备中心";
    int shown = 0;
    for (const auto& dev : snapshot_.devices) {
        if (!dev.batteryValid || !dev.online) continue;
        tip += L"\n";
        tip += dev.name;
        tip += L" ";
        if (dev.charging) tip += L"充电 ";
        tip += std::to_wstring(dev.battery) + L"%";
        if (++shown >= 4) break;
    }
    if (!shown) {
        // 没有电量设备时，明确列出有线/蓝牙设备，不写“等待电量”。
        int listed = 0;
        for (const auto& dev : snapshot_.devices) {
            if (!dev.online) continue;
            tip += L"\n" + dev.name + L" " + DeviceConnLabel(dev);
            if (++listed >= 4) break;
        }
        if (!listed) tip += L"\n未检测到兼容设备";
    }
    if (tip.size() > 120) tip.resize(120);

    std::wstring key = tip + Fmt(L"|%d|%d|%d|%d|%d|%s|%d|%d", pct, charging ? 1 : 0,
                                 stale ? 1 : 0, d ? (int)d->kind : -1,
                                 (int)snapshot_.devices.size(),
                                 d ? d->name.c_str() : L"", transportIcon ? 1 : 0,
                                 d ? (int)d->conn : -1);
    if (key != lastTrayKey_ || !trayIcon_) {
        HICON icon =
            transportIcon
                ? CreateTransportIcon(d->conn == Conn::Bluetooth, !d->online || d->stale,
                                      GetSystemMetrics(SM_CXSMICON))
                : CreateBatteryIcon(pct, charging, stale, GetSystemMetrics(SM_CXSMICON));
        if (icon) {
            tray_.Update(icon, tip);
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = icon;
        }
        lastTrayKey_ = key;
    }

}

void MainWindow::NotifyBatteryEvents() {
    if (manager_.IsDemo()) return;
    if (!notificationsEnabled_) {
        // Windows 总开关关闭时 Shell_NotifyIcon 仍返回成功，但通知不会显示。
        // 此时不推进状态机，等用户打开通知后仍能收到当前的低电/充满事件。
        if (!notifyDisabledHintShown_) {
            notifyDisabledHintShown_ = true;
            LogLine(L"Notify skipped: Windows system notifications are disabled");
            ShowToast(L"Windows 系统通知已关闭：托盘菜单 → 通知设置", false, 8);
        }
        return;
    }
    for (const auto& n : batteryNotifier_.Update(snapshot_.devices)) {
        const DWORD flags = n.warning ? NIIF_WARNING : NIIF_INFO;
        if (tray_.Balloon(n.title, n.text, flags)) {
            LogLine(L"Notify: %s - %s", n.title.c_str(), n.text.c_str());
        } else {
            LogLine(L"Notify failed: %s - %s", n.title.c_str(), n.text.c_str());
        }
    }
}

int MainWindow::PrimaryDeviceIndex() const {
    int best = -1;
    auto better = [&](int, const Device& d) {
        if (best < 0) return true;
        const Device& b = snapshot_.devices[best];
        auto score = [](const Device& x) {
            int s = 0;
            if (x.online) s += 1000;
            // 有电池属性的设备优先（无线暂时读不到电也比有线/蓝牙无电池设备更适合当托盘主设备）。
            if (x.batteryApplicable) s += 400;
            if (x.batteryValid) s += 200;
            if (x.kind == Kind::Mouse) s += 100;
            if (x.charging) s += 10;
            return s - (x.batteryValid ? x.battery : 0);
        };
        return score(d) > score(b);
    };
    for (int i = 0; i < (int)snapshot_.devices.size(); ++i) {
        if (!snapshot_.devices[i].batteryValid && !snapshot_.devices[i].online) continue;
        if (better(i, snapshot_.devices[i])) best = i;
    }
    if (best < 0 && !snapshot_.devices.empty()) best = 0;
    return best;
}

void MainWindow::ShowToast(const std::wstring& text, bool ok, int seconds) {
    toast_ = text;
    toastOk_ = ok;
    toastUntil_ = GetTickCount64() + (ULONGLONG)seconds * 1000;
    anim_.Cancel(toastAnim_);
    if (!SystemAnimationsEnabled()) {
        toastShown_ = 1.0f;
    } else {
        // 出现时从下方 3px 滑入 + 颜色渐入，柔和收尾。
        toastShown_ = 0.0f;
        toastAnim_ = anim_.Add(0.0f, 1.0f, 260, Easing::SpringSoft, [this](float v) {
            toastShown_ = v;
            InvalidateRect(hwnd_, &rcFooter_, FALSE);
        });
        StartAnimTimer();
    }
    InvalidateRect(hwnd_, &rcFooter_, FALSE);
}

void MainWindow::StartAnimTimer() {
    if (!hwnd_) return;
    const int hz = MonitorRefreshHz(hwnd_);
    // 120 Hz ⇒ 8 ms，144 Hz ⇒ 7 ms，60 Hz ⇒ 17 夹到 16 ms；整体夹在 4..16 ms。
    const int interval = ClampInt((int)std::lround(1000.0 / (double)hz), 4, 16);
    if (animTimerActive_) {
        // 已在运行：仅当刷新率换算出的间隔变化（换显示器/改显示模式）才重启，
        // 避免每次 StartAnimTimer 都重置计时。
        if ((UINT)interval == animTimerIntervalMs_) return;
        SetTimer(hwnd_, 2, (UINT)interval, nullptr);
        animTimerIntervalMs_ = (UINT)interval;
        return;
    }
    // Windows 默认时钟粒度 15.6 ms；不提升到 1 ms 时 8 ms 定时器实际仍按 15.6 ms 触发。
    if (!animTimerPeriodSet_ && timeBeginPeriod(1) == TIMERR_NOERROR) {
        animTimerPeriodSet_ = true;
    }
    SetTimer(hwnd_, 2, (UINT)interval, nullptr);
    animTimerIntervalMs_ = (UINT)interval;
    animTimerActive_ = true;
}

void MainWindow::StopAnimTimer() {
    if (hwnd_) KillTimer(hwnd_, 2);
    if (animTimerPeriodSet_) {
        timeEndPeriod(1);
        animTimerPeriodSet_ = false;
    }
    animTimerActive_ = false;
    animTimerIntervalMs_ = 0;
}

void MainWindow::AnimateBattery(const Device& d) {
    double target = (d.batteryValid && d.battery >= 0) ? (double)d.battery : -1.0;
    // 只有电量卡真正可见时才补间/启动定时器。充电脉冲是“常驻”条件，
    // 若在日志/测试/音频页也启动，会以 8–16 ms 永久整窗重绘（尤其是最小化后
    // IsWindowVisible 仍为 true），与 SPEC 的“动画定时器不常驻”冲突。
    const bool cardVisible = view_ == View::Device && !audioExpanded_ &&
                             IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    // 系统关闭动画或卡片不可见时不做补间，也不启动定时器。
    if (!SystemAnimationsEnabled() || !cardVisible) {
        ringDeviceId_ = d.id;
        ringShown_ = target;
        ringFrom_ = ringTo_ = target;
        ringAnimating_ = false;
        return;
    }
    if (ringDeviceId_ != d.id) {
        ringDeviceId_ = d.id;
        ringShown_ = target >= 0 ? 0.0 : -1.0;
        ringFrom_ = 0.0;
        ringTo_ = target;
        ringAnimating_ = target >= 0;
        ringStart_ = NowMs();
    } else if (target < 0) {
        ringShown_ = -1.0;
        ringAnimating_ = false;
    } else if (!ringAnimating_ && std::fabs(target - ringShown_) > 0.05) {
        ringFrom_ = ringShown_ >= 0 ? ringShown_ : 0.0;
        ringTo_ = target;
        ringStart_ = NowMs();
        ringAnimating_ = true;
    } else if (ringAnimating_) {
        ringTo_ = target;
    }
    if (ringAnimating_ || d.charging) StartAnimTimer();
    InvalidateRect(hwnd_, &rcBattery_, FALSE);
}

void MainWindow::RepaintBatteryCard() {
    const Device* d = Selected();
    if (!paintMem_ || !paintBmp_ || !d || rcBattery_.right <= rcBattery_.left) {
        InvalidateRect(hwnd_, &rcBattery_, FALSE);
        return;
    }
    // 场景缓存里有上一帧的完整画面：先把卡片区域恢复成背景，再只重画这张卡。
    if (!PaintBackdropRegion(paintMem_, rcBattery_)) {
        InvalidateRect(hwnd_, &rcBattery_, FALSE);
        return;
    }
    const int saved = SaveDC(paintMem_);
    IntersectClipRect(paintMem_, rcBattery_.left, rcBattery_.top, rcBattery_.right,
                      rcBattery_.bottom);
    svgicon::BeginFrame(paintMem_);
    PaintBatteryCard(paintMem_, *d);
    svgicon::EndFrame();
    RestoreDC(paintMem_, saved);

    HDC dc = GetDC(hwnd_);
    if (dc != nullptr) {
        BitBlt(dc, rcBattery_.left, rcBattery_.top, rcBattery_.right - rcBattery_.left,
               rcBattery_.bottom - rcBattery_.top, paintMem_, rcBattery_.left,
               rcBattery_.top, SRCCOPY);
        ReleaseDC(hwnd_, dc);
    }
}

void MainWindow::OnAnimTimer() {
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
        StopAnimTimer();
        return;
    }
    const double now = NowMs();
    anim_.Tick(now);
    // 入场轨道到达 1：用常规路径恢复原生子控件（可能顺带启动电量补间）。
    // 提前到 92% 收尾：此时内容已经基本不透明，原生控件恢复可见时不会明显"弹"出来，
    // 剩下 1–2px 的位移差肉眼不可见。
    if (introActive_ && introT_ >= 0.92f) FinishContentIntro();
    // 本帧电量环是否发生变化：补间期间每帧都在变（含收敛到终值的那一帧），
    // 结算后 ringAnimating_ 已经为 false，必须单独记下来。
    bool ringChanged = false;
    if (ringAnimating_) {
        double t = (now - ringStart_) / 650.0;
        ringChanged = true;
        if (t >= 1.0) {
            ringShown_ = ringTo_;
            ringAnimating_ = false;
        } else {
            // 与全应用共用 Material Decelerate 曲线；650 ms 时长保持不变。
            // 柔和弹簧：结束时略有过冲再回稳，数值越大观感越“活”。
            double eased = Ease(Easing::SpringSoft, (float)t);
            ringShown_ = ringFrom_ + (ringTo_ - ringFrom_) * eased;
        }
    }
    const Device* d = Selected();
    const bool cardVisible = view_ == View::Device && !audioExpanded_ && d &&
                             d->batteryApplicable;
    // 充电脉冲/电量补间只影响电量卡。非设备页/音频面板展开时必须让 charging
    // 不成立，否则 StopAnimTimer 的常驻条件会让定时器永远停不下来。
    const bool charging = cardVisible && d->charging;
    // 只有电量卡自己变了（环补间/充电脉冲）才重绘它。
    // B05：以前只要“有任意动画在跑”就无条件 InvalidateRect(rcBattery_)，
    // 导航悬停这类与电量无关的动画会因此把电量卡并进脏区，脏区 bbox 从
    // 76px 宽的导航栏膨胀到 674×741，增量重绘几乎白做。
    const bool batteryDirty = cardVisible && (ringChanged || charging);
    if (batteryDirty) {
        // 没有别的轨道/入场时走直达 BitBlt；否则并进脏区由 WM_PAINT 一起合成。
        if (!anim_.Active() && !introActive_) {
            RepaintBatteryCard();
        } else {
            InvalidateRect(hwnd_, &rcBattery_, FALSE);
        }
    }
    if (!anim_.Active() && !ringAnimating_ && !charging) StopAnimTimer();
}

void MainWindow::ShowFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    // 隐藏期间 OnAnimTimer 会停掉定时器但保留轨道（例如 DPI 滑块补间），
    // 恢复窗口后重新启动，下一帧会按已过时间直接收敛到终值。
    if (anim_.Active() || introActive_) StartAnimTimer();
    // 测试页隐藏到托盘时停掉了 Raw Input/50 ms 定时器；恢复窗口后重新进入
    // 测试视图（Show 内部会重新注册 Raw Input 并重启定时器）。
    if (view_ == View::Test) SetTestView(true);
    SetForegroundWindow(hwnd_);
    // 从托盘恢复后把焦点交回主窗口，方向键/Enter 可以立刻操作导航栏。
    focusNav_ = ClampInt((int)view_, 0, 2);
    SetFocus(hwnd_);
    InvalidateNavFocus();
    if (const Device* d = Selected()) AnimateBattery(*d);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::HideToTray() {
    if (audioExpanded_) CollapseAudio();
    // 测试页的 Raw Input 与 50 ms 定时器不能留在隐藏窗口上继续跑
    // （20 次/秒唤醒 + 统计仍可能累积）。重新显示时由 ShowFromTray 恢复。
    if (testView_.visible()) {
        testView_.Show(hwnd_, false);
        KillTimer(hwnd_, 3);
    }
    StopAnimTimer();
    // 隐藏窗口不保证再收到 WM_MOUSEMOVE/WM_MOUSELEAVE，主动清掉导航悬停。
    // 这里直接归零而不是走 120 ms 渐变：窗口已经不可见，没必要再跑动画。
    hoverNav_ = -1;
    for (int i = 0; i < 3; ++i) {
        anim_.Cancel(navHoverAnim_[i]);
        navHoverAnim_[i] = 0;
        navHover_[i] = 0.0f;
    }
    navStretch_ = 0.0f;
    navMouseTracking_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    if (!closeHintShown_) {
        closeHintShown_ = true;
        tray_.Balloon(L"2.4G 设备中心", L"程序仍在托盘中运行。右键托盘图标可退出。", NIIF_INFO);
    }
}

void MainWindow::ShowTrayMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_OPEN, L"打开主界面");
    AppendMenuW(menu, MF_STRING, ID_MENU_REFRESH, L"立即刷新");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0), ID_MENU_AUTOSTART,
                L"开机自启");
    AppendMenuW(menu, MF_STRING, ID_MENU_NOTIFY_SETTINGS,
                notificationsEnabled_ ? L"通知设置" : L"通知设置（系统通知已关闭）");
    AppendMenuW(menu, MF_STRING, ID_MENU_TEST_NOTIFY, L"发送测试通知");
    AppendMenuW(menu, MF_STRING, ID_MENU_EVENTS, L"日志与电量曲线");
    AppendMenuW(menu, MF_STRING, ID_MENU_TEST, L"鼠标 / 键盘测试");
    AppendMenuW(menu, MF_STRING, ID_MENU_AUDIO, L"音频控制…");
    AppendMenuW(menu, MF_STRING, ID_MENU_LOG, L"打开日志");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"退出");
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0,
                              hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == ID_MENU_OPEN) {
        ShowFromTray();
    } else if (cmd == ID_MENU_REFRESH) {
        manager_.RequestRefresh();
        ShowToast(L"正在刷新…", true, 2);
    } else if (cmd == ID_MENU_AUTOSTART) {
        SetAutoStart(!IsAutoStartEnabled());
    } else if (cmd == ID_MENU_NOTIFY_SETTINGS) {
        ShellExecuteW(nullptr, L"open", L"ms-settings:notifications", nullptr, nullptr,
                      SW_SHOWNORMAL);
    } else if (cmd == ID_MENU_TEST_NOTIFY) {
        notificationsEnabled_ = SystemNotificationsEnabled();
        if (!notificationsEnabled_) {
            ShowToast(L"Windows 系统通知已关闭：请先在通知设置中打开", false, 8);
        } else if (tray_.Balloon(L"通知测试",
                                 L"低电量与充电完成通知会通过 Windows 通知中心显示。",
                                 NIIF_INFO)) {
            LogLine(L"Notify test sent");
        }
    } else if (cmd == ID_MENU_EVENTS) {
        ShowFromTray();
        SetView(View::Log);
    } else if (cmd == ID_MENU_TEST) {
        ShowFromTray();
        SetView(View::Test);
    } else if (cmd == ID_MENU_AUDIO) {
        ShowFromTray();
        ExpandAudio();
    } else if (cmd == ID_MENU_LOG) {
        ShellExecuteW(nullptr, L"open", AppLogPath().c_str(), nullptr, ExeDir().c_str(),
                      SW_SHOWNORMAL);
    } else if (cmd == ID_MENU_EXIT) {
        exiting_ = true;
        DestroyWindow(hwnd_);
    }
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

LRESULT MainWindow::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer 崩溃/重启后任务栏会广播 TaskbarCreated；不重新 NIM_ADD 的话
    // 托盘图标永久消失（开机自启时 Explorer 也可能尚未就绪）。
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbarCreated && msg == taskbarCreated) {
        tray_.Remove();
        lastTrayKey_.clear();
        if (!trayIcon_) {
            trayIcon_ =
                CreateBatteryIcon(-1, false, true, GetSystemMetrics(SM_CXSMICON));
        }
        RefreshTray();
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            hwnd_ = hwnd;
            dpi_ = GetDpiForHwnd(hwnd);
            brushCard_ = CreateSolidBrush(kCard);
            brushBg_ = CreateSolidBrush(kBg);
            brushField_ = CreateSolidBrush(RGB(249, 251, 254));
            brushCard_global = brushCard_;
            brushField_global = brushField_;
            RecreateFonts();
            CreateControls();
            Layout();
            appIcon_ = LoadIconW(instance_, MAKEINTRESOURCEW(101));
            if (appIcon_) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)appIcon_);
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon_);
            }
            HICON trayInit = CreateBatteryIcon(-1, false, true, GetSystemMetrics(SM_CXSMICON));
            tray_.Add(hwnd, WM_APP_TRAY, trayInit, L"2.4G 设备中心 · 正在扫描");
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = trayInit;
            SetTimer(hwnd, 1, 1000, nullptr);
            UpdateControlsFromDevice();
            manager_.Start(hwnd, demo_);
            return 0;
        }
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                // 最小化时 IsWindowVisible 仍为 true；必须主动停表，否则充电脉冲
                // 会继续以 8–16 ms 唤醒进程。
                StopAnimTimer();
                // 测试页 50 ms 定时器与音频面板 2 s/33 ms 刷新同样不能留在
                // 最小化窗口上继续跑（麦克风采集涉及隐私）。
                if (testView_.visible()) KillTimer(hwnd, 3);
                if (audioExpanded_) audiocenter::SetPanelSuspended(true);
            } else if (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) {
                if (anim_.Active() || introActive_) StartAnimTimer();
                if (view_ == View::Test && testView_.visible()) {
                    SetTimer(hwnd, 3, 50, nullptr);
                }
                if (audioExpanded_) audiocenter::SetPanelSuspended(false);
                if (const Device* d = Selected()) AnimateBattery(*d);
            }
            Layout();
            return 0;
        case WM_DISPLAYCHANGE:
            // 分辨率或刷新率变化：刷新率缓存失效，活动中的动画定时器按新间隔重启。
            InvalidateRefreshCache();
            if (animTimerActive_) StartAnimTimer();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp) ? HIWORD(wp) : GetDpiForHwnd(hwnd);
            RecreateFonts();
            testView_.SetDpi(dpi_);
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            // LBS_OWNERDRAWFIXED 的行高只在控件创建时通过 WM_MEASUREITEM 测量一次，
            // 换到不同 DPI 的显示器后必须显式重设，否则屏幕缩放变化后行距仍按旧 DPI。
            if (list_) SendMessageW(list_, LB_SETITEMHEIGHT, 0, (LPARAM)Scale(56));
            // DPI 变化通常也伴随换显示器，刷新率可能不同；缓存失效后按新间隔重启。
            InvalidateRefreshCache();
            if (animTimerActive_) StartAnimTimer();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            int minW = Scale(1000);
            int minH = Scale(740);
            // 1080p@150% 的可用工作区小于 1000×740 逻辑尺寸的物理像素；
            // 夹到工作区，保证窗口始终能缩到屏幕内。设计尺寸本身不变。
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                int workW = mi.rcWork.right - mi.rcWork.left;
                int workH = mi.rcWork.bottom - mi.rcWork.top;
                if (workW > 0 && minW > workW) minW = workW;
                if (workH > 0 && minH > workH) minH = workH;
            }
            mmi->ptMinTrackSize.x = minW;
            mmi->ptMinTrackSize.y = minH;
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            // B05：把系统给的失效区交给 Paint。整窗失效才整帧重画，
            // 悬停/页脚/单张卡片这类局部失效只重画脏区，避开 8 张玻璃卡片的全量合成。
            Paint(dc, ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* mis = (MEASUREITEMSTRUCT*)lp;
            if (mis->CtlID == ID_LIST) {
                mis->itemHeight = Scale(56);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* dis = (DRAWITEMSTRUCT*)lp;
            if (dis->CtlID == ID_LIST) {
                if (dis->itemID == (UINT)-1) return TRUE;
                int idx = (int)dis->itemID;
                if (idx < 0 || idx >= (int)snapshot_.devices.size()) return TRUE;
                const Device& d = snapshot_.devices[idx];
                bool sel = (dis->itemState & ODS_SELECTED) != 0;
                RECT r = dis->rcItem;
                InflateRect(&r, -Scale(6), -Scale(2));
                FillRounded(dis->hDC, r, Scale(6), sel ? kAccentSoft : kCard,
                            sel ? kAccentSoft : kCard);
                RECT icon{r.left + Scale(10), r.top + Scale(13), r.left + Scale(42),
                          r.top + Scale(45)};
                DrawDeviceGlyph(dis->hDC, d.kind, icon, sel ? kAccent : RGB(100, 106, 115),
                                sel);
                RECT name{r.left + Scale(50), r.top + Scale(6), r.right - Scale(58),
                          r.top + Scale(30)};
                DrawTextIn(dis->hDC, d.name, name, fontBodyBold_, sel ? kAccent : kText,
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT sub{r.left + Scale(50), r.top + Scale(30), r.right - Scale(58),
                         r.top + Scale(50)};
                std::wstring info = Fmt(L"%s · %s", DeviceConnLabel(d).c_str(),
                                        DeviceStatusText(d).c_str());
                DrawTextIn(dis->hDC, info, sub, fontSmall_, kMuted,
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                if (d.batteryApplicable) {
                    std::wstring ringText =
                        d.batteryValid ? std::to_wstring(ClampInt(d.battery, 0, 100)) : L"--";
                    DrawRingGauge(dis->hDC, r.right - Scale(28), r.top + Scale(28), Scale(16),
                                  Scale(4), d.battery, d.charging, d.stale || !d.batteryValid,
                                  fontBadge_, ringText, kText);
                } else {
                    RECT badge{r.right - Scale(48), r.top + Scale(16), r.right - Scale(8),
                               r.top + Scale(40)};
                    DrawTransportBadge(dis->hDC, badge, d.conn, fontBadge_);
                }
                // 键盘 Tab 到设备列表后，系统不会替 owner-draw 行画焦点框。
                if (dis->itemState & ODS_FOCUS) {
                    RECT fr = r;
                    InflateRect(&fr, -Scale(2), -Scale(2));
                    DrawFocusRect(dis->hDC, &fr);
                }
                return TRUE;
            }
            if (dis->CtlID == ID_REFRESH || dis->CtlID == ID_APPLY ||
                dis->CtlID == ID_LOG_FOLDER || dis->CtlID == ID_LOG_EXPORT ||
                (dis->CtlID >= ID_STAGE_BASE && dis->CtlID < ID_STAGE_BASE + 8)) {
                bool accent = dis->CtlID == ID_APPLY;
                bool selected = false;
                if (dis->CtlID >= ID_STAGE_BASE) {
                    int i = (int)dis->CtlID - ID_STAGE_BASE;
                    selected = (i == stageIndex_);
                }
                bool disabled = (dis->itemState & ODS_DISABLED) != 0;
                COLORREF bg = disabled ? RGB(229, 231, 235)
                                        : (accent ? (selected ? RGB(29, 78, 216) : kAccent)
                                                  : (selected ? kAccent : RGB(243, 244, 246)));
                COLORREF fg = (accent || selected) ? RGB(255, 255, 255) : kText;
                if (disabled) fg = RGB(156, 163, 175);
                RECT r = dis->rcItem;
                FillRounded(dis->hDC, r, Scale(8), bg, bg);
                wchar_t text[128] = {};
                GetWindowTextW(dis->hwndItem, text, ARRAYSIZE(text));
                DrawTextIn(dis->hDC, text, r, fontBodyBold_, fg,
                           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (dis->itemState & ODS_FOCUS) {
                    RECT fr = r;
                    InflateRect(&fr, -Scale(3), -Scale(3));
                    DrawFocusRect(dis->hDC, &fr);
                }
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, kCard);
            SetTextColor(dc, kText);
            return (LRESULT)brushCard_;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, kCard);
            SetTextColor(dc, kText);
            return (LRESULT)brushCard_;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, kCard);
            SetTextColor(dc, kText);
            return (LRESULT)brushCard_;
        }
        case WM_CTLCOLORBTN: {
            // 复选框无法真正透明：用与玻璃卡实际呈现色接近的纯色，避免纯白方块。
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(249, 251, 254));
            SetTextColor(dc, kText);
            return (LRESULT)brushField_;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (id == ID_LIST && code == LBN_SELCHANGE) {
                if (rebuildingList_) return 0;
                int sel = (int)SendMessageW(list_, LB_GETCURSEL, 0, 0);
                OnSelectIndex(sel);
                return 0;
            }
            if (id == ID_REFRESH && code == BN_CLICKED) {
                manager_.RequestRefresh();
                if ((view_ == View::Log)) {
                    ReloadLogs();
                    ShowToast(L"日志已刷新", true, 2);
                } else {
                    ShowToast(L"正在刷新…", true, 2);
                }
                return 0;
            }
            if (id == MouseTestView::kComboId) {
                testView_.HandleCommand(id, code);
                return 0;
            }
            if (id == ID_LOG_COMBO && code == CBN_SELCHANGE) {
                ApplyLogSelection();
                return 0;
            }
            if (id == ID_LOG_FOLDER && code == BN_CLICKED) {
                ShellExecuteW(nullptr, L"open", EventLogDir().c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
                return 0;
            }
            if (id == ID_LOG_EXPORT && code == BN_CLICKED) {
                ExportLogs();
                return 0;
            }
            if (id == ID_LOG_DAYS && code == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(logDaysCombo_, CB_GETCURSEL, 0, 0);
                logDays_ = ClampInt(sel >= 0 ? sel + 1 : 7, 1, 7);
                logScrollX_ = 1 << 30;  // 换尺度后回到最右（最近数据）
                FillLogList();
                UpdateLogScrollbar();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (id == ID_APPLY && code == BN_CLICKED) {
                ApplySettings();
                return 0;
            }
            if (id == ID_DPI_EDIT && code == EN_CHANGE) {
                OnDpiEditChanged();
                return 0;
            }
            if (id == ID_VOL_DEV && code == EN_CHANGE) {
                OnVolumeEditChanged(true);
                return 0;
            }
            if (id == ID_VOL_SYS && code == EN_CHANGE) {
                OnVolumeEditChanged(false);
                return 0;
            }
            if (id == ID_VOL_DEV && code == EN_KILLFOCUS) {
                NormalizeVolumeEdit(true);
                return 0;
            }
            if (id == ID_VOL_SYS && code == EN_KILLFOCUS) {
                NormalizeVolumeEdit(false);
                return 0;
            }
            if (id == ID_SYS_ENDPOINT && code == CBN_SELCHANGE) {
                if (!updatingControls_) {
                    const int sel =
                        (int)SendMessageW(comboVolSys_, CB_GETCURSEL, 0, 0);
                    if (sel >= 0 && sel < (int)audioSysChoices_.size()) {
                        const std::wstring& picked = audioSysChoices_[sel];
                        // 选中列表里的默认端点等同于“跟随系统默认”，避免默认
                        // 切换后仍被旧 id 钉住。
                        bool isDefaultPick = picked.empty();
                        for (const auto& e : audioEndpoints_) {
                            if (e.id == picked && e.isDefault) {
                                isDefaultPick = true;
                                break;
                            }
                        }
                        audioSysPinnedId_ = isDefaultPick ? std::wstring() : picked;
                        headsetLoudPending_ = -1;
                        RefreshAudioState();
                        Layout();
                        InvalidateRect(hwnd_, &rcInfo_, FALSE);
                    }
                }
                return 0;
            }
            if (id == ID_LOUD_OK && code == BN_CLICKED) {
                // BM_SETCHECK 不会发 BN_CLICKED，因此这里不需要 updatingControls_ 保护；
                // 否则刷新期间的用户点击会只改变勾选状态却不执行逻辑。
                headsetLoudOk_ =
                    SendMessageW(chkLoudOk_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (headsetLoudOk_) {
                    ApplyHeadsetLoudPending();
                } else {
                    headsetLoudPending_ = -1;
                }
                Layout();  // 文案行数变化后复选框/链接位置要重排
                InvalidateRect(hwnd_, &rcInfo_, FALSE);
                return 0;
            }
            if (id == ID_POLL && code == CBN_SELCHANGE) {
                if (!updatingControls_) {
                    pollingDirty_ = true;
                    dirty_ = true;
                }
                return 0;
            }
            if (id == ID_DEB && code == CBN_SELCHANGE) {
                if (!updatingControls_) {
                    debounceDirty_ = true;
                    dirty_ = true;
                }
                return 0;
            }
            if (id == ID_ANGLE && code == BN_CLICKED) {
                if (!updatingControls_) {
                    angleDirty_ = true;
                    dirty_ = true;
                }
                return 0;
            }
            if (id >= ID_STAGE_BASE && id < ID_STAGE_BASE + 8 && code == BN_CLICKED) {
                OnStageClicked(id - ID_STAGE_BASE);
                return 0;
            }
            break;
        }
        case WM_APP_SNAPSHOT:
            OnSnapshot();
            return 0;
        case WM_APP_APPLY_RESULT:
            OnApplyResult();
            return 0;
        case WM_APP_SLIDER_CHANGED:
            OnSliderChanged((int)wp);
            return 0;
        case WM_APP_ANIM_START:
            // Slider 的补间挂在 MainWindow 的 Animator 上，由这里统一启动 16 ms 定时器。
            StartAnimTimer();
            return 0;
        case WM_APP_TRAY: {
            UINT event = LOWORD(lp);
            if (event == 0) event = (UINT)lp;
            if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK ||
                event == NIN_BALLOONUSERCLICK) {
                ShowFromTray();
            } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                ShowTrayMenu(pt);
            }
            return 0;
        }
        case WM_APP_SHOW_MAIN:
            ShowFromTray();
            return 0;
        case audiocenter::WM_BACK:
            CollapseAudio();
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                if (!toast_.empty() && GetTickCount64() >= toastUntil_) {
                    toast_.clear();
                    InvalidateRect(hwnd, &rcFooter_, FALSE);
                }
            } else if (wp == 2) {
                OnAnimTimer();
            } else if (wp == 3) {
                testView_.OnTimer();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                manager_.RequestRefresh();
            }
            return TRUE;
        case WM_DEVICECHANGE:
            manager_.RequestRefresh();
            return TRUE;
        case WM_INPUT:
            if (testView_.visible()) {
                testView_.HandleRawInput((HRAWINPUT)lp);
            }
            // Raw Input 的默认处理必须交给 DefWindowProc，否则部分设备收不到后续报文。
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_KEYDOWN: {
            // 焦点在主窗口 = 焦点在自绘导航栏。子控件的按键由控件自己处理，
            // Tab/全局快捷键由 PreTranslateMessage 在消息循环里统一接管。
            if (GetFocus() != hwnd_) break;
            switch (wp) {
                case VK_UP:
                case VK_LEFT:
                    focusNav_ = uikey::NextIndex(focusNav_, 3, -1);
                    InvalidateNavFocus();
                    return 0;
                case VK_DOWN:
                case VK_RIGHT:
                    focusNav_ = uikey::NextIndex(focusNav_, 3, 1);
                    InvalidateNavFocus();
                    return 0;
                case VK_HOME:
                    focusNav_ = 0;
                    InvalidateNavFocus();
                    return 0;
                case VK_END:
                    focusNav_ = 2;
                    InvalidateNavFocus();
                    return 0;
                case VK_RETURN:
                case VK_SPACE:
                    SetView((View)focusNav_);
                    return 0;
                case VK_TAB:
                    // 正常情况下 PreTranslateMessage 已消费 Tab；这里兜底，
                    // 保证直接 SendMessage(WM_KEYDOWN) 的测试路径也能导航。
                    HandleTabKey((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_SETFOCUS:
            // 焦点框位置由 SetView/SetNavFocus/Tab 逻辑维护，这里只重绘导航栏。
            InvalidateNavFocus();
            break;
        case WM_KILLFOCUS:
            InvalidateNavFocus();
            break;
        case WM_ACTIVATE:
            InvalidateNavFocus();
            break;
        case WM_PARENTNOTIFY: {
            // 原生子控件用鼠标获得焦点时不会通知父窗口，用 PARENTNOTIFY 让导航
            // 焦点框及时消失；只在鼠标按下类事件上做，避免创建/销毁时刷屏。
            const UINT ev = LOWORD(wp);
            if (ev == WM_LBUTTONDOWN || ev == WM_LBUTTONDBLCLK || ev == WM_RBUTTONDOWN ||
                ev == WM_MBUTTONDOWN) {
                InvalidateNavFocus();
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            if (testView_.visible()) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if ((view_ == View::Log)) {
                RECT chart = rcLogChart_;
                if (PtInRect(&chart, pt)) {
                    logScrollX_ += delta > 0 ? -Scale(90) : Scale(90);
                    UpdateLogScrollbar();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&rcRail_, pt)) {
                OnNavClick(pt.x, pt.y);
                return 0;
            }
            if (testView_.visible()) {
                testView_.OnClick(pt.x, pt.y);
                return 0;
            }
            // 音量卡：静音开关、系统声音设置链接、音量条按下拖动。
            if (view_ == View::Device && AudioCardAvailable() && PtInRect(&rcInfo_, pt)) {
                auto toggleMute = [&](bool device) {
                    if (device) {
                        if (audioDevId_.empty()) return;
                        audioDevMute_ = !audioDevMute_;
                        audio::SetMute(audioDevId_, audioDevMute_);
                        if (audioDevId_ == audioSysId_) audioSysMute_ = audioDevMute_;
                    } else {
                        if (audioSysId_.empty()) return;
                        audioSysMute_ = !audioSysMute_;
                        audio::SetMute(audioSysId_, audioSysMute_);
                        if (audioDevId_ == audioSysId_) audioDevMute_ = audioSysMute_;
                    }
                    InvalidateRect(hwnd_, &rcInfo_, FALSE);
                };
                if (PtInRect(&rcMuteDev_, pt)) {
                    toggleMute(true);
                    return 0;
                }
                if (PtInRect(&rcMuteSys_, pt)) {
                    toggleMute(false);
                    return 0;
                }
                if (PtInRect(&rcAudioCenter_, pt)) {
                    ExpandAudio();
                    return 0;
                }
                if (PtInRect(&rcSoundLink_, pt)) {
                    audio::OpenSoundSettings();
                    return 0;
                }
                if (PtInRect(&rcVolDev_, pt)) {
                    volDrag_ = 1;
                    SetCapture(hwnd_);
                    ApplyVolumeDrag(pt.x);
                    return 0;
                }
                if (PtInRect(&rcVolSys_, pt)) {
                    volDrag_ = 2;
                    SetCapture(hwnd_);
                    ApplyVolumeDrag(pt.x);
                    return 0;
                }
            }
            if ((view_ == View::Log)) {
                int maxScroll = LogMaxScroll();
                if (maxScroll > 0) {
                    RECT thumb = LogThumbRect();
                    RECT hit{rcLogChart_.left + Scale(46), rcLogChart_.bottom - Scale(18),
                             rcLogChart_.right - Scale(16), rcLogChart_.bottom};
                    if (PtInRect(&hit, pt)) {
                        int left = rcLogChart_.left + Scale(46);
                        int tw = thumb.right - thumb.left;
                        if (PtInRect(&thumb, pt)) {
                            dragGrab_ = pt.x - thumb.left;
                        } else {
                            int trackW =
                                (rcLogChart_.right - Scale(16)) - left - tw;
                            dragGrab_ = tw / 2;
                            int pos = (pt.x - left - dragGrab_) * maxScroll /
                                      std::max(1, trackW);
                            logScrollX_ = ClampInt(pos, 0, maxScroll);
                            InvalidateRect(hwnd_, nullptr, FALSE);
                        }
                        dragTarget_ = 2;
                        SetCapture(hwnd_);
                        return 0;
                    }
                }
                break;
            }
            if (!IsRectEmpty(&rcMiniChart_) && PtInRect(&rcMiniChart_, pt)) {
                SetView(View::Log);  // 点设备页底部曲线直接进日志视图（走胶囊动画）
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            // 测试页统计使用的光标位置必须在导航栏/内容区等所有分支之前更新；
            // 下拉框是原生子窗口，父窗口收不到这里的移动，测试页会在 Raw Input
            // 里低频用 GetCursorPos 兜底。
            if (testView_.visible()) testView_.TrackCursor(pt.x, pt.y);
            if (volDrag_ != 0) {
                ApplyVolumeDrag(pt.x);
                return 0;
            }
            if (PtInRect(&rcRail_, pt)) {
                // 鼠标离开窗口时系统只会发 WM_MOUSELEAVE；每次进入导航栏重新注册。
                if (!navMouseTracking_) {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd_;
                    if (TrackMouseEvent(&tme)) navMouseTracking_ = true;
                }
                int nav = -1;
                for (int i = 0; i < 3; ++i) {
                    if (PtInRect(&rcNavItems_[i], pt)) nav = i;
                }
                if (nav != hoverNav_) {
                    SetNavHover(nav);
                }
                return 0;
            }
            if (hoverNav_ != -1) {
                SetNavHover(-1);
            }
            if (testView_.visible()) {
                if (testView_.OnMouseMove(pt.x, pt.y)) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            if (dragTarget_ == 2) {
                int maxScroll = LogMaxScroll();
                if (maxScroll > 0) {
                    RECT thumb = LogThumbRect();
                    int left = rcLogChart_.left + Scale(46);
                    int trackW = (rcLogChart_.right - Scale(16)) - left -
                                 (thumb.right - thumb.left);
                    int pos = (GET_X_LPARAM(lp) - left - dragGrab_) * maxScroll /
                              std::max(1, trackW);
                    logScrollX_ = ClampInt(pos, 0, maxScroll);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            break;
        }
        case WM_MOUSELEAVE:
            // 指针直接离开窗口时不会再有 WM_MOUSEMOVE，必须在这里清掉导航悬停。
            navMouseTracking_ = false;
            SetNavHover(-1);
            return 0;
        case WM_LBUTTONUP:
            if (volDrag_ != 0) {
                volDrag_ = 0;
                ReleaseCapture();
                return 0;
            }
            if (dragTarget_) {
                dragTarget_ = 0;
                ReleaseCapture();
                return 0;
            }
            if (testView_.visible()) return 0;
            break;
        case WM_CAPTURECHANGED:
            dragTarget_ = 0;
            volDrag_ = 0;
            break;
        case WM_CLOSE:
            if (exiting_) {
                DestroyWindow(hwnd);
            } else {
                // 用户要求：关闭窗口时明确选择“退出程序”还是“后台运行”，
                // 默认选“否”，避免误按回车直接退出。
                int choice = MessageBoxW(hwnd,
                                         L"关闭窗口后，程序会继续在托盘运行并监控设备电量。\n\n"
                                         L"是：退出程序\n否：最小化到托盘继续运行\n取消：不关闭",
                                         L"2.4G 设备中心",
                                         MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
                if (choice == IDYES) {
                    exiting_ = true;
                    DestroyWindow(hwnd);
                } else if (choice == IDNO) {
                    HideToTray();
                }
            }
            return 0;
        case WM_DESTROY: {
            // 一行汇总，方便核对增量重绘是否真的生效（dirtyPx 占比越小收益越大）。
            RECT client{};
            GetClientRect(hwnd, &client);
            LogLine(L"Paint: full=%llu dirty=%llu dirtyPx=%llu minPx=%llu maxPx=%llu clientPx=%lld",
                    paintFull_, paintDirty_, paintDirtyPx_,
                    paintDirtyMinPx_ == ~0ULL ? 0ULL : paintDirtyMinPx_, paintDirtyMaxPx_,
                    (long long)client.right * (long long)client.bottom);
            StopAnimTimer();
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 3);
            ReleaseDrawBuffers();
            manager_.Stop();
            audiocenter::Shutdown();
            tray_.Remove();
            if (g_gdiplusToken) {
                Gdiplus::GdiplusShutdown(g_gdiplusToken);
                g_gdiplusToken = 0;
            }
            if (trayIcon_) {
                DestroyIcon(trayIcon_);
                trayIcon_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        }
        case WM_NCDESTROY: {
            if (brushCard_) DeleteObject(brushCard_);
            if (brushBg_) DeleteObject(brushBg_);
            if (brushField_) DeleteObject(brushField_);
            brushCard_ = brushBg_ = brushField_ = nullptr;
            brushCard_global = nullptr;
            brushField_global = nullptr;
            auto del = [](HFONT& f) {
                if (f) DeleteObject(f);
                f = nullptr;
            };
            del(fontTitle_);
            del(fontBody_);
            del(fontBodyBold_);
            del(fontSmall_);
            del(fontRing_);
            del(fontRingSmall_);
            del(fontBadge_);
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}





