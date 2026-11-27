#include "keyboard_test.h"

#include <algorithm>
#include <cwchar>

namespace keyboardtest {
namespace {

// ANSI 104 键布局，单位坐标系：总宽 23.5u、高 6u。
// 主键区 x=0..15，导航区 x=15.5..18.5，数字小键盘 x=19.5..23.5。
const KeyDef kKeys[KeyboardTestEngine::kKeyCount] = {
    // ---- 功能键行 y=0 -----------------------------------------------------
    {L"esc", L"ESC", L"", L"ESC", 0.0f, 0.0f, 1.0f, 1.0f, VK_ESCAPE, 0},
    {L"f1", L"F1", L"", L"F1", 2.0f, 0.0f, 1.0f, 1.0f, VK_F1, 0},
    {L"f2", L"F2", L"", L"F2", 3.0f, 0.0f, 1.0f, 1.0f, VK_F2, 0},
    {L"f3", L"F3", L"", L"F3", 4.0f, 0.0f, 1.0f, 1.0f, VK_F3, 0},
    {L"f4", L"F4", L"", L"F4", 5.0f, 0.0f, 1.0f, 1.0f, VK_F4, 0},
    {L"f5", L"F5", L"", L"F5", 6.25f, 0.0f, 1.0f, 1.0f, VK_F5, 0},
    {L"f6", L"F6", L"", L"F6", 7.25f, 0.0f, 1.0f, 1.0f, VK_F6, 0},
    {L"f7", L"F7", L"", L"F7", 8.25f, 0.0f, 1.0f, 1.0f, VK_F7, 0},
    {L"f8", L"F8", L"", L"F8", 9.25f, 0.0f, 1.0f, 1.0f, VK_F8, 0},
    {L"f9", L"F9", L"", L"F9", 10.5f, 0.0f, 1.0f, 1.0f, VK_F9, 0},
    {L"f10", L"F10", L"", L"F10", 11.5f, 0.0f, 1.0f, 1.0f, VK_F10, 0},
    {L"f11", L"F11", L"", L"F11", 12.5f, 0.0f, 1.0f, 1.0f, VK_F11, 0},
    {L"f12", L"F12", L"", L"F12", 13.5f, 0.0f, 1.0f, 1.0f, VK_F12, 0},
    {L"prtsc", L"PRTSC", L"", L"PRINT SCREEN", 15.5f, 0.0f, 1.0f, 1.0f, VK_SNAPSHOT, 0},
    {L"scrolllock", L"SCRLK", L"", L"SCROLL LOCK", 16.5f, 0.0f, 1.0f, 1.0f, VK_SCROLL, 0},
    {L"pause", L"PAUSE", L"", L"PAUSE", 17.5f, 0.0f, 1.0f, 1.0f, VK_PAUSE, 0},

    // ---- 数字行 y=1 -------------------------------------------------------
    {L"`", L"`", L"~", L"`", 0.0f, 1.0f, 1.0f, 1.0f, VK_OEM_3, 0},
    {L"1", L"1", L"!", L"1", 1.0f, 1.0f, 1.0f, 1.0f, '1', 0},
    {L"2", L"2", L"@", L"2", 2.0f, 1.0f, 1.0f, 1.0f, '2', 0},
    {L"3", L"3", L"#", L"3", 3.0f, 1.0f, 1.0f, 1.0f, '3', 0},
    {L"4", L"4", L"$", L"4", 4.0f, 1.0f, 1.0f, 1.0f, '4', 0},
    {L"5", L"5", L"%", L"5", 5.0f, 1.0f, 1.0f, 1.0f, '5', 0},
    {L"6", L"6", L"^", L"6", 6.0f, 1.0f, 1.0f, 1.0f, '6', 0},
    {L"7", L"7", L"&", L"7", 7.0f, 1.0f, 1.0f, 1.0f, '7', 0},
    {L"8", L"8", L"*", L"8", 8.0f, 1.0f, 1.0f, 1.0f, '8', 0},
    {L"9", L"9", L"(", L"9", 9.0f, 1.0f, 1.0f, 1.0f, '9', 0},
    {L"0", L"0", L")", L"0", 10.0f, 1.0f, 1.0f, 1.0f, '0', 0},
    {L"-", L"−", L"_", L"-", 11.0f, 1.0f, 1.0f, 1.0f, VK_OEM_MINUS, 0},
    {L"=", L"=", L"+", L"=", 12.0f, 1.0f, 1.0f, 1.0f, VK_OEM_PLUS, 0},
    {L"backspace", L"BACKSPACE", L"", L"BACKSPACE", 13.0f, 1.0f, 2.0f, 1.0f, VK_BACK, 0},
    {L"insert", L"INS", L"", L"INSERT", 15.5f, 1.0f, 1.0f, 1.0f, VK_INSERT, 0},
    {L"home", L"HOME", L"", L"HOME", 16.5f, 1.0f, 1.0f, 1.0f, VK_HOME, 0},
    {L"pgup", L"PGUP", L"", L"PAGE UP", 17.5f, 1.0f, 1.0f, 1.0f, VK_PRIOR, 0},
    {L"numlock", L"NUM", L"", L"NUM LOCK", 19.5f, 1.0f, 1.0f, 1.0f, VK_NUMLOCK, 0},
    {L"num/", L"/", L"", L"NUM /", 20.5f, 1.0f, 1.0f, 1.0f, VK_DIVIDE, 0},
    {L"num*", L"*", L"", L"NUM *", 21.5f, 1.0f, 1.0f, 1.0f, VK_MULTIPLY, 0},
    {L"num-", L"−", L"", L"NUM −", 22.5f, 1.0f, 1.0f, 1.0f, VK_SUBTRACT, 0},

    // ---- Tab 行 y=2 -------------------------------------------------------
    {L"tab", L"TAB", L"", L"TAB", 0.0f, 2.0f, 1.5f, 1.0f, VK_TAB, 0},
    {L"q", L"Q", L"", L"Q", 1.5f, 2.0f, 1.0f, 1.0f, 'Q', 0},
    {L"w", L"W", L"", L"W", 2.5f, 2.0f, 1.0f, 1.0f, 'W', 0},
    {L"e", L"E", L"", L"E", 3.5f, 2.0f, 1.0f, 1.0f, 'E', 0},
    {L"r", L"R", L"", L"R", 4.5f, 2.0f, 1.0f, 1.0f, 'R', 0},
    {L"t", L"T", L"", L"T", 5.5f, 2.0f, 1.0f, 1.0f, 'T', 0},
    {L"y", L"Y", L"", L"Y", 6.5f, 2.0f, 1.0f, 1.0f, 'Y', 0},
    {L"u", L"U", L"", L"U", 7.5f, 2.0f, 1.0f, 1.0f, 'U', 0},
    {L"i", L"I", L"", L"I", 8.5f, 2.0f, 1.0f, 1.0f, 'I', 0},
    {L"o", L"O", L"", L"O", 9.5f, 2.0f, 1.0f, 1.0f, 'O', 0},
    {L"p", L"P", L"", L"P", 10.5f, 2.0f, 1.0f, 1.0f, 'P', 0},
    {L"[", L"[", L"{", L"[", 11.5f, 2.0f, 1.0f, 1.0f, VK_OEM_4, 0},
    {L"]", L"]", L"}", L"]", 12.5f, 2.0f, 1.0f, 1.0f, VK_OEM_6, 0},
    {L"\\", L"\\", L"|", L"\\", 13.5f, 2.0f, 1.5f, 1.0f, VK_OEM_5, 0},
    {L"delete", L"DEL", L"", L"DELETE", 15.5f, 2.0f, 1.0f, 1.0f, VK_DELETE, 0},
    {L"end", L"END", L"", L"END", 16.5f, 2.0f, 1.0f, 1.0f, VK_END, 0},
    {L"pgdn", L"PGDN", L"", L"PAGE DOWN", 17.5f, 2.0f, 1.0f, 1.0f, VK_NEXT, 0},
    {L"num7", L"7", L"", L"NUM 7", 19.5f, 2.0f, 1.0f, 1.0f, VK_NUMPAD7, 0},
    {L"num8", L"8", L"", L"NUM 8", 20.5f, 2.0f, 1.0f, 1.0f, VK_NUMPAD8, 0},
    {L"num9", L"9", L"", L"NUM 9", 21.5f, 2.0f, 1.0f, 1.0f, VK_NUMPAD9, 0},
    {L"num+", L"+", L"", L"NUM +", 22.5f, 2.0f, 1.0f, 2.0f, VK_ADD, 0},

    // ---- Caps 行 y=3 ------------------------------------------------------
    {L"capslock", L"CAPS", L"", L"CAPS LOCK", 0.0f, 3.0f, 1.75f, 1.0f, VK_CAPITAL, 0},
    {L"a", L"A", L"", L"A", 1.75f, 3.0f, 1.0f, 1.0f, 'A', 0},
    {L"s", L"S", L"", L"S", 2.75f, 3.0f, 1.0f, 1.0f, 'S', 0},
    {L"d", L"D", L"", L"D", 3.75f, 3.0f, 1.0f, 1.0f, 'D', 0},
    {L"f", L"F", L"", L"F", 4.75f, 3.0f, 1.0f, 1.0f, 'F', 0},
    {L"g", L"G", L"", L"G", 5.75f, 3.0f, 1.0f, 1.0f, 'G', 0},
    {L"h", L"H", L"", L"H", 6.75f, 3.0f, 1.0f, 1.0f, 'H', 0},
    {L"j", L"J", L"", L"J", 7.75f, 3.0f, 1.0f, 1.0f, 'J', 0},
    {L"k", L"K", L"", L"K", 8.75f, 3.0f, 1.0f, 1.0f, 'K', 0},
    {L"l", L"L", L"", L"L", 9.75f, 3.0f, 1.0f, 1.0f, 'L', 0},
    {L";", L";", L":", L";", 10.75f, 3.0f, 1.0f, 1.0f, VK_OEM_1, 0},
    {L"'", L"'", L"\"", L"'", 11.75f, 3.0f, 1.0f, 1.0f, VK_OEM_7, 0},
    {L"enter", L"ENTER", L"", L"ENTER", 12.75f, 3.0f, 2.25f, 1.0f, VK_RETURN, 0},
    {L"num4", L"4", L"", L"NUM 4", 19.5f, 3.0f, 1.0f, 1.0f, VK_NUMPAD4, 0},
    {L"num5", L"5", L"", L"NUM 5", 20.5f, 3.0f, 1.0f, 1.0f, VK_NUMPAD5, 0},
    {L"num6", L"6", L"", L"NUM 6", 21.5f, 3.0f, 1.0f, 1.0f, VK_NUMPAD6, 0},

    // ---- Shift 行 y=4 -----------------------------------------------------
    {L"lshift", L"SHIFT", L"", L"L SHIFT", 0.0f, 4.0f, 2.25f, 1.0f, VK_LSHIFT, 0x2A},
    {L"z", L"Z", L"", L"Z", 2.25f, 4.0f, 1.0f, 1.0f, 'Z', 0},
    {L"x", L"X", L"", L"X", 3.25f, 4.0f, 1.0f, 1.0f, 'X', 0},
    {L"c", L"C", L"", L"C", 4.25f, 4.0f, 1.0f, 1.0f, 'C', 0},
    {L"v", L"V", L"", L"V", 5.25f, 4.0f, 1.0f, 1.0f, 'V', 0},
    {L"b", L"B", L"", L"B", 6.25f, 4.0f, 1.0f, 1.0f, 'B', 0},
    {L"n", L"N", L"", L"N", 7.25f, 4.0f, 1.0f, 1.0f, 'N', 0},
    {L"m", L"M", L"", L"M", 8.25f, 4.0f, 1.0f, 1.0f, 'M', 0},
    {L",", L",", L"<", L",", 9.25f, 4.0f, 1.0f, 1.0f, VK_OEM_COMMA, 0},
    {L".", L".", L">", L".", 10.25f, 4.0f, 1.0f, 1.0f, VK_OEM_PERIOD, 0},
    {L"/", L"/", L"?", L"/", 11.25f, 4.0f, 1.0f, 1.0f, VK_OEM_2, 0},
    {L"rshift", L"SHIFT", L"", L"R SHIFT", 12.25f, 4.0f, 2.75f, 1.0f, VK_RSHIFT, 0x36},
    {L"up", L"↑", L"", L"UP", 16.5f, 4.0f, 1.0f, 1.0f, VK_UP, 0},
    {L"num1", L"1", L"", L"NUM 1", 19.5f, 4.0f, 1.0f, 1.0f, VK_NUMPAD1, 0},
    {L"num2", L"2", L"", L"NUM 2", 20.5f, 4.0f, 1.0f, 1.0f, VK_NUMPAD2, 0},
    {L"num3", L"3", L"", L"NUM 3", 21.5f, 4.0f, 1.0f, 1.0f, VK_NUMPAD3, 0},
    {L"numenter", L"ENTER", L"", L"NUM ENTER", 22.5f, 4.0f, 1.0f, 2.0f, VK_RETURN, 0},

    // ---- 底行 y=5 ---------------------------------------------------------
    {L"lctrl", L"CTRL", L"", L"L CTRL", 0.0f, 5.0f, 1.25f, 1.0f, VK_LCONTROL, 0},
    {L"lwin", L"WIN", L"", L"L WIN", 1.25f, 5.0f, 1.25f, 1.0f, VK_LWIN, 0},
    {L"lalt", L"ALT", L"", L"L ALT", 2.5f, 5.0f, 1.25f, 1.0f, VK_LMENU, 0},
    {L"space", L"", L"", L"SPACE", 3.75f, 5.0f, 6.25f, 1.0f, VK_SPACE, 0},
    {L"ralt", L"ALT", L"", L"R ALT", 10.0f, 5.0f, 1.25f, 1.0f, VK_RMENU, 0},
    {L"rwin", L"WIN", L"", L"R WIN", 11.25f, 5.0f, 1.25f, 1.0f, VK_RWIN, 0},
    {L"rmenu", L"MENU", L"", L"MENU", 12.5f, 5.0f, 1.25f, 1.0f, VK_APPS, 0},
    {L"rctrl", L"CTRL", L"", L"R CTRL", 13.75f, 5.0f, 1.25f, 1.0f, VK_RCONTROL, 0},
    {L"left", L"←", L"", L"LEFT", 15.5f, 5.0f, 1.0f, 1.0f, VK_LEFT, 0},
    {L"down", L"↓", L"", L"DOWN", 16.5f, 5.0f, 1.0f, 1.0f, VK_DOWN, 0},
    {L"right", L"→", L"", L"RIGHT", 17.5f, 5.0f, 1.0f, 1.0f, VK_RIGHT, 0},
    {L"num0", L"0", L"", L"NUM 0", 19.5f, 5.0f, 2.0f, 1.0f, VK_NUMPAD0, 0},
    {L"num.", L".", L"", L"NUM .", 21.5f, 5.0f, 1.0f, 1.0f, VK_DECIMAL, 0},
};

bool IsNavVirtualKey(uint16_t vk) {
    switch (vk) {
        case VK_INSERT:
        case VK_HOME:
        case VK_PRIOR:
        case VK_DELETE:
        case VK_END:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_CLEAR:
            return true;
        default:
            return false;
    }
}

const wchar_t* NumpadCodeForScan(uint16_t makeCode) {
    // NumLock 关闭时数字小键盘会以导航 VK + 无 E0 上报；用扫描码还原物理键位。
    switch (makeCode) {
        case 0x47: return L"num7";
        case 0x48: return L"num8";
        case 0x49: return L"num9";
        case 0x4B: return L"num4";
        case 0x4C: return L"num5";
        case 0x4D: return L"num6";
        case 0x4F: return L"num1";
        case 0x50: return L"num2";
        case 0x51: return L"num3";
        case 0x52: return L"num0";
        case 0x53: return L"num.";
        default: return nullptr;
    }
}

}  // namespace

const wchar_t* EventName(EventType type) {
    switch (type) {
        case EventType::Down: return L"KeyDown";
        case EventType::Up: return L"KeyUp";
        case EventType::Blur: return L"Blur";
        default: return L"?";
    }
}

KeyboardTestEngine::KeyboardTestEngine() {
    for (int i = 0; i < kKeyCount; ++i) defs_[(size_t)i] = kKeys[i];
    events_.reserve(kLogCapacity + 8);
}

void KeyboardTestEngine::Reset() {
    // 清空时仍按住的键：记录并忽略其系统自动重复，直到收到物理 KeyUp。
    // 否则「清空」会在一个重复周期内被自动重复重新点亮。
    for (int i = 0; i < kKeyCount; ++i) {
        if (runtime_[(size_t)i].down) suppressRepeat_[(size_t)i] = true;
    }
    for (auto& r : runtime_) r = KeyRuntime{};
    events_.clear();
    downTimes_.clear();
    maxSimultaneous_ = 0;
    chatterCount_ = 0;
    lastKeyIndex_ = -1;
}

void KeyboardTestEngine::ReleaseAll() {
    // 离开键盘页/切回鼠标：只释放按住态。此时物理 KeyUp 可能落在鼠标模式里
    // 被丢弃，因此不能保留 suppressRepeat_，否则下一次真实按下会被吞掉。
    for (auto& r : runtime_) r.down = false;
    for (auto& s : suppressRepeat_) s = false;
}

int KeyboardTestEngine::ResolveKey(uint16_t vk, uint16_t makeCode, bool extended) const {
    // 数字小键盘在 NumLock 关闭时上报导航 VK 且无 E0；必须先按扫描码还原，
    // 否则按小键盘 8 会点亮主方向键。
    if (!extended && IsNavVirtualKey(vk)) {
        if (const wchar_t* code = NumpadCodeForScan(makeCode)) {
            for (int i = 0; i < kKeyCount; ++i) {
                if (wcscmp(defs_[(size_t)i].code, code) == 0) return i;
            }
        }
    }
    if (vk == VK_RETURN) {
        const wchar_t* code = extended ? L"numenter" : L"enter";
        for (int i = 0; i < kKeyCount; ++i) {
            if (wcscmp(defs_[(size_t)i].code, code) == 0) return i;
        }
        return -1;
    }
    if (vk == VK_CANCEL) {
        // Ctrl+Pause/Break 的旧式虚拟键是 VK_CANCEL；布局里只有 PAUSE 一个键位。
        for (int i = 0; i < kKeyCount; ++i) {
            if (wcscmp(defs_[(size_t)i].code, L"pause") == 0) return i;
        }
        return -1;
    }
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        const bool right = (makeCode == 0x36 || vk == VK_RSHIFT);
        const wchar_t* code = right ? L"rshift" : L"lshift";
        for (int i = 0; i < kKeyCount; ++i) {
            if (wcscmp(defs_[(size_t)i].code, code) == 0) return i;
        }
    }
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        const bool right = (extended || vk == VK_RCONTROL);
        const wchar_t* code = right ? L"rctrl" : L"lctrl";
        for (int i = 0; i < kKeyCount; ++i) {
            if (wcscmp(defs_[(size_t)i].code, code) == 0) return i;
        }
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        const bool right = (extended || vk == VK_RMENU);
        const wchar_t* code = right ? L"ralt" : L"lalt";
        for (int i = 0; i < kKeyCount; ++i) {
            if (wcscmp(defs_[(size_t)i].code, code) == 0) return i;
        }
    }

    int fallback = -1;
    for (int i = 0; i < kKeyCount; ++i) {
        const KeyDef& d = defs_[(size_t)i];
        if (d.vk != vk) continue;
        if (d.makeCode != 0) {
            if (d.makeCode == makeCode) return i;
            continue;
        }
        if (fallback < 0) fallback = i;
    }
    return fallback;
}

