#include "keyboard_test.h"

#include <cassert>
#include <iostream>

using keyboardtest::EventType;
using keyboardtest::KeyState;
using keyboardtest::KeyboardTestEngine;

namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

int FindCode(const KeyboardTestEngine& e, const wchar_t* code) {
    for (int i = 0; i < e.KeyCount(); ++i) {
        if (wcscmp(e.Def(i).code, code) == 0) return i;
    }
    return -1;
}

void TestLayout() {
    KeyboardTestEngine e;
    CHECK(e.KeyCount() == 104);
    CHECK(FindCode(e, L"esc") == 0);
    CHECK(FindCode(e, L"f12") > 0);
    CHECK(FindCode(e, L"a") > 0);
    CHECK(FindCode(e, L"lshift") > 0);
    CHECK(FindCode(e, L"rshift") > 0);
    CHECK(FindCode(e, L"numenter") > 0);
    CHECK(FindCode(e, L"num.") > 0);
    // 所有键都必须落在 23.5 x 6 单位画布内，且标签非空（空格键允许主标签为空）。
    for (int i = 0; i < e.KeyCount(); ++i) {
        const auto& d = e.Def(i);
        CHECK(d.code != nullptr && d.code[0] != 0);
        CHECK(d.name != nullptr && d.name[0] != 0);
        CHECK(d.x >= 0.0f && d.y >= 0.0f);
        CHECK(d.x + d.w <= 23.5f + 0.001f);
        CHECK(d.y + d.h <= 6.0f + 0.001f);
        CHECK(d.w > 0.0f && d.h > 0.0f);
    }
}

void TestResolveBasicAndModifiers() {
    KeyboardTestEngine e;
    CHECK(e.ResolveKey('A', 0x1E, false) == FindCode(e, L"a"));
    CHECK(e.ResolveKey(VK_F5, 0x3F, false) == FindCode(e, L"f5"));
    CHECK(e.ResolveKey(VK_OEM_3, 0x29, false) == FindCode(e, L"`"));
    CHECK(e.ResolveKey(VK_LEFT, 0x4B, true) == FindCode(e, L"left"));
    CHECK(e.ResolveKey(VK_UP, 0x48, true) == FindCode(e, L"up"));

    CHECK(e.ResolveKey(VK_SHIFT, 0x2A, false) == FindCode(e, L"lshift"));
    CHECK(e.ResolveKey(VK_SHIFT, 0x36, false) == FindCode(e, L"rshift"));
    CHECK(e.ResolveKey(VK_LSHIFT, 0x2A, false) == FindCode(e, L"lshift"));
    CHECK(e.ResolveKey(VK_CONTROL, 0x1D, false) == FindCode(e, L"lctrl"));
    CHECK(e.ResolveKey(VK_CONTROL, 0x1D, true) == FindCode(e, L"rctrl"));
    CHECK(e.ResolveKey(VK_MENU, 0x38, false) == FindCode(e, L"lalt"));
    CHECK(e.ResolveKey(VK_MENU, 0x38, true) == FindCode(e, L"ralt"));
    CHECK(e.ResolveKey(VK_LWIN, 0x5B, true) == FindCode(e, L"lwin"));
    CHECK(e.ResolveKey(VK_RWIN, 0x5C, true) == FindCode(e, L"rwin"));
    CHECK(e.ResolveKey(VK_APPS, 0x5D, true) == FindCode(e, L"rmenu"));

    // 主 Enter / 小键盘 Enter 必须分开。
    CHECK(e.ResolveKey(VK_RETURN, 0x1C, false) == FindCode(e, L"enter"));
    CHECK(e.ResolveKey(VK_RETURN, 0x1C, true) == FindCode(e, L"numenter"));
}

