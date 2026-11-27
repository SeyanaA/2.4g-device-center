#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "ui_anim.h"

namespace {

bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

void TestEaseTokens() {
    // Material 3 的贝塞尔令牌：端点固定 0/1，全程夹在 [0,1]。
    for (Easing e : {Easing::Linear, Easing::Standard, Easing::StandardDecelerate,
                     Easing::StandardAccelerate, Easing::Emphasized,
                     Easing::EmphasizedDecelerate, Easing::EmphasizedAccelerate,
                     Easing::Decelerate, Easing::Accelerate}) {
        assert(Near(Ease(e, 0.0f), 0.0f));
        assert(Near(Ease(e, 1.0f), 1.0f));
        float prev = -1.0f;
        for (int i = 0; i <= 200; ++i) {
            float v = Ease(e, (float)i / 200.0f);
            assert(std::isfinite(v));
            assert(v >= prev - 1e-4f);
            assert(v >= -1e-4f && v <= 1.0001f);
            prev = v;
        }
    }
}

void TestSpringCurves() {
    // 弹簧曲线允许过冲：Spring ≈8.3%，SpringSoft ≈2.1%；端点仍必须精确。
    assert(Near(Ease(Easing::Spring, 0.0f), 0.0f));
    assert(Near(Ease(Easing::Spring, 1.0f), 1.0f));
    assert(Near(Ease(Easing::SpringSoft, 0.0f), 0.0f));
    assert(Near(Ease(Easing::SpringSoft, 1.0f), 1.0f));

    float peak = 0.0f;
    float peakSoft = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float t = (float)i / 1000.0f;
        const float v = Ease(Easing::Spring, t);
        const float s = Ease(Easing::SpringSoft, t);
        assert(std::isfinite(v) && std::isfinite(s));
        peak = std::max(peak, v);
        peakSoft = std::max(peakSoft, s);
    }
    assert(peak > 1.05f && peak < 1.12f);
    assert(peakSoft > 1.005f && peakSoft < 1.04f);
    assert(peak > peakSoft);
}

void TestAnimatorSpringOvershoot() {
    Animator anim(true);
    float value = 0.0f;
    float peak = 0.0f;
    anim.AddAt(1000, 0.0f, 100.0f, 300, Easing::Spring, [&](float v) {
        value = v;
        peak = std::max(peak, v);
    });
    for (double t = 1000.0; t <= 1296.0; t += 16.0) {
        anim.Tick(t);
    }
    // 采样步长不一定正好落在 1300；补一次确保轨道收尾。
    anim.Tick(1400);
    assert(peak > 100.0f);           // 轨道确实过冲
    assert(peak < 115.0f);           // 过冲必须柔和，不能失控
    assert(Near(value, 100.0f, 0.001f));  // 结束精确落回终值
    assert(!anim.Active());
}

void TestLerp() {
    assert(Near(Lerp(0.0f, 10.0f, 0.5f), 5.0f));
    COLORREF c = LerpColor(RGB(0, 0, 0), RGB(100, 200, 50), 0.5f);
    assert(GetRValue(c) == 50);
    assert(GetGValue(c) == 100);
    assert(GetBValue(c) == 25);
    RECT a{0, 0, 10, 10};
    RECT b{10, 20, 30, 40};
    RECT r = LerpRect(a, b, 0.5f);
    assert(r.left == 5 && r.top == 10 && r.right == 20 && r.bottom == 25);
}

void TestAnimatorProgress() {
    Animator anim(true);
    float value = -1.0f;
    int calls = 0;
    anim.AddAt(1000, 0.0f, 10.0f, 100, Easing::Linear, [&](float v) {
        value = v;
        ++calls;
    });
    assert(anim.Active());
    assert(anim.Tick(1050));
    assert(Near(value, 5.0f, 0.2f));
    assert(anim.Tick(1100) == false);
    assert(Near(value, 10.0f));
    assert(!anim.Active());
    assert(calls >= 2);
}

void TestAnimatorDelay() {
    Animator anim(true);
    std::vector<float> values;
    anim.AddAt(1000, 0.0f, 10.0f, 100, Easing::Linear, [&](float v) { values.push_back(v); }, 50);
    anim.Tick(1020);
    assert(values.empty());
    anim.Tick(1100);
    assert(!values.empty());
    assert(Near(values.back(), 5.0f, 0.2f));
    anim.Tick(1200);
    assert(Near(values.back(), 10.0f));
}