const KeyDef& KeyboardTestEngine::Def(int index) const {
    static const KeyDef kEmpty{};
    if (index < 0 || index >= kKeyCount) return kEmpty;
    return defs_[(size_t)index];
}

const KeyRuntime& KeyboardTestEngine::Runtime(int index) const {
    static const KeyRuntime kEmpty{};
    if (index < 0 || index >= kKeyCount) return kEmpty;
    return runtime_[(size_t)index];
}

KeyState KeyboardTestEngine::State(int index) const {
    if (index < 0 || index >= kKeyCount) return KeyState::Untested;
    const KeyRuntime& r = runtime_[(size_t)index];
    if (r.down) return KeyState::Down;
    return r.tested ? KeyState::Up : KeyState::Untested;
}

int KeyboardTestEngine::KeyDown(uint16_t vk, uint16_t makeCode, bool extended,
                                uint64_t nowMs) {
    const int index = ResolveKey(vk, makeCode, extended);
    if (index < 0) return -1;
    KeyRuntime& r = runtime_[(size_t)index];
    if (suppressRepeat_[(size_t)index]) return -1;  // 清空后仍按住的键，忽略自动重复
    if (r.down) return -1;  // 自动重复：保持按下态，不重复记日志/APM
    // 用 hasLastDown/hasPrevDown 判断，不能用时间戳 == 0 作哨兵：
    // 第一次按下恰好发生在 nowMs == 0 时会被误判为没有历史。
    // 失焦会同时清掉两个标记，连击/间隔不能跨失焦计算。
    const bool hadLastDown = r.hasLastDown;
    r.down = true;
    r.tested = true;
    r.prevDownMs = r.lastDownMs;
    r.lastDownMs = nowMs;
    r.hasLastDown = true;
    r.hasPrevDown = hadLastDown;
    if (r.hasPrevDown && nowMs >= r.prevDownMs &&
        nowMs - r.prevDownMs < (uint64_t)kChatterThresholdMs) {
        ++chatterCount_;
    }
    PushDownTime(nowMs);
    const int down = DownCount();
    if (down > maxSimultaneous_) maxSimultaneous_ = down;
    lastKeyIndex_ = index;
    PushEvent(nowMs, index, EventType::Down);
    return index;
}