void TestPrintScreen() {
    // PrintScreen 的 Raw Input 报告是 VK_SNAPSHOT + scan 0x37 + E0。
    // 系统级注入会触发 Windows 11 的 Snipping Tool，因此用引擎单测覆盖
    // 应用侧映射/三态/日志/APM；Raw Input 投递路径与其他扩展键相同。
    KeyboardTestEngine e;
    const int p = FindCode(e, L"prtsc");
    CHECK(p > 0);
    CHECK(e.ResolveKey(VK_SNAPSHOT, 0x37, true) == p);
    CHECK(e.KeyDown(VK_SNAPSHOT, 0x37, true, 1000) == p);
    CHECK(e.State(p) == KeyState::Down);
    CHECK(e.KeyUp(VK_SNAPSHOT, 0x37, true, 1010) == p);
    CHECK(e.State(p) == KeyState::Up);
    CHECK(e.Apm(1010) == 12);
}

void TestNumpadWithNumLockOff() {
    KeyboardTestEngine e;
    // NumLock 关闭时数字小键盘上报导航 VK + 无 E0；扫描码决定物理键位。
    CHECK(e.ResolveKey(VK_HOME, 0x47, false) == FindCode(e, L"num7"));
    CHECK(e.ResolveKey(VK_UP, 0x48, false) == FindCode(e, L"num8"));
    CHECK(e.ResolveKey(VK_PRIOR, 0x49, false) == FindCode(e, L"num9"));
    CHECK(e.ResolveKey(VK_LEFT, 0x4B, false) == FindCode(e, L"num4"));
    CHECK(e.ResolveKey(VK_CLEAR, 0x4C, false) == FindCode(e, L"num5"));
    CHECK(e.ResolveKey(VK_RIGHT, 0x4D, false) == FindCode(e, L"num6"));
    CHECK(e.ResolveKey(VK_END, 0x4F, false) == FindCode(e, L"num1"));
    CHECK(e.ResolveKey(VK_DOWN, 0x50, false) == FindCode(e, L"num2"));
    CHECK(e.ResolveKey(VK_NEXT, 0x51, false) == FindCode(e, L"num3"));
    CHECK(e.ResolveKey(VK_INSERT, 0x52, false) == FindCode(e, L"num0"));
    CHECK(e.ResolveKey(VK_DELETE, 0x53, false) == FindCode(e, L"num."));
}

void TestStateAndAutoRepeat() {
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    CHECK(e.State(a) == KeyState::Untested);
    CHECK(e.KeyDown('A', 0x1E, false, 1000) == a);
    CHECK(e.State(a) == KeyState::Down);
    CHECK(e.DownCount() == 1);
    CHECK(e.TestedCount() == 1);
    // 自动重复不重复记日志/APM。
    CHECK(e.KeyDown('A', 0x1E, false, 1020) == -1);
    CHECK(e.Events().size() == 1);
    CHECK(e.KeyUp('A', 0x1E, false, 1040) == a);
    CHECK(e.State(a) == KeyState::Up);
    CHECK(e.DownCount() == 0);
    CHECK(e.Events().size() == 2);
    CHECK(e.Events()[0].type == EventType::Down);
    CHECK(e.Events()[1].type == EventType::Up);
}

void TestApmAndHistory() {
    KeyboardTestEngine e;
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyUp('A', 0x1E, false, 1010);
    e.KeyDown('B', 0x30, false, 1100);
    e.KeyUp('B', 0x30, false, 1110);
    e.KeyDown('C', 0x2E, false, 1200);
    e.KeyUp('C', 0x2E, false, 1210);
    CHECK(e.Apm(1300) == 36);  // 3 次 / 5s * 60
    e.KeyDown('D', 0x20, false, 2999);
    e.KeyUp('D', 0x20, false, 3005);
    CHECK(e.Apm(3000) == 48);  // 4 次 / 5s * 60
    CHECK(e.Apm(8500) == 0);   // 全部超出最近 5 秒
    const auto hist = e.ApmHistory(3000, 5);
    CHECK(hist.size() == 5);
    int sum = 0;
    for (int v : hist) sum += v;
    CHECK(sum == 4);
    CHECK(hist.back() == 1);  // 2999ms 的最近一秒
}

