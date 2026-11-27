#include "mouse_test.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <iostream>

namespace {

constexpr int64_t kFreq = 1000000;  // ticks == 微秒，便于构造精确间隔

RAWMOUSE MoveReport(int dx, int dy) {
    RAWMOUSE m{};
    m.usFlags = MOUSE_MOVE_RELATIVE;
    m.lLastX = dx;
    m.lLastY = dy;
    return m;
}

RAWMOUSE ButtonReport(USHORT flags, int wheelDelta = 0) {
    RAWMOUSE m{};
    m.usButtonFlags = flags;
    m.usButtonData = static_cast<USHORT>(static_cast<int16_t>(wheelDelta));
    return m;
}

void Move(MouseTestEngine& e, int64_t us, int dx = 1, int dy = 0) {
    e.AddMouse(MoveReport(dx, dy), us, kFreq);
}

void Press(MouseTestEngine& e, int64_t us, USHORT flags = RI_MOUSE_BUTTON_1_DOWN) {
    e.AddMouse(ButtonReport(flags), us, kFreq);
}

bool Near(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

}  // namespace

int main() {
    // 1. 500 µs 的移动间隔 -> 2000 Hz。
    {
        MouseTestEngine e;
        Move(e, 0);
        Move(e, 500);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.pollingSamples == 1);
        assert(Near(s.medianIntervalUs, 500.0));
        assert(Near(s.pollingHz, 2000.0));
        assert(Near(s.within10Pct, 100.0));
        assert(wcscmp(s.stability, L"稳定") == 0);
    }

