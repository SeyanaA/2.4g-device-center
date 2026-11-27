#include "mouse_test.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {

// Snapshot 只读；用整数比较避免 90%/70% 边界上的浮点误差。
bool AtLeastPercent(uint64_t part, uint64_t total, uint64_t percent) {
    return total != 0 && part * 100 >= total * percent;
}

}  // namespace

MouseTestEngine::MouseTestEngine() {
    // 轨迹容量较大，构造时一次性分配；运行期只做环形写入，不产生分配。
    trajectoryRing_.resize(kTrajectoryCapacity);
    trajectoryCache_.reserve(kTrajectoryCapacity);
    ResetAll();
}

bool MouseTestEngine::IsMoveReport(const RAWMOUSE& m) {
    // 相对模式的 usFlags 没有“移动”位（MOUSE_MOVE_RELATIVE == 0），
    // 因此以非零位移或绝对坐标标志判断是否为移动报文。
    return (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0 || m.lLastX != 0 || m.lLastY != 0;
}

int64_t MouseTestEngine::TicksToUs(int64_t ticks, int64_t freq) {
    if (freq <= 0 || ticks <= 0) return 0;
    // 拆成整秒与余数，避免 ticks * 1e6 在大计数下溢出 int64。
    const int64_t seconds = ticks / freq;
    const int64_t remainder = ticks % freq;
    return seconds * 1000000 + (remainder * 1000000 + freq / 2) / freq;
}

void MouseTestEngine::PushTrajectoryPoint(int64_t x, int64_t y) {
    trajectoryRing_[trajectoryHead_].x = static_cast<LONG>(x);
    trajectoryRing_[trajectoryHead_].y = static_cast<LONG>(y);
    trajectoryHead_ = (trajectoryHead_ + 1) % kTrajectoryCapacity;
    if (trajectoryCount_ < kTrajectoryCapacity) ++trajectoryCount_;
    trajectoryDirty_ = true;
}

void MouseTestEngine::RebuildTrajectoryCache() const {
    trajectoryCache_.clear();
    const size_t oldest =
        (trajectoryHead_ + kTrajectoryCapacity - trajectoryCount_) % kTrajectoryCapacity;
    for (size_t i = 0; i < trajectoryCount_; ++i) {
        trajectoryCache_.push_back(trajectoryRing_[(oldest + i) % kTrajectoryCapacity]);
    }
    trajectoryDirty_ = false;
}

void MouseTestEngine::PushClickTime(int64_t us) {
    clickTimes_[clickHead_] = us;
    clickHead_ = (clickHead_ + 1) % kClickCapacity;
    if (clickCount_ < kClickCapacity) ++clickCount_;
}

uint64_t MouseTestEngine::CountClicksWithin(int64_t anchorUs, int64_t windowUs) const {
    if (clickCount_ == 0) return 0;
    const int64_t cutoff = anchorUs - windowUs;
    const size_t oldest =
        (clickHead_ + kClickCapacity - clickCount_) % kClickCapacity;
    uint64_t count = 0;
    for (size_t i = 0; i < clickCount_; ++i) {
        const int64_t t = clickTimes_[(oldest + i) % kClickCapacity];
        if (t >= cutoff && t <= anchorUs) ++count;
    }
    return count;
}

void MouseTestEngine::UpdateBestCps(int64_t us) {
    // 最佳值按 5 s / 10 s 两个窗口分别累计，切换窗口只影响读取哪一个。
    const double cps5 = static_cast<double>(CountClicksWithin(us, 5000000)) / 5.0;
    const double cps10 = static_cast<double>(CountClicksWithin(us, 10000000)) / 10.0;
    if (cps5 > bestCps5_) bestCps5_ = cps5;
    if (cps10 > bestCps10_) bestCps10_ = cps10;
}

void MouseTestEngine::PushWheelEvent(int64_t us, int notches) {
    wheelEvents_[wheelHead_].us = us;
    wheelEvents_[wheelHead_].notches = notches;
    wheelHead_ = (wheelHead_ + 1) % kWheelCapacity;
    if (wheelCount_ < kWheelCapacity) ++wheelCount_;
}

double MouseTestEngine::ComputeWheelPerSec() const {
    if (wheelCount_ == 0) return 0.0;
    const size_t oldest =
        (wheelHead_ + kWheelCapacity - wheelCount_) % kWheelCapacity;
    auto at = [&](size_t logical) -> const WheelEvent& {
        return wheelEvents_[(oldest + logical) % kWheelCapacity];
    };

    // 只评估最近 10 s 内的事件，再从每个起点做不超过 1.0 s 的滑动窗口。
    const int64_t newest = at(wheelCount_ - 1).us;
    const int64_t cutoff = newest - 10000000;
    size_t start = 0;
    while (start < wheelCount_ && at(start).us < cutoff) ++start;

    uint64_t best = 0;
    uint64_t current = 0;
    size_t end = start;
    for (size_t i = start; i < wheelCount_; ++i) {
        if (end < i) end = i;
        while (end < wheelCount_ && at(end).us - at(i).us <= 1000000) {
            const int n = at(end).notches;
            current += static_cast<uint64_t>(n < 0 ? -n : n);
            ++end;
        }
        if (current > best) best = current;
        const int n = at(i).notches;
        current -= static_cast<uint64_t>(n < 0 ? -n : n);
    }
    return static_cast<double>(best);
}

void MouseTestEngine::AddMouse(const RAWMOUSE& m, int64_t qpcTicks, int64_t qpcFreq) {
    const int64_t us = TicksToUs(qpcTicks, qpcFreq);

    // 按键：同一报文可能同时带多个 DOWN 位，逐个处理；
    // 双击只按同一按键的相邻两次按下配对。
    static const USHORT kDownFlags[5] = {
        RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
        RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
    for (int i = 0; i < 5; ++i) {
        if ((m.usButtonFlags & kDownFlags[i]) == 0) continue;
        ++buttonDown_[i];
        if (hasButtonDown_[i]) {
            const int64_t interval = us - lastButtonDownUs_[i];
            if (interval >= 0 && interval <= 500000) {  // ≤ 500 ms 计一次双击
                ++doubleClicks_;
                lastDoubleClickMs_ = static_cast<int>(interval / 1000);
                if (interval < 80000) {  // < 80 ms 疑似故障双击
                    ++badDoubleClicks_;
                    const int ms = static_cast<int>(interval / 1000);
                    if (minBadDoubleClickMs_ < 0 || ms < minBadDoubleClickMs_) {
                        minBadDoubleClickMs_ = ms;
                    }
                }
            }
        }
        hasButtonDown_[i] = true;
        lastButtonDownUs_[i] = us;
    }
    if ((m.usButtonFlags & RI_MOUSE_BUTTON_1_DOWN) != 0) {
        PushClickTime(us);
        UpdateBestCps(us);
        // 记录原始 QPC，UI 用它判断当前 CPS 是否已经过期。
        lastLeftClickQpc_ = qpcTicks;
        qpcFreq_ = qpcFreq;
    }

    // 滚轮：usButtonData 按有符号 16 位解释；±120 为一格。
    // 不足一格的余量保留，避免高分辨率滚轮的小增量被直接丢弃。
    if ((m.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
        const int delta = static_cast<int>(static_cast<int16_t>(m.usButtonData));
        wheelRemainder_ += delta;
        const int notches = wheelRemainder_ / 120;
        wheelRemainder_ -= notches * 120;
        if (notches > 0) {
            wheelUp_ += static_cast<uint64_t>(notches);
        } else if (notches < 0) {
            wheelDown_ += static_cast<uint64_t>(-notches);
        }
        if (notches != 0) PushWheelEvent(us, notches);
    }

    if (!IsMoveReport(m)) return;
    ++reports_;

    const int64_t dx = m.lLastX;
    const int64_t dy = m.lLastY;

    // 轨迹原点固定为 (0,0)：第一段位移之前先补一个原点采样。
    if (reports_ == 1) {
        trajectoryX_ = 0;
        trajectoryY_ = 0;
        PushTrajectoryPoint(0, 0);
    }
    trajectoryX_ += dx;
    trajectoryY_ += dy;
    PushTrajectoryPoint(trajectoryX_, trajectoryY_);

    // 单次曼哈顿位移超过 200 计为跳点（甩动/丢帧导致的位置突跳）。
    const int64_t manhattan = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    pathLength_ += static_cast<uint64_t>(manhattan);
    if (manhattan > 200) ++jumps_;

    if (hasLastTrajectoryUs_) {
        const int64_t gap = us - lastTrajectoryUs_;
        // 超过 2 s 视为离开后重新开始，不计入“最长中断”。
        if (gap >= 0 && gap <= 2000000) {
            const double gapMs = static_cast<double>(gap) / 1000.0;
            if (gapMs > maxGapMs_) maxGapMs_ = gapMs;
        }
    }
    lastTrajectoryUs_ = us;
    hasLastTrajectoryUs_ = true;

    // 回报率取“相邻移动报文”的间隔；越界样本丢弃，但基准点照常前移，
    // 否则一次 60 ms 的停顿会把后续正常样本也串成超长间隔。
    if (hasLastPollUs_) {
        const int64_t interval = us - lastPollUs_;
        if (interval >= 50 && interval <= 50000) {
            pollingRing_[pollingHead_] = static_cast<uint32_t>(interval);
            pollingHead_ = (pollingHead_ + 1) % kPollingCapacity;
            if (pollingCount_ < kPollingCapacity) ++pollingCount_;
        }
    }
    lastPollUs_ = us;
    hasLastPollUs_ = true;
}

MouseTestSnapshot MouseTestEngine::Snapshot() const {
    MouseTestSnapshot s;
    s.reports = reports_;
    for (int i = 0; i < 5; ++i) s.buttonDown[i] = buttonDown_[i];
    s.doubleClicks = doubleClicks_;
    s.badDoubleClicks = badDoubleClicks_;
    s.lastDoubleClickMs = lastDoubleClickMs_;
    s.minBadDoubleClickMs = minBadDoubleClickMs_;
    s.wheelUp = wheelUp_;
    s.wheelDown = wheelDown_;
    s.wheelPerSec = ComputeWheelPerSec();
    s.clickWindowSec = clickWindowSec_;
    s.jumps = jumps_;
    s.pathLength = pathLength_;
    s.maxGapMs = maxGapMs_;
    s.stability = L"等待数据";

    // 当前 CPS 以“最近一次左键按下”为窗口右端，还没有点击时为 0。
    if (clickCount_ > 0) {
        const size_t latest = (clickHead_ + kClickCapacity - 1) % kClickCapacity;
        const int64_t anchor = clickTimes_[latest];
        const int64_t windowUs = static_cast<int64_t>(clickWindowSec_) * 1000000;
        s.cps = static_cast<double>(CountClicksWithin(anchor, windowUs)) / clickWindowSec_;
    }
    s.bestCps = clickWindowSec_ == 10 ? bestCps10_ : bestCps5_;
    s.lastClickQpc = lastLeftClickQpc_;
    s.qpcFreq = qpcFreq_;

    if (pollingCount_ > 0) {
        std::array<uint32_t, kPollingCapacity> values{};
        const size_t oldest =
            (pollingHead_ + kPollingCapacity - pollingCount_) % kPollingCapacity;
        for (size_t i = 0; i < pollingCount_; ++i) {
            values[i] = pollingRing_[(oldest + i) % kPollingCapacity];
        }
        const size_t n = pollingCount_;
        const size_t mid = n / 2;
        // 只取中位，避免 50 ms 刷新时对 4096 个样本做全排序。
        std::nth_element(values.begin(), values.begin() + mid, values.begin() + n);
        // 偶数个样本取中间两个的平均值，避免中位间隔在 1 kHz 附近跳动。
        double median = static_cast<double>(values[mid]);
        if ((n % 2) == 0) {
            const uint32_t lower =
                *std::max_element(values.begin(), values.begin() + mid);
            median = (static_cast<double>(lower) + static_cast<double>(values[mid])) / 2.0;
        }

        s.medianIntervalUs = median;
        s.pollingSamples = n;
        if (median > 0.0) s.pollingHz = 1000000.0 / median;

        uint64_t buckets[5] = {};
        for (size_t i = 0; i < n; ++i) {
            const double v = static_cast<double>(values[i]);
            // 五段：≤-20%、(-20%,-10%)、±10%、(+10%,+20%)、≥+20%。
            if (v <= median * 0.8) {
                ++buckets[0];
            } else if (v < median * 0.9) {
                ++buckets[1];
            } else if (v <= median * 1.1) {
                ++buckets[2];
            } else if (v < median * 1.2) {
                ++buckets[3];
            } else {
                ++buckets[4];
            }
        }
        for (int b = 0; b < 5; ++b) {
            s.bucketPct[b] = static_cast<double>(buckets[b]) * 100.0 / static_cast<double>(n);
        }
        s.within10Pct =
            static_cast<double>(buckets[2]) * 100.0 / static_cast<double>(n);
        if (AtLeastPercent(buckets[2], n, 90)) {
            s.stability = L"稳定";
        } else if (AtLeastPercent(buckets[2], n, 70)) {
            s.stability = L"一般";
        } else {
            s.stability = L"不稳定";
        }
    }
    return s;
}

const std::vector<POINT>& MouseTestEngine::Trajectory() const {
    if (trajectoryDirty_) RebuildTrajectoryCache();
    return trajectoryCache_;
}

void MouseTestEngine::ResetAll() {
    ResetCounters();
    ResetTrajectory();
    ResetPolling();
    // 历史最佳只由 ResetAll 清空；ResetCounters 保留它，窗口切换也不影响。
    bestCps5_ = 0.0;
    bestCps10_ = 0.0;
}

void MouseTestEngine::ResetCounters() {
    for (int i = 0; i < 5; ++i) {
        buttonDown_[i] = 0;
        hasButtonDown_[i] = false;
        lastButtonDownUs_[i] = 0;
    }
    doubleClicks_ = 0;
    badDoubleClicks_ = 0;
    lastDoubleClickMs_ = -1;
    minBadDoubleClickMs_ = -1;

    wheelUp_ = 0;
    wheelDown_ = 0;
    wheelRemainder_ = 0;
    wheelHead_ = 0;
    wheelCount_ = 0;

    clickHead_ = 0;
    clickCount_ = 0;
}

void MouseTestEngine::ResetTrajectory() {
    trajectoryHead_ = 0;
    trajectoryCount_ = 0;
    trajectoryX_ = 0;
    trajectoryY_ = 0;
    reports_ = 0;
    jumps_ = 0;
    pathLength_ = 0;
    maxGapMs_ = 0.0;
    lastTrajectoryUs_ = 0;
    hasLastTrajectoryUs_ = false;
    trajectoryDirty_ = true;
}

void MouseTestEngine::ResetPolling() {
    pollingHead_ = 0;
    pollingCount_ = 0;
    lastPollUs_ = 0;
    hasLastPollUs_ = false;
}

void MouseTestEngine::SetClickWindow(int seconds) {
    // UI 只提供 5 s / 10 s；其他值就近归入合法档位，避免出现 0 秒窗口。
    clickWindowSec_ = (seconds >= 8) ? 10 : 5;
}

void MouseTestEngine::SetTargetHandle(HANDLE h) {
    target_ = h;
}

bool MouseTestEngine::AcceptsHandle(HANDLE h) const {
    // nullptr 表示“任意鼠标”；设备过滤实际由调用方在分发 WM_INPUT 前完成。
    return target_ == nullptr || target_ == h;
}

const wchar_t* MouseButtonName(int index) {
    // 顺序必须与 RAWMOUSE 的 BUTTON_1..BUTTON_5 一致：微软把 2/3 定义为右/中键。
    static const wchar_t* const kNames[5] = {L"左键", L"右键", L"中键", L"后退", L"前进"};
    if (index < 0 || index >= 5) return L"-";
    return kNames[index];
}

int MouseTargetIndexById(const std::vector<std::wstring>& ids, const std::wstring& targetId) {
    if (targetId.empty()) return -1;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == targetId) return (int)i;
    }
    return -1;
}