void TestApmBeyondLogCapacity() {
    // 40 次按下 = 80 条日志事件，超过 64 条日志容量；APM 不能因此截断。
    KeyboardTestEngine e;
    for (int i = 0; i < 40; ++i) {
        const uint64_t t = 1000 + (uint64_t)i * 100;
        CHECK(e.KeyDown('A', 0x1E, false, t) >= 0);
        CHECK(e.KeyUp('A', 0x1E, false, t + 10) >= 0);
    }
    CHECK(e.Apm(5000) == 40 * 12);
    const auto hist5 = e.ApmHistory(5000, 5);
    int sum5 = 0;
    for (int v : hist5) sum5 += v;
    CHECK(sum5 == 40);
    const auto hist30 = e.ApmHistory(5000, 30);
    int sum30 = 0;
    for (int v : hist30) sum30 += v;
    CHECK(sum30 == 40);
    CHECK(e.Events().size() == (size_t)KeyboardTestEngine::kLogCapacity);
}

void TestApmKeepsThirtySeconds() {
    // 33 次按下跨越约 26.6 秒，全部应在 30s 直方图内；不能被 64 条日志挤掉。
    KeyboardTestEngine e;
    for (int i = 0; i < 33; ++i) {
        const uint64_t t = 1000 + (uint64_t)i * 800;
        CHECK(e.KeyDown('A', 0x1E, false, t) >= 0);
        CHECK(e.KeyUp('A', 0x1E, false, t + 10) >= 0);
    }
    CHECK(e.Apm(30500) == 2 * 12);  // 30.5s 时只有 25.8s/26.6s 两次在 5s 内
    const auto hist = e.ApmHistory(30500, 30);
    int sum = 0;
    for (int v : hist) sum += v;
    CHECK(sum == 33);
}

void TestChatter() {
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyUp('A', 0x1E, false, 1010);
    e.KeyDown('A', 0x1E, false, 1030);  // 30ms
    e.KeyUp('A', 0x1E, false, 1040);
    CHECK(e.ChatterCount() == 1);
    CHECK(e.LastIntervalMs(a) == 30);
    e.KeyDown('A', 0x1E, false, 1200);  // 170ms
    CHECK(e.ChatterCount() == 1);
}

void TestChatterAtZeroTimestamp() {
    // 第一次按下的时间戳可以是 0；不能用 prevDownMs == 0 当作“没有上一次”。
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    CHECK(e.KeyDown('A', 0x1E, false, 0) == a);
    CHECK(e.KeyUp('A', 0x1E, false, 10) == a);
    CHECK(e.KeyDown('A', 0x1E, false, 30) == a);
    CHECK(e.LastIntervalMs(a) == 30);
    CHECK(e.ChatterCount() == 1);
}

void TestChatterDoesNotBridgeBlur() {
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyUp('A', 0x1E, false, 1010);
    e.Blur(1020);
    // 失焦前 1000ms 按下，失焦后 1040ms 再按：间隔 40ms 但中间失焦，不能算连击。
    CHECK(e.KeyDown('A', 0x1E, false, 1040) == a);
    CHECK(e.ChatterCount() == 0);
    CHECK(e.LastIntervalMs(a) == -1);
}

void TestNKeyRollover() {
    KeyboardTestEngine e;
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyDown('S', 0x1F, false, 1001);
    e.KeyDown('D', 0x20, false, 1002);
    e.KeyDown('F', 0x21, false, 1003);
    e.KeyDown('G', 0x22, false, 1004);
    e.KeyDown('H', 0x23, false, 1005);
    CHECK(e.DownCount() == 6);
    CHECK(e.MaxSimultaneous() == 6);
    e.KeyUp('A', 0x1E, false, 1010);
    e.KeyUp('S', 0x1F, false, 1011);
    CHECK(e.DownCount() == 4);
    CHECK(e.MaxSimultaneous() == 6);
}

void TestBlur() {
    KeyboardTestEngine e;
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyDown('B', 0x30, false, 1010);
    e.Blur(1020);
    CHECK(e.DownCount() == 0);
    CHECK(e.State(FindCode(e, L"a")) == KeyState::Up);
    CHECK(!e.Events().empty());
    CHECK(e.Events().back().type == EventType::Blur);
    // 失焦后仍可正常接收新的 KeyDown。
    CHECK(e.KeyDown('A', 0x1E, false, 1100) >= 0);
    CHECK(e.DownCount() == 1);
}

