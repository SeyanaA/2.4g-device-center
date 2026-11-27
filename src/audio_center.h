#pragma once

#include <string>

#include "common.h"

// 音频面板：作为主窗口内容区的子窗口显示（不再创建顶层窗口）。
// 打开后隐藏设备列表，内容区展示系统输出 / 应用音量 / 麦克风；返回由主窗口折叠。
namespace audiocenter {

struct Context {
    std::wstring deviceName;        // 当前设备名（用于匹配麦克风）
    std::wstring product;           // 当前产品名
    std::wstring renderEndpointId;  // 已匹配的播放端点；空则用系统默认
    bool demo = false;
};

// 主窗口 -> 音频面板：用户点击“返回设备”。
// 注意不要复用 ui.cpp 的 WM_APP+37（滑块消息），避免消息号重叠后被误路由。
constexpr UINT WM_BACK = WM_APP + 39;

bool ShowPanel(HWND parent, const RECT& area, const Context& ctx);
void HidePanel();
void SetPanelBounds(const RECT& area);
void SetPanelDpi(UINT dpi);
bool PanelVisible();
// 过渡用：临时隐藏/显示面板，不销毁窗口；隐藏时停止刷新与麦克风采集。
void SetPanelVisible(bool visible);
HWND PanelHwnd();
// 主窗口最小化/恢复：暂停面板刷新与麦克风采集，恢复后重新读取一次。
void SetPanelSuspended(bool suspended);
// 主窗口销毁时释放面板单例（停止麦克风线程、回收字体/缓冲）。
void Shutdown();

}  // namespace audiocenter
