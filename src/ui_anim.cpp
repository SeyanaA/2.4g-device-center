#include "ui_anim.h"

#include <algorithm>
#include <cmath>

namespace {

// 一维三次贝塞尔：x 控制点固定，解出参数 t 再取 y。
// 用 Newton 迭代 + 二分兜底，避免复杂查表。
float BezierY(float x1, float y1, float x2, float y2, float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    auto sampleX = [&](float t) {
        float u = 1.0f - t;
        return 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t;
    };
    auto sampleY = [&](float t) {
        float u = 1.0f - t;
        return 3.0f * u * u * t * y1 + 3.0f * u * t * t * y2 + t * t * t;
    };

    // 先用 Newton 快速逼近。
    float t = x;
    for (int i = 0; i < 6; ++i) {
        float dx = sampleX(t) - x;
        if (std::fabs(dx) < 1e-5f) return sampleY(t);
        float u = 1.0f - t;
        float d = 3.0f * u * u * x1 + 6.0f * u * t * (x2 - x1) + 3.0f * t * t * (1.0f - x2);
        if (std::fabs(d) < 1e-6f) break;
        t -= dx / d;
        t = std::max(0.0f, std::min(1.0f, t));
    }

    // 二分兜底，保证单调。
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 20; ++i) {
        t = (lo + hi) * 0.5f;
        if (sampleX(t) < x) {
            lo = t;
        } else {
            hi = t;
        }
    }
    return sampleY((lo + hi) * 0.5f);
}

// 欠阻尼弹簧阶跃响应（归一化时间 t=0..1）。
// wd 固定为 9：过冲峰值落在 t≈0.35，t=1 时残差 <0.1%，所以 Animator 直接
// 用终值收尾不会有可见跳变。ζ 决定过冲量：0.62 → ≈8.3%，0.78 → ≈2.1%。
float SpringY(float t, float zeta) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float wd = 9.0f;
    const float wn = wd / std::sqrt(1.0f - zeta * zeta);
    const float envelope = std::exp(-zeta * wn * t);
    return 1.0f - envelope *
                      (std::cos(wd * t) + (zeta * wn / wd) * std::sin(wd * t));
}

}  // namespace

double NowMs() {
    // QPC 频率是常量，只查询一次；C++11 起局部静态初始化线程安全且不分配堆内存。
    static const double ticksPerMs = [] {
        LARGE_INTEGER freq{};
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return 0.0;
        return static_cast<double>(freq.QuadPart) / 1000.0;
    }();

    if (ticksPerMs > 0.0) {
        LARGE_INTEGER counter{};
        if (QueryPerformanceCounter(&counter)) {
            return static_cast<double>(counter.QuadPart) / ticksPerMs;
        }
    }
    // QPC 不可用时退回系统毫秒时钟（约 15.6ms 粒度，仅作保底）。
    return static_cast<double>(GetTickCount64());
}

float Ease(Easing easing, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    switch (easing) {
        case Easing::Linear:
            return t;
        case Easing::Standard:
            return BezierY(0.2f, 0.0f, 0.0f, 1.0f, t);
        case Easing::StandardDecelerate:
            return BezierY(0.0f, 0.0f, 0.0f, 1.0f, t);
        case Easing::StandardAccelerate:
            return BezierY(0.3f, 0.0f, 1.0f, 1.0f, t);
        case Easing::Emphasized:
            return BezierY(0.2f, 0.0f, 0.0f, 1.0f, t);
        case Easing::EmphasizedDecelerate:
        case Easing::Decelerate:
            return BezierY(0.05f, 0.7f, 0.1f, 1.0f, t);
        case Easing::EmphasizedAccelerate:
        case Easing::Accelerate:
            return BezierY(0.3f, 0.0f, 0.8f, 0.15f, t);
        case Easing::Spring:
            return SpringY(t, 0.62f);
        case Easing::SpringSoft:
            return SpringY(t, 0.78f);
    }
    return t;
}

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

COLORREF LerpColor(COLORREF a, COLORREF b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    int r = (int)std::lround(Lerp((float)GetRValue(a), (float)GetRValue(b), t));
    int g = (int)std::lround(Lerp((float)GetGValue(a), (float)GetGValue(b), t));
    int bl = (int)std::lround(Lerp((float)GetBValue(a), (float)GetBValue(b), t));
    return RGB(r, g, bl);
}

RECT LerpRect(const RECT& a, const RECT& b, float t) {
    RECT r{};
    r.left = (LONG)std::lround(Lerp((float)a.left, (float)b.left, t));
    r.top = (LONG)std::lround(Lerp((float)a.top, (float)b.top, t));
    r.right = (LONG)std::lround(Lerp((float)a.right, (float)b.right, t));
    r.bottom = (LONG)std::lround(Lerp((float)a.bottom, (float)b.bottom, t));
    return r;
}

bool SystemAnimationsEnabled() {
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return true;
    return enabled != FALSE;
}

Animator::Animator(bool animationsEnabled) : animationsEnabled_(animationsEnabled) {}

Animator::AnimId Animator::Add(float from, float to, int durationMs, Easing easing,
                               std::function<void(float)> apply, int delayMs) {
    return AddAt(NowMs(), from, to, durationMs, easing, std::move(apply), delayMs);
}

Animator::AnimId Animator::AddAt(double nowMs, float from, float to, int durationMs,
                                 Easing easing, std::function<void(float)> apply,
                                 int delayMs) {
    AnimId id = nextId_++;
    if (!apply) return id;

    if (!animationsEnabled_ || durationMs <= 0) {
        apply(to);
        return id;
    }

    Track t;
    t.id = id;
    t.from = from;
    t.to = to;
    t.durationMs = durationMs;
    t.delayMs = std::max(0, delayMs);
    t.easing = easing;
    t.apply = std::move(apply);
    t.startMs = nowMs;
    tracks_.push_back(std::move(t));
    return id;
}

bool Animator::Tick(double nowMs) {
    if (tracks_.empty()) return false;

    struct Pending {
        AnimId id;
        float value;
    };
    // 先算值，再统一回调；回调里 Add/Cancel/Clear 可能改写 tracks_，所以按 id 查找。
    std::vector<Pending> pending;
    std::vector<AnimId> finished;
    pending.reserve(tracks_.size());
    for (const Track& t : tracks_) {
        // 用 double 计算亚毫秒 elapsed；延迟比较与进度都不再截断。
        const double elapsed = nowMs > t.startMs ? nowMs - t.startMs : 0.0;
        if (elapsed < static_cast<double>(t.delayMs)) continue;
        const double active = elapsed - static_cast<double>(t.delayMs);
        const double progress =
            t.durationMs > 0 ? active / static_cast<double>(t.durationMs) : 1.0;
        if (progress >= 1.0) {
            pending.push_back({t.id, t.to});
            finished.push_back(t.id);
        } else {
            pending.push_back(
                {t.id, Lerp(t.from, t.to, Ease(t.easing, static_cast<float>(progress)))});
        }
    }

    for (const Pending& p : pending) {
        for (Track& t : tracks_) {
            if (t.id == p.id) {
                t.apply(p.value);
                break;
            }
        }
    }

    if (!finished.empty()) {
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                     [&finished](const Track& t) {
                                         return std::find(finished.begin(), finished.end(),
                                                          t.id) != finished.end();
                                     }),
                      tracks_.end());
    }
    return !tracks_.empty();
}

void Animator::Cancel(AnimId id) {
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [id](const Track& t) { return t.id == id; }),
                  tracks_.end());
}

void Animator::Clear() { tracks_.clear(); }