void TestReset() {
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyUp('A', 0x1E, false, 1010);
    e.KeyDown('B', 0x30, false, 1020);
    e.Reset();
    CHECK(e.DownCount() == 0);
    CHECK(e.TestedCount() == 0);
    CHECK(e.MaxSimultaneous() == 0);
    CHECK(e.ChatterCount() == 0);
    CHECK(e.Events().empty());
    CHECK(e.State(a) == KeyState::Untested);
    CHECK(e.Apm(1050) == 0);
}

void TestResetWhileHeld() {
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    CHECK(e.KeyDown('A', 0x1E, false, 1000) == a);
    e.Reset();
    CHECK(e.State(a) == KeyState::Untested);
    CHECK(e.TestedCount() == 0);
    CHECK(e.Events().empty());
    // 物理键还按着：系统自动重复不能把清空后的键重新点亮。
    CHECK(e.KeyDown('A', 0x1E, false, 1100) == -1);
    CHECK(e.State(a) == KeyState::Untested);
    CHECK(e.TestedCount() == 0);
    CHECK(e.Events().empty());
    CHECK(e.Apm(1100) == 0);
    // 物理松开只解除抑制，不补记 KeyUp。
    CHECK(e.KeyUp('A', 0x1E, false, 1150) == -1);
    CHECK(e.Events().empty());
    // 下一次真实按下恢复正常。
    CHECK(e.KeyDown('A', 0x1E, false, 1200) == a);
    CHECK(e.State(a) == KeyState::Down);
}

void TestReleaseAll() {
    KeyboardTestEngine e;
    const int a = FindCode(e, L"a");
    e.KeyDown('A', 0x1E, false, 1000);
    e.KeyDown('B', 0x30, false, 1010);
    e.ReleaseAll();
    CHECK(e.DownCount() == 0);
    CHECK(e.State(a) == KeyState::Up);  // 已测试过，只是不再按住
    // 离开页面后物理 KeyUp 会落在鼠标模式里被丢弃；下一次按下不能被吞掉。
    CHECK(e.KeyDown('A', 0x1E, false, 1100) == a);
    CHECK(e.State(a) == KeyState::Down);
}

void TestPauseAndCancel() {
    KeyboardTestEngine e;
    const int pause = FindCode(e, L"pause");
    CHECK(pause >= 0);
    CHECK(e.ResolveKey(VK_PAUSE, 0x45, false) == pause);
    // Ctrl+Pause/Break 的旧式虚拟键。
    CHECK(e.ResolveKey(VK_CANCEL, 0x46, true) == pause);
}

void TestReservedKeys() {
    KeyboardTestEngine e;
    CHECK(e.IsSystemReserved(FindCode(e, L"lwin")));
    CHECK(e.IsSystemReserved(FindCode(e, L"rwin")));
    CHECK(e.IsSystemReserved(FindCode(e, L"rmenu")));
    CHECK(!e.IsSystemReserved(FindCode(e, L"a")));
    CHECK(!e.IsSystemReserved(-1));
    CHECK(!e.IsSystemReserved(999));
}

}  // namespace

int main() {
    TestLayout();
    TestResolveBasicAndModifiers();
    TestPrintScreen();
    TestNumpadWithNumLockOff();
    TestStateAndAutoRepeat();
    TestApmAndHistory();
    TestApmBeyondLogCapacity();
    TestApmKeepsThirtySeconds();
    TestChatter();
    TestChatterAtZeroTimestamp();
    TestChatterDoesNotBridgeBlur();
    TestNKeyRollover();
    TestBlur();
    TestReset();
    TestResetWhileHeld();
    TestReleaseAll();
    TestPauseAndCancel();
    TestReservedKeys();
    std::cout << "keyboard_test: 18 tests, " << g_checks << " checks passed\n";
    return 0;
}
