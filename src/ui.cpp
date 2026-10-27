#include "ui.h"

#include <commctrl.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

static HBRUSH brushCard_global = nullptr;
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
    ID_SLIDER = 2100,
    ID_DPI_EDIT = 2101,
    ID_POLL = 2102,
    ID_DEB = 2103,
    ID_ANGLE = 2104,
    ID_STAGE_BASE = 2200,
    ID_STARTUP = 2300,
    ID_MENU_OPEN = 2401,
    ID_MENU_REFRESH = 2402,
    ID_MENU_AUTOSTART = 2403,
    ID_MENU_LOG = 2404,
    ID_MENU_EXIT = 2405,
};

const COLORREF kBg = RGB(244, 246, 249);
const COLORREF kCard = RGB(255, 255, 255);
const COLORREF kBorder = RGB(226, 230, 236);
const COLORREF kText = RGB(17, 24, 39);
const COLORREF kMuted = RGB(107, 114, 128);
const COLORREF kAccent = RGB(37, 99, 235);
const COLORREF kAccentSoft = RGB(219, 234, 254);
const COLORREF kGreen = RGB(22, 163, 74);
const COLORREF kAmber = RGB(217, 119, 6);
const COLORREF kRed = RGB(220, 38, 38);
const COLORREF kTrack = RGB(229, 231, 235);

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

HFONT MakeFont(UINT dpi, int pt, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void SetFont(HWND wnd, HFONT font) {
    if (wnd) SendMessageW(wnd, WM_SETFONT, (WPARAM)font, TRUE);
}

void DrawTextIn(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
                UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, flags);
}

void FillRounded(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, brush);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
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

void DrawDeviceGlyph(HDC dc, Kind kind, const RECT& rc, COLORREF color, bool filled) {
    EnsureGdiplus();
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float x = (float)rc.left;
    float y = (float)rc.top;
    float w = (float)(rc.right - rc.left);
    float h = (float)(rc.bottom - rc.top);
    Gdiplus::Color c(255, GetRValue(color), GetGValue(color), GetBValue(color));
    Gdiplus::Pen pen(c, std::max(1.6f, w / 16.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush soft(Gdiplus::Color(filled ? 40 : 18, GetRValue(color),
                                            GetGValue(color), GetBValue(color)));
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 250, 251, 253));

    if (kind == Kind::Keyboard) {
        Gdiplus::GraphicsPath body;
        AddRoundRect(body, x + w * 0.04f, y + h * 0.27f, w * 0.92f, h * 0.48f, w * 0.08f);
        g.FillPath(&white, &body);
        if (filled) g.FillPath(&soft, &body);
        g.DrawPath(&pen, &body);
        Gdiplus::Pen key(c, std::max(1.2f, w / 26.0f));
        key.SetLineJoin(Gdiplus::LineJoinRound);
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 4; ++col) {
                Gdiplus::GraphicsPath k;
                float kx = x + w * 0.16f + col * w * 0.19f;
                float ky = y + h * 0.40f + row * h * 0.17f;
                AddRoundRect(k, kx, ky, w * 0.11f, h * 0.10f, w * 0.025f);
                g.DrawPath(&key, &k);
            }
        }
    } else if (kind == Kind::Headset) {
        Gdiplus::Pen band(c, std::max(1.8f, w / 15.0f));
        band.SetStartCap(Gdiplus::LineCapRound);
        band.SetEndCap(Gdiplus::LineCapRound);
        g.DrawArc(&band, x + w * 0.10f, y + h * 0.08f, w * 0.80f, h * 0.76f, 180.0f, 180.0f);
        Gdiplus::GraphicsPath left;
        AddRoundRect(left, x + w * 0.06f, y + h * 0.46f, w * 0.24f, h * 0.40f, w * 0.08f);
        Gdiplus::GraphicsPath right;
        AddRoundRect(right, x + w * 0.70f, y + h * 0.46f, w * 0.24f, h * 0.40f, w * 0.08f);
        g.FillPath(&white, &left);
        g.FillPath(&white, &right);
        if (filled) {
            g.FillPath(&soft, &left);
            g.FillPath(&soft, &right);
        }
        g.DrawPath(&pen, &left);
        g.DrawPath(&pen, &right);
    } else if (kind == Kind::Other) {
        Gdiplus::GraphicsPath body;
        AddRoundRect(body, x + w * 0.24f, y + h * 0.12f, w * 0.52f, h * 0.76f, w * 0.14f);
        g.FillPath(&white, &body);
        if (filled) g.FillPath(&soft, &body);
        g.DrawPath(&pen, &body);
        Gdiplus::Pen wave(c, std::max(1.4f, w / 22.0f));
        wave.SetStartCap(Gdiplus::LineCapRound);
        wave.SetEndCap(Gdiplus::LineCapRound);
        g.DrawArc(&wave, x + w * 0.38f, y + h * 0.28f, w * 0.24f, h * 0.24f, 180.0f, 180.0f);
        g.DrawArc(&wave, x + w * 0.32f, y + h * 0.20f, w * 0.36f, h * 0.40f, 180.0f, 180.0f);
    } else {
        Gdiplus::GraphicsPath body;
        AddRoundRect(body, x + w * 0.28f, y + h * 0.04f, w * 0.44f, h * 0.92f, w * 0.20f);
        g.FillPath(&white, &body);
        if (filled) g.FillPath(&soft, &body);
        g.DrawPath(&pen, &body);
        Gdiplus::Pen slot(c, std::max(1.4f, w / 22.0f));
        slot.SetStartCap(Gdiplus::LineCapRound);
        g.DrawLine(&slot, x + w * 0.50f, y + h * 0.20f, x + w * 0.50f, y + h * 0.45f);
        Gdiplus::SolidBrush dot(c);
        g.FillEllipse(&dot, x + w * 0.46f, y + h * 0.55f, w * 0.08f, w * 0.08f);
    }
}

