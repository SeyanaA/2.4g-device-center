#pragma once

// SVG 图标 / 矢量绘制层。
//
// 设计约束：
// - 图标以 24x24 viewBox 的 SVG path `d` 数据为唯一来源，运行时用 GDI+
//   GraphicsPath 抗锯齿渲染，不产生任何位图资源，因此任意 DPI 都清晰。
// - 只支持图标需要的 SVG path 子集：M L H V C S Q T A Z（大小写/相对绝对）。
// - 描边宽度以 viewBox 单位给出，渲染时按目标矩形等比缩放。
//
// 约定：所有函数都接受调用方已经创建好的内存 DC（与既有 GDI 绘制混用），
// 内部只在函数作用域内创建 Gdiplus::Graphics，保证与 GDI 调用的先后顺序。

#include <windows.h>

#include <string>

namespace svgicon {

enum class Id {
    None = 0,
    NavDevice,   // 导航：设备（鼠标）
    NavLog,      // 导航：日志
    NavTest,     // 导航：测试（靶心）
    Mouse,
    Keyboard,
    Headset,
    Other,       // 通用 HID
    Bolt,        // 充电闪电
    Bluetooth,
    Usb,
    Link,        // 有线连接
};

// 在 box 内按 24x24 viewBox 等比缩放并居中绘制图标。
// strokeUnits：viewBox 单位线宽（典型 1.8；0 表示使用图标默认值）。
// filled：轮廓内加 12% 透明底色（用于选中态）；Bolt 这类闭合实心图标
// 改为不透明填充且不再描边，避免变成一层淡影。
void Draw(HDC dc, Id id, const RECT& box, COLORREF color, float strokeUnits = 0.0f,
          bool filled = false);

// 直接绘制一段 SVG path 数据（供图标表以外的场景复用）。
void DrawPath(HDC dc, const wchar_t* d, const RECT& box, COLORREF color,
              float strokeUnits, bool filled);

// 抗锯齿圆角矩形。用于替换 GDI RoundRect，消除卡片/胶囊的 1px 毛边。
// border 与 fill 相同即“无边框填充”。
void FillRoundRect(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF border);

// 带 alpha 的圆角矩形/矩形填充（ARGB = 0xAARRGGBB）。
// 用于毛玻璃面板与柔和投影；border 的 alpha 为 0 时不描边。
void FillRoundRectArgb(HDC dc, const RECT& rc, int radius, DWORD argbFill, DWORD argbBorder);
void FillRectArgb(HDC dc, const RECT& rc, DWORD argb);

// 抗锯齿圆环进度。percent 取 0..100；startDeg 默认 -90（12 点方向）。
void DrawRing(HDC dc, int cx, int cy, int radius, int thickness, COLORREF track,
              COLORREF fill, float percent, float startDeg = -90.0f);

// 抗锯齿实心多边形（充电闪电等）。
void FillPolygon(HDC dc, const POINT* pts, int count, COLORREF color);

// 缩放后的图标默认线宽（供调用方需要对齐时使用）。
float DefaultStrokeUnits(Id id);

// 进程内 GDI+ 初始化（幂等）。需要直接用 GDI+ 绘制时先调用一次。
bool EnsureStarted();

// 每帧共享的 GDI+ 上下文：Paint 开始时 BeginFrame(dc)，结束时 EndFrame()。
// 之前每个圆角/图标各自 new 一个 Graphics，满屏玻璃卡片时一帧上百个对象，
// CPU 会飙到单核 70%+；整帧复用一个即可回到个位数。
// 未调用 BeginFrame 时各函数自动创建临时 Graphics（测试/零散调用仍然正确）。
void BeginFrame(HDC dc);
void EndFrame();

// 仅供测试/诊断：把 SVG path 解析后统计包围盒与锚点数量。
// 曲线只计端点和控制点，不做细分；解析失败返回 false。
bool InspectPath(const wchar_t* d, RECT* bounds, int* points);

}  // namespace svgicon