    // 2. 抖动样本取中位数：400/600/500 -> 500 µs -> 2000 Hz。
    {
        MouseTestEngine e;
        Move(e, 0);
        Move(e, 400);
        Move(e, 1000);
        Move(e, 1500);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.pollingSamples == 3);
        assert(Near(s.medianIntervalUs, 500.0));
        assert(Near(s.pollingHz, 2000.0));
    }

    // 3. ±10% 占比与五段分桶：300/420/500/580/700，中位 500。
    {
        MouseTestEngine e;
        Move(e, 0);
        Move(e, 300);
        Move(e, 720);
        Move(e, 1220);
        Move(e, 1800);
        Move(e, 2500);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.pollingSamples == 5);
        assert(Near(s.within10Pct, 20.0));  // 只有 500 µs 落在 ±10%
        for (int i = 0; i < 5; ++i) assert(Near(s.bucketPct[i], 20.0));
        double sum = 0.0;
        for (int i = 0; i < 5; ++i) sum += s.bucketPct[i];
        assert(Near(sum, 100.0));
        assert(wcscmp(s.stability, L"不稳定") == 0);
    }

    // 4. 60 ms 与 30 µs 的间隔都不进回报率样本，但基准点继续前移。
    {
        MouseTestEngine e;
        Move(e, 0);
        Move(e, 1000);
        Move(e, 61000);
        Move(e, 61030);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.pollingSamples == 1);
        assert(Near(s.medianIntervalUs, 1000.0));
        assert(Near(s.pollingHz, 1000.0));
        assert(Near(s.maxGapMs, 60.0));  // 60 ms 的停顿仍计入最长中断
    }

    // 5. 五个按键的 DOWN 次数：左/中/右/后退/前进。
    {
        MouseTestEngine e;
        Press(e, 0, RI_MOUSE_BUTTON_1_DOWN);
        Press(e, 100000, RI_MOUSE_BUTTON_1_DOWN);
        Press(e, 200000, RI_MOUSE_BUTTON_1_DOWN);
        Press(e, 300000, RI_MOUSE_BUTTON_2_DOWN);
        Press(e, 400000, RI_MOUSE_BUTTON_2_DOWN);
        Press(e, 500000, RI_MOUSE_BUTTON_3_DOWN);
        Press(e, 600000, RI_MOUSE_BUTTON_4_DOWN);
        Press(e, 700000, RI_MOUSE_BUTTON_5_DOWN);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.buttonDown[0] == 3);
        assert(s.buttonDown[1] == 2);
        assert(s.buttonDown[2] == 1);
        assert(s.buttonDown[3] == 1);
        assert(s.buttonDown[4] == 1);
    }

    // 6. 5 s 窗口：5 次左键（0..4 s）-> 1.0 CPS。
    {
        MouseTestEngine e;
        for (int i = 0; i < 5; ++i) Press(e, static_cast<int64_t>(i) * 1000000);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.clickWindowSec == 5);
        assert(Near(s.cps, 1.0));
        assert(Near(s.bestCps, 1.0));
    }

    // 7. 6 次点击集中在 0.5 s 内：5 s 最佳 1.2，10 s 最佳 0.6。
    {
        MouseTestEngine e;
        for (int i = 0; i < 6; ++i) Press(e, static_cast<int64_t>(i) * 100000);
        const MouseTestSnapshot s5 = e.Snapshot();
        assert(Near(s5.cps, 6.0 / 5.0));
        assert(Near(s5.bestCps, 6.0 / 5.0));

        e.SetClickWindow(10);
        const MouseTestSnapshot s10 = e.Snapshot();
        assert(s10.clickWindowSec == 10);
        assert(Near(s10.cps, 6.0 / 10.0));
        assert(Near(s10.bestCps, 6.0 / 10.0));

        e.SetClickWindow(5);
        assert(Near(e.Snapshot().bestCps, 6.0 / 5.0));
    }

    // 8. 正常双击：400 ms 配对，计一次且不算故障。
    {
        MouseTestEngine e;
        Press(e, 0);
        Press(e, 400000);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.doubleClicks == 1);
        assert(s.badDoubleClicks == 0);
        assert(s.lastDoubleClickMs == 400);
        assert(s.minBadDoubleClickMs == -1);
    }

    // 9. 故障双击：50 ms < 80 ms，同时计入双击与故障，并记录最小值。
    {
        MouseTestEngine e;
        Press(e, 0);
        Press(e, 50000);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.doubleClicks == 1);
        assert(s.badDoubleClicks == 1);
        assert(s.lastDoubleClickMs == 50);
        assert(s.minBadDoubleClickMs == 50);
    }

    // 10. 滚轮：+120 两格、-240 两格，1 s 窗口内 4 格/秒。
    {
        MouseTestEngine e;
        e.AddMouse(ButtonReport(RI_MOUSE_WHEEL, 120), 0, kFreq);
        e.AddMouse(ButtonReport(RI_MOUSE_WHEEL, 120), 10000, kFreq);
        e.AddMouse(ButtonReport(RI_MOUSE_WHEEL, -240), 20000, kFreq);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.wheelUp == 2);
        assert(s.wheelDown == 2);
        assert(Near(s.wheelPerSec, 4.0));
    }

    // 11. 轨迹：原点 + 累计位移；路径长度累加曼哈顿距离；单次 > 200 计跳点。
    {
        MouseTestEngine e;
        Move(e, 0, 10, 0);
        Move(e, 1000, 5, 5);
        Move(e, 2000, 150, 100);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.reports == 3);
        assert(s.jumps == 1);
        assert(s.pathLength == 10 + 10 + 250);  // 每段 |dx| + |dy| 累加
        const std::vector<POINT>& t = e.Trajectory();
        assert(t.size() == 4);
        assert(t[0].x == 0 && t[0].y == 0);
        assert(t[1].x == 10 && t[1].y == 0);
        assert(t[2].x == 15 && t[2].y == 5);
        assert(t[3].x == 165 && t[3].y == 105);
    }

    // 12. 最长中断：取相邻移动报文间隔，> 2 s 的离开忽略。
    {
        MouseTestEngine e;
        Move(e, 0);
        Move(e, 100000);
        Move(e, 500000);
        assert(Near(e.Snapshot().maxGapMs, 400.0));
        Move(e, 5500000);  // 5 s 停顿，不计入
        assert(Near(e.Snapshot().maxGapMs, 400.0));
    }

    // 13. 分级重置：计数器 / 轨迹 / 回报率互不越界，最佳 CPS 只由 ResetAll 清空。
    {
        MouseTestEngine e;
        Press(e, 0);
        Press(e, 100000);
        e.AddMouse(ButtonReport(RI_MOUSE_WHEEL, 120), 150000, kFreq);
        Move(e, 200000);
        Move(e, 200500);
        const MouseTestSnapshot before = e.Snapshot();
        assert(before.doubleClicks == 1);
        assert(before.bestCps > 0.0);

        e.ResetCounters();
        const MouseTestSnapshot counters = e.Snapshot();
        assert(counters.buttonDown[0] == 0);
        assert(counters.doubleClicks == 0);
        assert(counters.wheelUp == 0);
        assert(Near(counters.cps, 0.0));
        assert(Near(counters.bestCps, before.bestCps));  // 最佳记录保留
        assert(counters.reports == 2);
        assert(counters.pathLength == 2);  // 路径长度属于轨迹，不被计数器重置清空
        assert(counters.pollingSamples == 1);

        e.ResetTrajectory();
        const MouseTestSnapshot traj = e.Snapshot();
        assert(e.Trajectory().empty());
        assert(traj.reports == 0);
        assert(traj.jumps == 0);
        assert(traj.pathLength == 0);
        assert(Near(traj.maxGapMs, 0.0));
        assert(traj.pollingSamples == 1);  // 回报率样本未被轨迹重置影响

        e.ResetPolling();
        const MouseTestSnapshot poll = e.Snapshot();
        assert(poll.pollingSamples == 0);
        assert(Near(poll.pollingHz, 0.0));
        assert(wcscmp(poll.stability, L"等待数据") == 0);

        e.ResetAll();
        const MouseTestSnapshot all = e.Snapshot();
        assert(Near(all.bestCps, 0.0));
        assert(all.buttonDown[0] == 0);
        assert(e.Trajectory().empty());
    }

    // 14. 设备过滤：nullptr 接受任意句柄，指定句柄只接受匹配项。
    {
        MouseTestEngine e;
        const HANDLE h1 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234));
        const HANDLE h2 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x5678));
        assert(e.AcceptsHandle(h1));  // 默认任意鼠标
        e.SetTargetHandle(h1);
        assert(e.AcceptsHandle(h1));
        assert(!e.AcceptsHandle(h2));
        e.SetTargetHandle(nullptr);
        assert(e.AcceptsHandle(h2));
    }

    // 15. 点击窗口只允许 5 s / 10 s，非法值就近归档。
    {
        MouseTestEngine e;
        e.SetClickWindow(3);
        assert(e.Snapshot().clickWindowSec == 5);
        e.SetClickWindow(10);
        assert(e.Snapshot().clickWindowSec == 10);
        e.SetClickWindow(30);
        assert(e.Snapshot().clickWindowSec == 10);
    }

    // 16. 按键显示名与 RAWMOUSE BUTTON_1..5 同序：0=左 1=右 2=中 3=后退 4=前进。
    {
        assert(wcscmp(MouseButtonName(0), L"左键") == 0);
        assert(wcscmp(MouseButtonName(1), L"右键") == 0);  // BUTTON_2 是右键，不是中键
        assert(wcscmp(MouseButtonName(2), L"中键") == 0);  // BUTTON_3 是中键，不是右键
        assert(wcscmp(MouseButtonName(3), L"后退") == 0);
        assert(wcscmp(MouseButtonName(4), L"前进") == 0);
        assert(wcscmp(MouseButtonName(-1), L"-") == 0);
        assert(wcscmp(MouseButtonName(5), L"-") == 0);

        // 引擎计数下标与显示名必须落在同一个按键上。
        MouseTestEngine e;
        Press(e, 0, RI_MOUSE_BUTTON_2_DOWN);
        Press(e, 10000, RI_MOUSE_BUTTON_3_DOWN);
        const MouseTestSnapshot s = e.Snapshot();
        assert(s.buttonDown[1] == 1);
        assert(wcscmp(MouseButtonName(1), L"右键") == 0);
        assert(s.buttonDown[2] == 1);
        assert(wcscmp(MouseButtonName(2), L"中键") == 0);
    }

    // 17. 目标设备 id 记忆：拔插恢复按 id 优先，空 id / 不存在返回 -1。
    {
        const std::vector<std::wstring> ids{L"mouse-a", L"mouse-b", L"mouse-c"};
        assert(MouseTargetIndexById(ids, L"mouse-b") == 1);
        assert(MouseTargetIndexById(ids, L"mouse-a") == 0);
        assert(MouseTargetIndexById(ids, L"") == -1);
        assert(MouseTargetIndexById(ids, L"missing") == -1);
        assert(MouseTargetIndexById({}, L"mouse-a") == -1);
    }

    std::cout << "mouse_test: 17 groups passed\n";
    return 0;
}
