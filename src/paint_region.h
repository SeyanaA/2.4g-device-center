#pragma once

#include <windows.h>

// 局部重绘的几何判定（不调用任何 GDI）：把 WM_PAINT 的 ps.rcPaint 映射成
// “整帧 / 只画脏区 / 跳过该绘制块”。单独成文件是为了能用单测覆盖边界，
// 不必启动窗口；UI 侧只做绘制，不做算术。
namespace paintregion {

enum class Mode {
    Full,   // 整帧重绘
    Dirty,  // 只重画脏区
};

bool IsEmpty(const RECT& rc);

// 把 dirty 夹到 client 内；返回 false 表示夹完为空。
bool ClampToClient(RECT& dirty, const RECT& client);

// dirty 是否覆盖整个 client（两者都必须非空）。
bool Covers(const RECT& dirty, const RECT& client);

// 一次 WM_PAINT 的策略：脏区为空或覆盖整窗时整帧，其余增量。
// 空脏区视为“没有可信刷新范围”，宁可按整帧处理，避免留下半块旧画面。
Mode Decide(const RECT& dirty, const RECT& client);

// 绘制块是否落在脏区。pad 外扩用于阴影/焦点框这类画到矩形外的装饰：
// 卡片阴影 3px、焦点框 2px，外扩不足会在交界处留下旧像素。
bool Touches(const RECT& dirty, const RECT& item, int pad);

}  // namespace paintregion
