#pragma once

#include "common.h"

// 鼠标测试页的统计引擎：纯逻辑，不依赖窗口与 GDI。
// Raw Input 的解包、设备句柄过滤和“光标是否在卡片内”由调用方负责，
// 引擎只接收已决定要统计的 RAWMOUSE。规格见 docs/SPEC-mouse-test.md。
// 所有方法均在 UI 线程调用，内部不加锁。

struct MouseTestSnapshot {
    uint64_t reports = 0;  // 移动报文数（轨迹卡“报告 N”）
    uint64_t buttonDown[5] = {};
    uint64_t doubleClicks = 0;
    uint64_t badDoubleClicks = 0;
    int lastDoubleClickMs = -1;
    int minBadDoubleClickMs = -1;
    uint64_t wheelUp = 0, wheelDown = 0;
    double wheelPerSec = 0.0;
    double pollingHz = 0.0;
    double medianIntervalUs = 0.0;
    uint64_t pollingSamples = 0;
    double within10Pct = 0.0;
    double bucketPct[5] = {};
    const wchar_t* stability = L"等待数据";
    double cps = 0.0;
    double bestCps = 0.0;
    int clickWindowSec = 5;
    // 最近一次左键按下的 QPC 时刻与频率：UI 用它判断“当前 CPS”是否已过期
    // （停手超过窗口长度就应显示 0，而不是冻结在最后一次点击时的值）。
    int64_t lastClickQpc = 0;
    int64_t qpcFreq = 0;
    uint64_t jumps = 0;
    uint64_t pathLength = 0;  // 轨迹累计曼哈顿路径长度（像素，随轨迹一起重置）
    double maxGapMs = 0.0;
};

// 按键显示名：下标与 RAWMOUSE 的 BUTTON_1..BUTTON_5 一致
// （0=左键、1=右键、2=中键、3=后退、4=前进；微软把 BUTTON_2/3 定义为右/中键）。
// 越界返回 L"-"，便于测试与调用方防御。
const wchar_t* MouseButtonName(int index);

// 测试页目标记忆：按设备 id 查下拉项对应的鼠标序号；空 id 或找不到返回 -1。
// 用于设备拔插后判断目标是否仍在快照中（不能只按名称，重名会选错设备）。
int MouseTargetIndexById(const std::vector<std::wstring>& ids, const std::wstring& targetId);

class MouseTestEngine {
public:
    MouseTestEngine();
    void ResetAll();
    void ResetCounters();
    void ResetTrajectory();
    void ResetPolling();
    void SetClickWindow(int seconds);   // clamp 5 or 10
    void SetTargetHandle(HANDLE h);     // nullptr = accept all
    bool AcceptsHandle(HANDLE h) const;
    void AddMouse(const RAWMOUSE& m, int64_t qpcTicks, int64_t qpcFreq);
    MouseTestSnapshot Snapshot() const;
    const std::vector<POINT>& Trajectory() const;

private:
    static constexpr size_t kPollingCapacity = 4096;
    static constexpr size_t kTrajectoryCapacity = 32768;
    static constexpr size_t kClickCapacity = 4096;
    static constexpr size_t kWheelCapacity = 4096;

    struct WheelEvent {
        int64_t us = 0;
        int notches = 0;  // 正数上滚，负数下滚
    };

    static bool IsMoveReport(const RAWMOUSE& m);
    static int64_t TicksToUs(int64_t ticks, int64_t freq);

    void PushTrajectoryPoint(int64_t x, int64_t y);
    void PushClickTime(int64_t us);
    void UpdateBestCps(int64_t us);
    uint64_t CountClicksWithin(int64_t anchorUs, int64_t windowUs) const;
    void PushWheelEvent(int64_t us, int notches);
    double ComputeWheelPerSec() const;
    void RebuildTrajectoryCache() const;

    HANDLE target_ = nullptr;
    int clickWindowSec_ = 5;
    // 最近一次左键按下的原始 QPC 与频率，供 UI 判断 CPS 是否过期。
    int64_t lastLeftClickQpc_ = 0;
    int64_t qpcFreq_ = 0;

    // 按键与双击：lastButtonDownUs_ 只用于配对的相邻按下。
    uint64_t buttonDown_[5] = {};
    uint64_t doubleClicks_ = 0;
    uint64_t badDoubleClicks_ = 0;
    int lastDoubleClickMs_ = -1;
    int minBadDoubleClickMs_ = -1;
    bool hasButtonDown_[5] = {};
    int64_t lastButtonDownUs_[5] = {};

    // 滚轮：剩余不足一格的 delta 留到下一次同向/反向事件继续累计。
    uint64_t wheelUp_ = 0;
    uint64_t wheelDown_ = 0;
    int wheelRemainder_ = 0;
    WheelEvent wheelEvents_[kWheelCapacity] = {};
    size_t wheelHead_ = 0;   // 下一个写入位置
    size_t wheelCount_ = 0;

    // 点击速度：保存左键按下时间，Snapshot 时按选定窗口重新计数。
    int64_t clickTimes_[kClickCapacity] = {};
    size_t clickHead_ = 0;
    size_t clickCount_ = 0;
    double bestCps5_ = 0.0;
    double bestCps10_ = 0.0;

    // 回报率：只存有效间隔（50 µs .. 50 ms），环形覆盖最近 4096 个。
    uint32_t pollingRing_[kPollingCapacity] = {};
    size_t pollingHead_ = 0;
    size_t pollingCount_ = 0;
    int64_t lastPollUs_ = 0;
    bool hasLastPollUs_ = false;

    // 轨迹：位移累加值与点数分开保存；缓存仅在绘制读取时重建。
    std::vector<POINT> trajectoryRing_;  // 固定容量，构造时一次性分配
    size_t trajectoryHead_ = 0;
    size_t trajectoryCount_ = 0;
    int64_t trajectoryX_ = 0;
    int64_t trajectoryY_ = 0;
    uint64_t reports_ = 0;
    uint64_t jumps_ = 0;
    uint64_t pathLength_ = 0;
    double maxGapMs_ = 0.0;
    int64_t lastTrajectoryUs_ = 0;
    bool hasLastTrajectoryUs_ = false;
    mutable std::vector<POINT> trajectoryCache_;
    mutable bool trajectoryDirty_ = true;
};