int KeyboardTestEngine::KeyUp(uint16_t vk, uint16_t makeCode, bool extended,
                              uint64_t nowMs) {
    const int index = ResolveKey(vk, makeCode, extended);
    if (index < 0) return -1;
    if (suppressRepeat_[(size_t)index]) {
        // 清空时仍按住的键，物理 KeyUp 只解除抑制，不补记 KeyUp。
        suppressRepeat_[(size_t)index] = false;
        return -1;
    }
    KeyRuntime& r = runtime_[(size_t)index];
    if (!r.down) return -1;
    r.down = false;
    r.lastUpMs = nowMs;
    PushEvent(nowMs, index, EventType::Up);
    return index;
}

void KeyboardTestEngine::Blur(uint64_t nowMs) {
    for (auto& r : runtime_) {
        r.down = false;
        // 失焦可能持续任意时长；清掉同键间隔，连击/最近间隔不能跨失焦计算。
        r.hasLastDown = false;
        r.hasPrevDown = false;
        r.prevDownMs = 0;
        r.lastDownMs = 0;
    }
    // 失焦期间的 KeyUp 可能丢失；抑制不能留到重新聚焦后吞掉真实按键。
    for (auto& s : suppressRepeat_) s = false;
    PushEvent(nowMs, -1, EventType::Blur);
}