void TestAnimatorImmediate() {
    Animator anim(false);
    float value = -1.0f;
    anim.AddAt(1000, 3.0f, 9.0f, 200, Easing::Standard, [&](float v) { value = v; });
    assert(Near(value, 9.0f));
    assert(!anim.Active());

    Animator zero(true);
    float z = -1.0f;
    zero.AddAt(1000, 1.0f, 2.0f, 0, Easing::Standard, [&](float v) { z = v; });
    assert(Near(z, 2.0f));
}

void TestAnimatorCancelAndClear() {
    Animator anim(true);
    int calls = 0;
    auto id = anim.AddAt(1000, 0.0f, 10.0f, 200, Easing::Linear, [&](float) { ++calls; });
    anim.Tick(1050);
    int before = calls;
    anim.Cancel(id);
    anim.Tick(1100);
    assert(calls == before);
    assert(!anim.Active());

    anim.AddAt(1000, 0.0f, 1.0f, 200, Easing::Linear, [](float) {});
    anim.AddAt(1000, 0.0f, 1.0f, 200, Easing::Linear, [](float) {});
    assert(anim.Count() == 2);
    anim.Clear();
    assert(anim.Count() == 0);
}

void TestAnimatorOverlappingTracks() {
    Animator anim(true);
    float a = 0.0f, b = 0.0f;
    anim.AddAt(1000, 0.0f, 10.0f, 100, Easing::Linear, [&](float v) { a = v; });
    anim.AddAt(1000, 20.0f, 10.0f, 100, Easing::Linear, [&](float v) { b = v; });
    anim.Tick(1050);
    assert(b > a);  // 上升轨道 a 在 5 附近，下降轨道 b 在 15 附近
    assert(Near(a, 5.0f, 0.3f));
    assert(Near(b, 15.0f, 0.3f));
}

void TestAnimatorCallbackMutation() {
    Animator anim(true);
    bool called = false;
    anim.AddAt(1000, 0.0f, 10.0f, 100, Easing::Linear, [&](float) {
        called = true;
        anim.Clear();  // 回调里修改容器不得导致迭代器失效
    });
    anim.Tick(1050);
    assert(called);
    assert(!anim.Active());
}

void TestNowMsHighResolution() {
    const double t0 = NowMs();
    assert(std::isfinite(t0));
    assert(t0 >= 0.0);

    // 短睡 + 忙等：只验证时钟前进且不倒退，不断言具体步进大小。
    Sleep(1);
    double t1 = NowMs();
    for (int i = 0; i < 5000000 && t1 <= t0; ++i) {
        t1 = NowMs();
    }
    assert(std::isfinite(t1));
    assert(t1 > t0);
    assert(t1 - t0 < 1000.0);  // 前进量必须是合理的毫秒数，而不是单位错误

    // QPC 单调：连续采样不得倒退。
    double prev = t1;
    for (int i = 0; i < 64; ++i) {
        const double now = NowMs();
        assert(std::isfinite(now));
        assert(now >= prev);
        prev = now;
    }
}

void TestAnimatorSubMillisecondTicks() {
    Animator anim(true);
    float value = -1.0f;
    int calls = 0;
    const double t0 = 1000.0;
    anim.AddAt(t0, 0.0f, 100.0f, 100, Easing::Linear, [&](float v) {
        value = v;
        ++calls;
    });

    // 0.5ms 步长远细于 120Hz 的 8ms：证明 Tick 不会把亚毫秒时间截断。
    // 100ms 线性轨道从 0 到 100，t0 后第 i 个 0.5ms 采样应为 0.5*i。
    float prev = -1.0f;
    for (int i = 1; i <= 20; ++i) {
        const double now = t0 + 0.5 * static_cast<double>(i);
        assert(anim.Tick(now));
        const float expected = 0.5f * static_cast<float>(i);
        assert(Near(value, expected, 0.01f));
        assert(value > prev);  // 每个采样都必须严格递增
        prev = value;
    }
    assert(calls == 20);
    assert(anim.Active());

    assert(!anim.Tick(t0 + 100.0));
    assert(Near(value, 100.0f, 0.001f));
    assert(calls == 21);
    assert(!anim.Active());
}

}  // namespace

int main() {
    TestEaseTokens();
    TestSpringCurves();
    TestAnimatorSpringOvershoot();
    TestLerp();
    TestAnimatorProgress();
    TestAnimatorDelay();
    TestAnimatorImmediate();
    TestAnimatorCancelAndClear();
    TestAnimatorOverlappingTracks();
    TestAnimatorCallbackMutation();
    TestNowMsHighResolution();
    TestAnimatorSubMillisecondTicks();
    std::cout << "animator: 12 tests passed\n";
    return 0;
}
