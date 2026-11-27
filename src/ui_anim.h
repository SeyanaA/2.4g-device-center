#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "common.h"

// Google Material 3 动效令牌 + 欠阻尼弹簧。
// Ease 的输入 t 会夹到 0..1；Spring/SpringSoft 的**输出可以大于 1**（过冲），
// 调用方需要自己把几何量夹回有效范围。
enum class Easing {
    Linear,
    Standard,              // cubic-bezier(0.2, 0, 0, 1)
    StandardDecelerate,    // cubic-bezier(0, 0, 0, 1)
    StandardAccelerate,    // cubic-bezier(0.3, 0, 1, 1)
    Emphasized,            // cubic-bezier(0.2, 0, 0, 1)
    EmphasizedDecelerate,  // cubic-bezier(0.05, 0.7, 0.1, 1)
    EmphasizedAccelerate,  // cubic-bezier(0.3, 0, 0.8, 0.15)
    Decelerate,            // 旧名，等价 EmphasizedDecelerate
    Accelerate,            // 旧名，等价 EmphasizedAccelerate
    Spring,                // 果冻：ζ=0.62，约 8.3% 过冲，峰值在 t≈0.35
    SpringSoft,            // 柔和：ζ=0.78，约 2.1% 过冲，用于数值/面积较大的动效
};

float Ease(Easing easing, float t);

// 高精度时间戳（毫秒，double）。GetTickCount64 的分辨率约 15.6 ms，
// 120 Hz 动画需要亚毫秒时间戳，否则每一帧的进度都会整块跳动。
double NowMs();

float Lerp(float a, float b, float t);
COLORREF LerpColor(COLORREF a, COLORREF b, float t);
RECT LerpRect(const RECT& a, const RECT& b, float t);

// 系统“显示动画”开关（SPI_GETCLIENTAREAANIMATION）。关闭时所有时长按 0 处理。
bool SystemAnimationsEnabled();

// 单一 UI 线程动画器：所有轨道共用一个 16 ms 定时器。
// 只保存数值并回调 apply，不创建 GDI 对象；调用方负责 InvalidateRect。
class Animator {
public:
    using AnimId = uint64_t;

    // animationsEnabled=false 用于测试/系统关闭动画；默认读取系统设置。
    explicit Animator(bool animationsEnabled = SystemAnimationsEnabled());

    // 以 NowMs() 为起点。
    AnimId Add(float from, float to, int durationMs, Easing easing,
               std::function<void(float)> apply, int delayMs = 0);

    // 显式起点，便于测试与同一帧内启动多条轨道。
    AnimId AddAt(double nowMs, float from, float to, int durationMs, Easing easing,
                 std::function<void(float)> apply, int delayMs = 0);

    // 推进所有轨道；apply 在 t=1 时保证被调用一次。返回是否仍有活动轨道。
    bool Tick(double nowMs);

    bool Active() const { return !tracks_.empty(); }
    size_t Count() const { return tracks_.size(); }
    void Cancel(AnimId id);
    void Clear();

private:
    struct Track {
        AnimId id = 0;
        float from = 0.0f;
        float to = 0.0f;
        int durationMs = 0;
        int delayMs = 0;
        Easing easing = Easing::Standard;
        std::function<void(float)> apply;
        double startMs = 0.0;
    };

    AnimId nextId_ = 1;
    bool animationsEnabled_ = true;
    std::vector<Track> tracks_;
};