void KeyboardTestEngine::PushEvent(uint64_t nowMs, int keyIndex, EventType type) {
    if (events_.size() >= (size_t)kLogCapacity) {
        events_.erase(events_.begin());
    }
    events_.push_back(Event{nowMs, keyIndex, type});
}

int KeyboardTestEngine::DownCount() const {
    int n = 0;
    for (const auto& r : runtime_) {
        if (r.down) ++n;
    }
    return n;
}

int KeyboardTestEngine::TestedCount() const {
    int n = 0;
    for (const auto& r : runtime_) {
        if (r.tested) ++n;
    }
    return n;
}

int KeyboardTestEngine::LastIntervalMs(int index) const {
    if (index < 0 || index >= kKeyCount) return -1;
    const KeyRuntime& r = runtime_[(size_t)index];
    if (!r.hasPrevDown || r.lastDownMs < r.prevDownMs) return -1;
    return (int)(r.lastDownMs - r.prevDownMs);
}

void KeyboardTestEngine::PushDownTime(uint64_t nowMs) {
    // events_ 只保留 64 条日志，不能拿来算 5s/30s 指标；这里独立保留最近 30s
    // 的有效 KeyDown 时间戳。高频率输入不会因 KeyUp/Blur 挤占而少算。
    downTimes_.push_back(nowMs);
    PruneDownTimes(nowMs);
}

