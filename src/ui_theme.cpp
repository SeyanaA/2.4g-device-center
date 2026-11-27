#include "ui_theme.h"

// gdiplus.h 依赖 objidl/propidl 里的 PROPID；必须先包含它们（与 ui.cpp 顺序一致）。
#include <objidl.h>
#include <propidl.h>

#include <gdiplus.h>

#include <cmath>
#include <vector>

#include "svg_icon.h"

namespace uitheme {

namespace {

constexpr DWORD ArgbOf(int alpha, COLORREF c) {
    return ((DWORD)(alpha & 0xFF) << 24) | ((DWORD)GetRValue(c) << 16) |
           ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
}

// 背景渐变与柔光色斑在窗口尺寸不变时是常量，缓存成位图，避免每帧重画
// 30+ 次抗锯齿椭圆（充电时窗口是 60 fps 全量重绘）。
struct BackdropCache {
    HDC mem = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0;
    int h = 0;
};

BackdropCache g_backdrop;

void RenderBackdrop(HDC dc, int w, int h) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);

    Gdiplus::Rect r(0, 0, w, h);
    Gdiplus::LinearGradientBrush grad(
        r, Gdiplus::Color(255, GetRValue(kBgTop), GetGValue(kBgTop), GetBValue(kBgTop)),
        Gdiplus::Color(255, GetRValue(kBgBottom), GetGValue(kBgBottom), GetBValue(kBgBottom)),
        Gdiplus::LinearGradientModeVertical);
    g.FillRectangle(&grad, r);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

    struct Glow {
        float cx, cy, radius;
        COLORREF color;
        int alpha;
    };
    const Glow glows[] = {
        {0.78f, 0.04f, 0.62f, kAccent, 30},  // 右上冷光：正好落在内容卡片后面
        {0.12f, 0.94f, 0.50f, kGreen, 18},   // 左下暖绿
    };
    for (const Glow& glow : glows) {
        const float cx = w * glow.cx;
        const float cy = h * glow.cy;
        const float rr = (float)(w > h ? w : h) * glow.radius;
        for (int i = 12; i >= 1; --i) {
            const float t = (float)i / 12.0f;
            const int a = (int)std::lround(glow.alpha * (1.0f - t) * (1.0f - t));
            if (a <= 0) continue;
            const int radius = (int)std::lround(rr * t);
            RECT er{(LONG)std::lround(cx) - radius, (LONG)std::lround(cy) - radius,
                    (LONG)std::lround(cx) + radius, (LONG)std::lround(cy) + radius};
            svgicon::FillRoundRectArgb(dc, er, radius, ArgbOf(a, glow.color), 0);
        }
    }
}

}  // namespace

void PaintBackdrop(HDC dc, const RECT& rc) {
    if (dc == nullptr) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (!svgicon::EnsureStarted()) return;

    if (g_backdrop.mem == nullptr) {
        g_backdrop.mem = CreateCompatibleDC(nullptr);
    }
    if (g_backdrop.bmp == nullptr || g_backdrop.w != w || g_backdrop.h != h) {
        if (g_backdrop.bmp) {
            SelectObject(g_backdrop.mem, g_backdrop.old);
            DeleteObject(g_backdrop.bmp);
            g_backdrop.bmp = nullptr;
        }
        HDC screen = GetDC(nullptr);
        g_backdrop.bmp = CreateCompatibleBitmap(screen, w, h);
        ReleaseDC(nullptr, screen);
        if (g_backdrop.bmp == nullptr) return;
        g_backdrop.old = SelectObject(g_backdrop.mem, g_backdrop.bmp);
        g_backdrop.w = w;
        g_backdrop.h = h;
        RenderBackdrop(g_backdrop.mem, w, h);
    }
    BitBlt(dc, rc.left, rc.top, w, h, g_backdrop.mem, 0, 0, SRCCOPY);
}

bool PaintBackdropRegion(HDC dc, const RECT& rc) {
    if (dc == nullptr || g_backdrop.mem == nullptr || g_backdrop.bmp == nullptr) return false;
    if (rc.left < 0 || rc.top < 0 || rc.right > g_backdrop.w || rc.bottom > g_backdrop.h)
        return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return true;
    BitBlt(dc, rc.left, rc.top, w, h, g_backdrop.mem, rc.left, rc.top, SRCCOPY);
    return true;
}

void GlassPanel(HDC dc, const RECT& rc, int radius, int fillAlpha, bool shadow) {
    if (dc == nullptr || rc.right <= rc.left || rc.bottom <= rc.top) return;
    if (shadow) {
        // 3 层足够柔和；层数越多面积越大，全窗重绘时的成本主要来自这里。
        for (int i = 3; i >= 1; --i) {
            RECT s{rc.left - i, rc.top - i + 3, rc.right + i, rc.bottom + i + 3};
            svgicon::FillRoundRectArgb(dc, s, radius + i, 0x0A0F172A, 0);
        }
    }
    // 深色细描边让玻璃版面在浅色背景上有明确边界；白色高光靠上层的 1px 内描边。
    svgicon::FillRoundRectArgb(dc, rc, radius, ArgbOf(fillAlpha, kCard), 0x1E0F172A);
}

void GlassFill(HDC dc, const RECT& rc, int radius, int alpha) {
    if (dc == nullptr || rc.right <= rc.left || rc.bottom <= rc.top) return;
    svgicon::FillRoundRectArgb(dc, rc, radius, ArgbOf(alpha, kCard), 0);
}

HFONT MakeFont(UINT dpi, int pt, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, (int)dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void SetFont(HWND wnd, HFONT font) {
    if (wnd) SendMessageW(wnd, WM_SETFONT, (WPARAM)font, TRUE);
}

void SetFont(HDC dc, HFONT font) {
    if (dc && font) SelectObject(dc, font);
}

void DrawTextIn(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
                UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    // 自绘文本不解析 & 助记符：设备名/日志里的 & 必须原样显示。
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, flags | DT_NOPREFIX);
}

void FillRounded(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border) {
    // 改为 GDI+ 抗锯齿圆角：GDI RoundRect 的角部会产生 1px 阶梯，
    // 在卡片、导航胶囊和徽章上尤其明显（见 docs/SPEC-vector-icons.md）。
    svgicon::FillRoundRect(dc, rc, radius, fill, border);
}

void DrawBadge(HDC dc, const RECT& rc, const std::wstring& text, HFONT font, COLORREF bg,
               COLORREF fg) {
    FillRounded(dc, rc, (rc.bottom - rc.top) / 2, bg, bg);
    DrawTextIn(dc, text, rc, font, fg,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

}  // namespace uitheme