void DrawLightning(HDC dc, int x, int y, int w, int h, COLORREF color) {
    POINT pts[6] = {
        {x + w * 55 / 100, y},
        {x + w * 12 / 100, y + h * 55 / 100},
        {x + w * 45 / 100, y + h * 55 / 100},
        {x + w * 30 / 100, y + h},
        {x + w * 88 / 100, y + h * 40 / 100},
        {x + w * 55 / 100, y + h * 40 / 100},
    };
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH ob = (HBRUSH)SelectObject(dc, brush);
    HPEN op = (HPEN)SelectObject(dc, pen);
    Polygon(dc, pts, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
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
    DrawTextIn(dc, title, r, font, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

std::wstring DeviceStatusText(const Device& d) {
    if (!d.online) return L"离线";
    if (d.stale) return L"上次读数";
    return L"在线";
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
    bool animating = false;
    ULONGLONG animStart = 0;
    double animFrom = 1000.0;
    double animTo = 1000.0;
};

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

int SliderFraction(const SliderState& st) { return SliderFractionValue(st, st.value); }

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
        st.animating = false;
        KillTimer(hwnd, 1);
        InvalidateRect(hwnd, nullptr, FALSE);
        SliderNotify(hwnd, v);
    }
}

LRESULT CALLBACK SliderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SliderState* st = (SliderState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            (void)lp;
            auto* s = new SliderState();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
            return TRUE;
        }
        case WM_NCDESTROY:
            KillTimer(hwnd, 1);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        case WM_SLIDER_SETRANGE:
            if (st) {
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
                    st->animFrom = st->shownValue;
                    st->animTo = v;
                    st->animStart = GetTickCount64();
                    st->animating = true;
                    SetTimer(hwnd, 1, 16, nullptr);
                } else {
                    st->shownValue = v;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_TIMER:
            if (wp == 1 && st) {
                double t = (double)(GetTickCount64() - st->animStart) / 180.0;
                if (t >= 1.0) {
                    st->shownValue = st->animTo;
                    st->animating = false;
                    KillTimer(hwnd, 1);
                } else {
                    double eased = 1.0 - std::pow(1.0 - t, 3.0);
                    st->shownValue = st->animFrom + (st->animTo - st->animFrom) * eased;
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
            FillRect(mem, &rc, brushCard_global);
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
                HBRUSH tb = CreateSolidBrush(enabled ? kAccent : RGB(156, 163, 175));
                HPEN tp = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                HBRUSH ob = (HBRUSH)SelectObject(mem, tb);
                HPEN op = (HPEN)SelectObject(mem, tp);
                Ellipse(mem, thumbX - thumbR, rc.bottom / 2 - thumbR, thumbX + thumbR,
                        rc.bottom / 2 + thumbR);
                SelectObject(mem, ob);
                SelectObject(mem, op);
                DeleteObject(tb);
                DeleteObject(tp);
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
                st->animating = false;
                KillTimer(hwnd, 1);
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
        case WM_MOUSEWHEEL:
            if (st) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? st->step : -st->step;
                int v = SliderSnap(*st, st->value + delta);
                if (v != st->value) {
                    st->value = v;
                    st->shownValue = v;
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

}  // namespace

const wchar_t* MainWindow::ClassName() { return L"WirelessCenterMain"; }

bool MainWindow::Create(HINSTANCE instance, bool demo, bool startHidden) {
    instance_ = instance;
    demo_ = demo;
    startHidden_ = startHidden;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
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

    HDC screen = GetDC(nullptr);
    int sysDpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    int w = MulDiv(1000, sysDpi, 96);
    int h = MulDiv(780, sysDpi, 96);

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
    btnApply_ = CreateWindowExW(0, L"BUTTON", L"应用设置",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                                hwnd_, (HMENU)ID_APPLY, instance_, nullptr);
    slider_ = CreateWindowExW(0, L"WirelessCenterSlider", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, hwnd_,
                              (HMENU)ID_SLIDER, instance_, nullptr);
    editDpi_ = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL |
                                   ES_CENTER | WS_BORDER,
                               0, 0, 10, 10, hwnd_, (HMENU)ID_DPI_EDIT, instance_, nullptr);
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
    chkStartup_ = CreateWindowExW(0, L"BUTTON", L"开机自启",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10,
                                  10, hwnd_, (HMENU)ID_STARTUP, instance_, nullptr);

    SetFont(list_, fontBody_);
    SetFont(btnRefresh_, fontBodyBold_);
    SetFont(btnApply_, fontBodyBold_);
    SetFont(editDpi_, fontBodyBold_);
    SetFont(comboPoll_, fontBody_);
    SetFont(comboDeb_, fontBody_);
    SetFont(chkAngle_, fontBody_);
    SetFont(chkStartup_, fontBody_);

    for (int i = 0; i < 8; ++i) {
        HWND b = CreateWindowExW(0, L"BUTTON", L"",
                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd_,
                                 (HMENU)(INT_PTR)(ID_STAGE_BASE + i), instance_, nullptr);
        SetFont(b, fontBodyBold_);
        stageButtons_.push_back(b);
    }
    UpdateStartupCheckbox();
}

void MainWindow::RecreateFonts() {
    auto del = [](HFONT& f) {
        if (f) DeleteObject(f);
        f = nullptr;
    };
    del(fontTitle_);
    del(fontSub_);
    del(fontBody_);
    del(fontBodyBold_);
    del(fontSmall_);
    del(fontBig_);
    del(fontRing_);
    del(fontBadge_);
    fontTitle_ = MakeFont(dpi_, 16, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontSub_ = MakeFont(dpi_, 9, FW_NORMAL, L"Microsoft YaHei UI");
    fontBody_ = MakeFont(dpi_, 10, FW_NORMAL, L"Microsoft YaHei UI");
    fontBodyBold_ = MakeFont(dpi_, 10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontSmall_ = MakeFont(dpi_, 8, FW_NORMAL, L"Microsoft YaHei UI");
    fontBig_ = MakeFont(dpi_, 30, FW_SEMIBOLD, L"Segoe UI");
    fontRing_ = MakeFont(dpi_, 26, FW_SEMIBOLD, L"Segoe UI");
    fontBadge_ = MakeFont(dpi_, 8, FW_SEMIBOLD, L"Microsoft YaHei UI");
    if (list_) SetFont(list_, fontBody_);
    if (btnRefresh_) SetFont(btnRefresh_, fontBodyBold_);
    if (btnApply_) SetFont(btnApply_, fontBodyBold_);
    if (editDpi_) SetFont(editDpi_, fontBodyBold_);
    if (comboPoll_) SetFont(comboPoll_, fontBody_);
    if (comboDeb_) SetFont(comboDeb_, fontBody_);
    if (chkAngle_) SetFont(chkAngle_, fontBody_);
    if (chkStartup_) SetFont(chkStartup_, fontBody_);
    for (HWND b : stageButtons_) SetFont(b, fontBodyBold_);
}

void MainWindow::Layout() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int W = rc.right;
    int H = rc.bottom;
    int margin = Scale(18);
    int headerH = Scale(58);
    int footerH = Scale(28);
    int contentTop = margin + headerH;
    int contentBottom = H - footerH - Scale(4);
    int leftW = ClampInt(Scale(320), Scale(270), Scale(360));
    int rightX = margin + leftW + Scale(16);
    int rightW = W - margin - rightX;

    rcHeader_ = {margin, margin, W - margin, margin + headerH};
    rcList_ = {margin, contentTop, margin + leftW, contentBottom};
    rcDevice_ = {rightX, contentTop, rightX + rightW, contentTop + Scale(92)};
    rcBattery_ = {rightX, rcDevice_.bottom + Scale(12), rightX + rightW,
                  rcDevice_.bottom + Scale(12) + Scale(140)};
    rcDpi_ = {rightX, rcBattery_.bottom + Scale(12), rightX + rightW,
              rcBattery_.bottom + Scale(12) + Scale(156)};
    rcPerf_ = {rightX, rcDpi_.bottom + Scale(12), rightX + rightW,
               rcDpi_.bottom + Scale(12) + Scale(116)};
    rcFooter_ = {0, H - footerH, W, H};

    int buttonW = Scale(92);
    int buttonH = Scale(32);
    MoveWindow(btnRefresh_, rcHeader_.right - buttonW, rcHeader_.top + Scale(8), buttonW, buttonH,
               TRUE);
    MoveWindow(chkStartup_, rcHeader_.right - buttonW - Scale(108), rcHeader_.top + Scale(14),
               Scale(100), Scale(24), TRUE);

    RECT listBox{rcList_.left + Scale(8), rcList_.top + Scale(40), rcList_.right - Scale(8),
                 rcList_.bottom - Scale(8)};
    MoveWindow(list_, listBox.left, listBox.top, listBox.right - listBox.left,
               listBox.bottom - listBox.top, TRUE);

    int applyW = Scale(124);
    int applyH = Scale(36);
    MoveWindow(btnApply_, rcPerf_.right - applyW, contentBottom - applyH, applyW, applyH, TRUE);

    // DPI card controls.
    int cx = rcDpi_.left + Scale(16);
    int cw = rcDpi_.right - rcDpi_.left - Scale(32);
    MoveWindow(editDpi_, rcDpi_.right - Scale(116), rcDpi_.top + Scale(30), Scale(92),
               Scale(28), TRUE);
    MoveWindow(slider_, cx, rcDpi_.top + Scale(74), cw, Scale(34), TRUE);
    int stageY = rcDpi_.top + Scale(116);
    int stageW = Scale(62);
    int gap = Scale(8);
    for (int i = 0; i < (int)stageButtons_.size(); ++i) {
        MoveWindow(stageButtons_[i], cx + i * (stageW + gap), stageY, stageW, Scale(30), TRUE);
    }

    // Performance card controls.
    int py = rcPerf_.top + Scale(34);
    MoveWindow(comboPoll_, rcPerf_.left + Scale(86), py, Scale(116), Scale(220), TRUE);
    MoveWindow(comboDeb_, rcPerf_.left + Scale(332), py, Scale(100), Scale(220), TRUE);
    MoveWindow(chkAngle_, rcPerf_.left + Scale(16), py + Scale(42), Scale(120), Scale(24), TRUE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Paint(HDC dc) {
    EnsureGdiplus();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    FillRect(mem, &rc, brushBg_);

    PaintHeader(mem);
    PaintListCard(mem);
    const Device* d = Selected();
    if (d) {
        PaintDeviceCard(mem, *d);
        PaintBatteryCard(mem, *d);
        PaintDpiCard(mem, *d);
        PaintPerfCard(mem, *d);
    } else {
        PaintEmptyState(mem);
    }
    PaintFooter(mem);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::PaintHeader(HDC dc) {
    RECT title{rcHeader_.left, rcHeader_.top, rcHeader_.right - Scale(240),
               rcHeader_.top + Scale(34)};
    DrawTextIn(dc, L"2.4G 设备中心", title, fontTitle_, kText,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT sub{rcHeader_.left, rcHeader_.top + Scale(34), rcHeader_.right - Scale(240),
             rcHeader_.top + Scale(56)};
    DrawTextIn(dc, L"电量 · DPI · 轮询率 · 原生 HID", sub, fontSub_, kMuted,
               DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

void MainWindow::PaintListCard(HDC dc) {
    FillRounded(dc, rcList_, Scale(12), kCard, kBorder);
    RECT hdr{rcList_.left + Scale(16), rcList_.top + Scale(10), rcList_.right - Scale(16),
             rcList_.top + Scale(36)};
    DrawTextIn(dc, L"设备", hdr, fontBodyBold_, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    std::wstring count = std::to_wstring(snapshot_.devices.size()) + L" 台";
    DrawTextIn(dc, count, hdr, fontSmall_, kMuted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

void MainWindow::PaintDeviceCard(HDC dc, const Device& d) {
    FillRounded(dc, rcDevice_, Scale(12), kCard, kBorder);
    int x = rcDevice_.left + Scale(16);
    int y = rcDevice_.top + Scale(8);
    RECT name{x, y, rcDevice_.right - Scale(180), y + Scale(34)};
    DrawTextIn(dc, d.name, name, fontTitle_, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    std::wstring vidpid = Fmt(L"VID_%04X  PID_%04X", d.vid, d.pid);
    RECT vp{rcDevice_.right - Scale(190), y + Scale(2), rcDevice_.right - Scale(16),
            y + Scale(26)};
    DrawTextIn(dc, vidpid, vp, fontSmall_, kMuted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    int by = rcDevice_.top + Scale(46);
    int bh = Scale(20);
    int bx = x;
    bx += DrawPill(dc, bx, by, KindName(d.kind), fontBadge_, kAccentSoft, kAccent, bh) + Scale(6);
    bx += DrawPill(dc, bx, by, ConnName(d.conn), fontBadge_, RGB(236, 253, 245), kGreen, bh) +
          Scale(6);
    bx += DrawPill(dc, bx, by, FriendlyProtocolName(d), fontBadge_, RGB(243, 244, 246), kMuted,
                   bh) +
          Scale(6);
    std::wstring status = DeviceStatusText(d);
    COLORREF sc = d.online ? (d.stale ? kAmber : kGreen) : RGB(156, 163, 175);
    DrawPill(dc, bx, by, status, fontBadge_, RGB(249, 250, 251), sc, bh);
    if (!d.note.empty()) {
        RECT note{x, rcDevice_.bottom - Scale(22), rcDevice_.right - Scale(16),
                  rcDevice_.bottom - Scale(6)};
        DrawTextIn(dc, d.note, note, fontSmall_, kMuted,
                   DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

void MainWindow::PaintBatteryCard(HDC dc, const Device& d) {
    FillRounded(dc, rcBattery_, Scale(12), kCard, kBorder);
    DrawSectionTitle(dc, rcBattery_.left + Scale(16), rcBattery_.top + Scale(8), L"电量",
                     fontBodyBold_);

    int cx = rcBattery_.left + Scale(82);
    int cy = rcBattery_.top + Scale(84);
    int radius = Scale(48);
    float displayPct = -1.0f;
    if (d.batteryValid) {
        displayPct = ringShown_ >= 0 ? (float)ringShown_ : (float)d.battery;
        displayPct = std::max(0.0f, std::min(100.0f, displayPct));
    }
    int displayInt = displayPct >= 0 ? (int)lround(displayPct) : -1;
    std::wstring ringText = displayInt >= 0 ? Fmt(L"%d%%", displayInt) : L"--";
    float pulse = 0.5f;
    if (d.charging) {
        double phase = (double)(GetTickCount64() % 1400) / 1400.0;
        pulse = 0.5f + 0.5f * (float)std::sin(phase * 2.0 * 3.14159265358979323846);
    }
    DrawRingGauge(dc, cx, cy, radius, Scale(10), displayPct, d.charging,
                  d.stale || !d.batteryValid, fontRing_, ringText, kText, pulse);

    int x = rcBattery_.left + Scale(158);
    int y = rcBattery_.top + Scale(32);
    std::wstring state;
    if (!d.batteryValid) state = d.stale ? L"上次读数" : L"等待电量读数";
    else if (d.charging) state = d.battery >= 100 ? L"已充满" : L"充电中";
    else if (d.stale) state = L"上次读数";
    else state = L"电池供电";
    RECT st{x, y, rcBattery_.right - Scale(16), y + Scale(28)};
    DrawTextIn(dc, state, st, fontBodyBold_, d.charging ? kGreen : kText,
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
    else if (d.batteryValid) extra = L"实时读取 · 圆形电量仪表";
    else extra = L"设备休眠或电量暂不可读";

    RECT ex{x, infoY + Scale(30), rcBattery_.right - Scale(16), infoY + Scale(54)};
    DrawTextIn(dc, extra, ex, fontSmall_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

void MainWindow::PaintDpiCard(HDC dc, const Device& d) {
    FillRounded(dc, rcDpi_, Scale(12), kCard, kBorder);
    DrawSectionTitle(dc, rcDpi_.left + Scale(16), rcDpi_.top + Scale(10), L"DPI",
                     fontBodyBold_);
    if (!d.dpi.valid) {
        RECT r{rcDpi_.left + Scale(16), rcDpi_.top + Scale(46), rcDpi_.right - Scale(16),
               rcDpi_.top + Scale(104)};
        std::wstring msg = d.note.empty()
                               ? L"此设备不提供可读 DPI（键盘/耳机或协议未公开）"
                               : d.note;
        DrawTextIn(dc, msg, r, fontBody_, kMuted, DT_LEFT | DT_WORDBREAK);
        return;
    }
    std::wstring value = d.dpi.currentKnown ? std::to_wstring(dpiValue_) : L"档位表";
    RECT big{rcDpi_.left + Scale(16), rcDpi_.top + Scale(28), rcDpi_.left + Scale(220),
             rcDpi_.top + Scale(64)};
    DrawTextIn(dc, value, big, fontTitle_, kText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    if (d.dpi.currentKnown) {
        RECT rng{rcDpi_.left + Scale(130), rcDpi_.top + Scale(32), rcDpi_.right - Scale(130),
                 rcDpi_.top + Scale(58)};
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
    FillRounded(dc, rcPerf_, Scale(12), kCard, kBorder);
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
        RECT l{rcPerf_.left + Scale(16), rcPerf_.top + Scale(38), rcPerf_.left + Scale(82),
               rcPerf_.top + Scale(62)};
        DrawTextIn(dc, L"轮询率", l, fontSmall_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    if (d.debounce.valid) {
        RECT l{rcPerf_.left + Scale(250), rcPerf_.top + Scale(38), rcPerf_.left + Scale(328),
               rcPerf_.top + Scale(62)};
        DrawTextIn(dc, L"按键去抖", l, fontSmall_, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
}

void MainWindow::PaintFooter(HDC dc) {
    FillRect(dc, &rcFooter_, brushCard_);
    RECT line{0, rcFooter_.top, rcFooter_.right, rcFooter_.top + 1};
    HBRUSH lb = CreateSolidBrush(kBorder);
    FillRect(dc, &line, lb);
    DeleteObject(lb);

    bool toastActive = !toast_.empty() && GetTickCount64() < toastUntil_;
    std::wstring left = toastActive ? toast_ : snapshot_.status;
    COLORREF color = toastActive ? (toastOk_ ? kGreen : kRed) : kMuted;
    RECT lr{Scale(18), rcFooter_.top, rcFooter_.right - Scale(250), rcFooter_.bottom};
    DrawTextIn(dc, left, lr, fontSmall_, color, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT rr{rcFooter_.right - Scale(250), rcFooter_.top, rcFooter_.right - Scale(18),
            rcFooter_.bottom};
    std::wstring right = Fmt(L"轮询 %d 秒 · 设置需手动应用", manager_.PollSeconds());
    DrawTextIn(dc, right, rr, fontSmall_, kMuted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

void MainWindow::PaintEmptyState(HDC dc) {
    RECT area{rcDevice_.left, rcDevice_.top, rcDevice_.right, rcPerf_.bottom};
    FillRounded(dc, area, Scale(12), kCard, kBorder);
    RECT icon{area.left + (area.right - area.left) / 2 - Scale(30), area.top + Scale(70),
              area.left + (area.right - area.left) / 2 + Scale(30), area.top + Scale(130)};
    DrawDeviceGlyph(dc, Kind::Mouse, icon, RGB(148, 163, 184), false);
    RECT t1{area.left, area.top + Scale(150), area.right, area.top + Scale(184)};
    DrawTextIn(dc, L"未发现兼容的 2.4G 设备", t1, fontTitle_, kText,
               DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT t2{area.left + Scale(30), area.top + Scale(188), area.right - Scale(30),
            area.top + Scale(250)};
    DrawTextIn(dc,
               L"请插入 2.4G 接收器并移动鼠标将其唤醒。\n"
               L"支持 Logitech、ASUS ROG、Razer、Rapoo、Compx、ATK/VXE、LAMZU、G-Wolves 等。",
               t2, fontBody_, kMuted, DT_CENTER | DT_WORDBREAK);
}

void MainWindow::OnSnapshot() {
    Snapshot s = manager_.GetSnapshot();
    std::wstring keep = selectedId_;
    snapshot_ = std::move(s);

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
    if (!dirty_ || selectionChanged) UpdateControlsFromDevice();
    RefreshTray();
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
    for (const auto& d : snapshot_.devices) ids.push_back(d.id);
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
    bool showDpi = has && d->dpi.valid;
    bool dpiWritable = showDpi && d->dpi.canWrite && d->dpi.currentKnown;
    ShowWindow(slider_, dpiWritable ? SW_SHOW : SW_HIDE);
    ShowWindow(editDpi_, dpiWritable ? SW_SHOW : SW_HIDE);
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
            ShowWindow(stageButtons_[i], SW_SHOW);
            // Read-only protocols still allow browsing the stored stages.
            EnableWindow(stageButtons_[i], d->online);
        } else {
            ShowWindow(stageButtons_[i], SW_HIDE);
        }
    }

    bool showPoll = has && d->polling.valid;
    ShowWindow(comboPoll_, showPoll ? SW_SHOW : SW_HIDE);
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
    ShowWindow(comboDeb_, showDeb ? SW_SHOW : SW_HIDE);
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
    ShowWindow(chkAngle_, showAngle ? SW_SHOW : SW_HIDE);
    EnableWindow(chkAngle_, showAngle && d->angle.canWrite && d->online);
    if (showAngle) {
        SendMessageW(chkAngle_, BM_SETCHECK, d->angle.on ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    bool canWrite = has && d->online && (d->dpi.canWrite || d->polling.canWrite ||
                                         d->debounce.canWrite || d->angle.canWrite);
    ShowWindow(btnApply_, canWrite ? SW_SHOW : SW_HIDE);
    EnableWindow(btnApply_, canWrite);

    dirty_ = false;
    dpiDirty_ = pollingDirty_ = debounceDirty_ = angleDirty_ = stageDirty_ = false;
    updatingControls_ = false;
    Layout();
    if (d) AnimateBattery(*d);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnSelectIndex(int index) {
    if (index < 0 || index >= (int)snapshot_.devices.size()) {
        selectedIndex_ = -1;
        selectedId_.clear();
        UpdateControlsFromDevice();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    selectedIndex_ = index;
    selectedId_ = snapshot_.devices[index].id;
    dirty_ = false;
    UpdateControlsFromDevice();
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
    stageDirty_ = true;
    dpiValue_ = d->dpi.stages[index];
    dirty_ = true;
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
    if (d->dpi.valid && d->dpi.canWrite && d->dpi.currentKnown) {
        req.setDpi = true;
        req.dpiX = dpiValue_;
        req.dpiY = dpiValue_;
        req.stageIndex = stageIndex_;
        req.selectStage = stageDirty_ && stageIndex_ >= 0;
    }
    if (d->polling.valid && d->polling.canWrite) {
        int sel = (int)SendMessageW(comboPoll_, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)d->polling.options.size()) {
            req.setPolling = true;
            req.pollingHz = d->polling.options[sel];
        }
    }
    if (d->debounce.valid && d->debounce.canWrite) {
        int sel = (int)SendMessageW(comboDeb_, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)d->debounce.options.size()) {
            req.setDebounce = true;
            req.debounceMs = d->debounce.options[sel];
        }
    }
    if (d->angle.valid && d->angle.canWrite) {
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
    int idx = PrimaryDeviceIndex();
    const Device* d = idx >= 0 ? &snapshot_.devices[idx] : nullptr;
    int pct = d && d->batteryValid ? d->battery : -1;
    bool charging = d && d->charging;
    bool stale = !d || d->stale || !d->batteryValid;

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
        if (!snapshot_.devices.empty()) {
            tip += L"\n" + snapshot_.devices[0].name + L"（等待电量）";
        } else {
            tip += L"\n未检测到兼容设备";
        }
    }
    if (tip.size() > 120) tip.resize(120);

    std::wstring key = Fmt(L"%d|%d|%d|%d|%d|%s", pct, charging ? 1 : 0, stale ? 1 : 0,
                           d ? (int)d->kind : -1, (int)snapshot_.devices.size(),
                           d ? d->name.c_str() : L"");
    if (key != lastTrayKey_ || !trayIcon_) {
        HICON icon = CreateBatteryIcon(pct, charging, stale, GetSystemMetrics(SM_CXSMICON));
        if (icon) {
            tray_.Update(icon, tip);
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = icon;
        }
        lastTrayKey_ = key;
    }

    if (d && d->batteryValid && d->battery <= 20 && d->battery > 0 && !d->charging && !d->stale) {
        if (!lowNotified_) {
            lowNotified_ = true;
            tray_.Balloon(L"电量偏低", d->name + L" 剩余 " + std::to_wstring(d->battery) +
                                          L"%，请及时充电。",
                          NIIF_WARNING);
        }
    } else if (!d || d->charging || d->battery > 25 || d->battery < 0) {
        lowNotified_ = false;
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
    InvalidateRect(hwnd_, &rcFooter_, FALSE);
}

void MainWindow::StartAnimTimer() {
    if (!animTimerActive_) {
        SetTimer(hwnd_, 2, 33, nullptr);
        animTimerActive_ = true;
    }
}

void MainWindow::AnimateBattery(const Device& d) {
    double target = (d.batteryValid && d.battery >= 0) ? (double)d.battery : -1.0;
    if (ringDeviceId_ != d.id) {
        ringDeviceId_ = d.id;
        ringShown_ = target >= 0 ? 0.0 : -1.0;
        ringFrom_ = 0.0;
        ringTo_ = target;
        ringAnimating_ = target >= 0;
        ringStart_ = GetTickCount64();
    } else if (target < 0) {
        ringShown_ = -1.0;
        ringAnimating_ = false;
    } else if (!ringAnimating_ && std::fabs(target - ringShown_) > 0.05) {
        ringFrom_ = ringShown_ >= 0 ? ringShown_ : 0.0;
        ringTo_ = target;
        ringStart_ = GetTickCount64();
        ringAnimating_ = true;
    } else if (ringAnimating_) {
        ringTo_ = target;
    }
    if (ringAnimating_ || d.charging) StartAnimTimer();
    InvalidateRect(hwnd_, &rcBattery_, FALSE);
}

void MainWindow::OnAnimTimer() {
    const Device* d = Selected();
    bool active = false;
    if (ringAnimating_) {
        double t = (double)(GetTickCount64() - ringStart_) / 650.0;
        if (t >= 1.0) {
            ringShown_ = ringTo_;
            ringAnimating_ = false;
        } else {
            double eased = 1.0 - std::pow(1.0 - t, 3.0);
            ringShown_ = ringFrom_ + (ringTo_ - ringFrom_) * eased;
            active = true;
        }
    }
    if (d && d->charging) active = true;
    InvalidateRect(hwnd_, &rcBattery_, FALSE);
    if (!active) {
        KillTimer(hwnd_, 2);
        animTimerActive_ = false;
    }
}

void MainWindow::ShowFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    if (const Device* d = Selected()) AnimateBattery(*d);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::HideToTray() {
    if (animTimerActive_) {
        KillTimer(hwnd_, 2);
        animTimerActive_ = false;
    }
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
        UpdateStartupCheckbox();
    } else if (cmd == ID_MENU_LOG) {
        ShellExecuteW(nullptr, L"open", AppLogPath().c_str(), nullptr, ExeDir().c_str(),
                      SW_SHOWNORMAL);
    } else if (cmd == ID_MENU_EXIT) {
        exiting_ = true;
        DestroyWindow(hwnd_);
    }
    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

void MainWindow::UpdateStartupCheckbox() {
    if (!chkStartup_) return;
    SendMessageW(chkStartup_, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
}

LRESULT MainWindow::Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            hwnd_ = hwnd;
            dpi_ = GetDpiForHwnd(hwnd);
            brushCard_ = CreateSolidBrush(kCard);
            brushBg_ = CreateSolidBrush(kBg);
            brushCard_global = brushCard_;
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
            Layout();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp) ? HIWORD(wp) : GetDpiForHwnd(hwnd);
            RecreateFonts();
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = Scale(900);
            mmi->ptMinTrackSize.y = Scale(740);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* mis = (MEASUREITEMSTRUCT*)lp;
            if (mis->CtlID == ID_LIST) {
                mis->itemHeight = Scale(66);
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
                InflateRect(&r, -Scale(4), -Scale(3));
                FillRounded(dis->hDC, r, Scale(9), sel ? kAccentSoft : kCard,
                            sel ? kAccentSoft : kCard);
                RECT icon{r.left + Scale(10), r.top + Scale(10), r.left + Scale(42),
                          r.top + Scale(42)};
                DrawDeviceGlyph(dis->hDC, d.kind, icon, sel ? kAccent : RGB(100, 116, 139), sel);
                RECT name{r.left + Scale(52), r.top + Scale(7), r.right - Scale(76),
                          r.top + Scale(30)};
                DrawTextIn(dis->hDC, d.name, name, fontBodyBold_, kText,
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RECT sub{r.left + Scale(52), r.top + Scale(32), r.right - Scale(76),
                         r.top + Scale(52)};
                std::wstring info = Fmt(L"%s · %s", ConnName(d.conn), DeviceStatusText(d).c_str());
                DrawTextIn(dis->hDC, info, sub, fontSmall_, kMuted,
                           DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                std::wstring ringText =
                    d.batteryValid ? std::to_wstring(ClampInt(d.battery, 0, 100)) : L"--";
                DrawRingGauge(dis->hDC, r.right - Scale(34), r.top + Scale(30), Scale(19),
                              Scale(5), d.battery, d.charging, d.stale || !d.batteryValid,
                              fontBodyBold_, ringText, kText);
                return TRUE;
            }
            if (dis->CtlID == ID_REFRESH || dis->CtlID == ID_APPLY ||
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
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            bool onHeader = (HWND)lp == chkStartup_;
            SetBkColor(dc, onHeader ? kBg : kCard);
            SetTextColor(dc, kText);
            return (LRESULT)(onHeader ? brushBg_ : brushCard_);
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
            if (id == ID_REFRESH) {
                manager_.RequestRefresh();
                ShowToast(L"正在刷新…", true, 2);
                return 0;
            }
            if (id == ID_APPLY) {
                ApplySettings();
                return 0;
            }
            if (id == ID_DPI_EDIT && code == EN_CHANGE) {
                OnDpiEditChanged();
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
            if (id == ID_STARTUP && code == BN_CLICKED) {
                SetAutoStart(SendMessageW(chkStartup_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                UpdateStartupCheckbox();
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
        case WM_TIMER:
            if (wp == 1) {
                if (!toast_.empty() && GetTickCount64() >= toastUntil_) {
                    toast_.clear();
                    InvalidateRect(hwnd, &rcFooter_, FALSE);
                }
            } else if (wp == 2) {
                OnAnimTimer();
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
        case WM_CLOSE:
            if (exiting_) {
                DestroyWindow(hwnd);
            } else {
                HideToTray();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            animTimerActive_ = false;
            manager_.Stop();
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
        case WM_NCDESTROY: {
            if (brushCard_) DeleteObject(brushCard_);
            if (brushBg_) DeleteObject(brushBg_);
            brushCard_ = brushBg_ = nullptr;
            brushCard_global = nullptr;
            auto del = [](HFONT& f) {
                if (f) DeleteObject(f);
                f = nullptr;
            };
            del(fontTitle_);
            del(fontSub_);
            del(fontBody_);
            del(fontBodyBold_);
            del(fontSmall_);
            del(fontBig_);
            del(fontRing_);
            del(fontBadge_);
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
