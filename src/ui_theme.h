#pragma once

#include "common.h"

// 共享主题：颜色、字体与基础绘制。ui.cpp / ui_test_view.cpp 共用，避免两套色值漂移。
namespace uitheme {

inline constexpr COLORREF kBg = RGB(245, 246, 247);
inline constexpr COLORREF kCard = RGB(255, 255, 255);
inline constexpr COLORREF kBorder = RGB(229, 230, 235);
inline constexpr COLORREF kText = RGB(31, 35, 41);
inline constexpr COLORREF kMuted = RGB(143, 149, 158);
inline constexpr COLORREF kSub = RGB(100, 106, 115);
inline constexpr COLORREF kHover = RGB(242, 243, 245);
inline constexpr COLORREF kAccent = RGB(43, 125, 246);
inline constexpr COLORREF kAccentSoft = RGB(234, 242, 255);
inline constexpr COLORREF kGreen = RGB(0, 180, 42);
inline constexpr COLORREF kAmber = RGB(255, 125, 0);
inline constexpr COLORREF kRed = RGB(245, 63, 63);
inline constexpr COLORREF kTrack = RGB(235, 238, 242);

// 毛玻璃质感：背景渐变端点与玻璃/高光/投影色（alpha 由函数内部处理）。
inline constexpr COLORREF kBgTop = RGB(245, 248, 253);
inline constexpr COLORREF kBgBottom = RGB(226, 234, 246);

HFONT MakeFont(UINT dpi, int pt, int weight, const wchar_t* face);

void SetFont(HWND wnd, HFONT font);
void SetFont(HDC dc, HFONT font);

void DrawTextIn(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color,
                UINT flags);
void FillRounded(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border);
// 毛玻璃背景：垂直渐变 + 两团大半径柔光色斑，必须在所有面板之前绘制。
void PaintBackdrop(HDC dc, const RECT& rc);
// 从背景缓存里恢复局部区域；用于只重绘某个动画卡片的场景。
// 缓存尚未建立时返回 false，调用方应退回整窗重绘。
bool PaintBackdropRegion(HDC dc, const RECT& rc);
// 玻璃卡片：半透明白 + 白色高光边 + 柔和投影。
void GlassPanel(HDC dc, const RECT& rc, int radius, int fillAlpha = 204, bool shadow = true);
// 无边框半透明圆角填充（导航栏/页脚/列表底板）。
void GlassFill(HDC dc, const RECT& rc, int radius, int alpha);
void DrawBadge(HDC dc, const RECT& rc, const std::wstring& text, HFONT font, COLORREF bg,
               COLORREF fg);

}  // namespace uitheme
