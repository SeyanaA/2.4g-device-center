#pragma once

// 主窗口键盘导航的纯逻辑部分：焦点槽位计算与循环，便于单元测试。
// UI 线程负责把槽位映射到具体 HWND，这里不依赖 Windows。
namespace uikey {

// 焦点在自绘导航栏（主窗口自身）时使用该槽位；原生控件使用 0..count-1。
constexpr int kFocusNav = -1;

// 在 count 个导航项中循环移动：delta 可为负数，结果始终夹在 0..count-1。
// count <= 0 时返回 kFocusNav。
int NextIndex(int current, int count, int delta);

// Tab / Shift+Tab 的槽位推进：
//   导航(kFocusNav) → 首个/末个原生控件 → ... → 末尾后回到导航。
// nativeCount <= 0 时始终返回 kFocusNav。
// current 非法（越界）时视为导航栏，避免把焦点交给不存在的控件。
int NextFocusSlot(int current, int nativeCount, bool forward);

}  // namespace uikey