void KeyboardTestEngine::PruneDownTimes(uint64_t nowMs) {
    // 时间戳升序；只在 nowMs 向前推进时裁剪。调用方可能传入回退的时间
    // （测试/注入），此时不裁剪，避免误删仍在窗口内的样本。
    while (!downTimes_.empty()) {
        const uint64_t front = downTimes_.front();
        if (front > nowMs || nowMs - front < kMetricWindowMs) break;
        downTimes_.pop_front();
    }
}

int KeyboardTestEngine::Apm(uint64_t nowMs) const {
    const uint64_t window = 5000;
    int n = 0;
    // 从新到旧扫描；升序存储，一旦遇到超出 5s 的样本即可停止。
    for (auto it = downTimes_.rbegin(); it != downTimes_.rend(); ++it) {
        if (*it > nowMs) continue;  // 未来时间戳不计入
        if (nowMs - *it >= window) break;
        ++n;
    }
    return n * 12;
}

std::vector<int> KeyboardTestEngine::ApmHistory(uint64_t nowMs, int seconds) const {
    if (seconds <= 0) return {};
    std::vector<int> out((size_t)seconds, 0);
    for (const auto& down : downTimes_) {
        if (down > nowMs) continue;
        const uint64_t age = nowMs - down;
        if (age >= (uint64_t)seconds * 1000) continue;
        const int bucket = (int)(age / 1000);
        const int pos = seconds - 1 - bucket;
        if (pos >= 0 && pos < seconds) ++out[(size_t)pos];
    }
    return out;
}

bool KeyboardTestEngine::IsSystemReserved(int index) const {
    if (index < 0 || index >= kKeyCount) return false;
    const wchar_t* code = defs_[(size_t)index].code;
    return wcscmp(code, L"lwin") == 0 || wcscmp(code, L"rwin") == 0 ||
           wcscmp(code, L"rmenu") == 0;
}

}  // namespace keyboardtest
