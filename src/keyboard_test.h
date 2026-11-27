#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "common.h"

// 键盘测试统计引擎：纯逻辑，不依赖窗口/GDI。
// Raw Input 的解包、扩展位/扫描码归一化由调用方传入；引擎只维护 104 键三态、
// 日志、APM、连击与 N 键无冲统计。规格见 docs/SPEC-keyboard-test.md。
// 所有方法均在 UI 线程调用，内部不加锁。

namespace keyboardtest {

enum class KeyState { Untested, Down, Up };
enum class EventType { Down, Up, Blur };

struct KeyDef {
    const wchar_t* code;        // 稳定标识，如 "a" / "num7" / "lshift"
    const wchar_t* label;       // 键帽主标签
    const wchar_t* shiftLabel;  // 数字/符号键的 Shift 标签；可为空
    const wchar_t* name;        // 日志/诊断显示名（左右修饰键区分）
    float x = 0.0f;             // 布局坐标（单位）
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
    uint16_t vk = 0;
    uint16_t makeCode = 0;      // 0 = 不按扫描码区分
};

struct KeyRuntime {
    bool down = false;
    bool tested = false;
    // hasLastDown：当前时序里至少有过一次按下（用于生成上一次时间戳）；
    // hasPrevDown：已有两次按下，间隔/连击有效。不能用时间戳 == 0 判断，
    // nowMs 可能从 0 开始。
    bool hasLastDown = false;
    bool hasPrevDown = false;
    uint64_t lastDownMs = 0;
    uint64_t prevDownMs = 0;
    uint64_t lastUpMs = 0;
};

struct Event {
    uint64_t monoMs = 0;
    int keyIndex = -1;
    EventType type = EventType::Down;
};

class KeyboardTestEngine {
public:
    static constexpr int kKeyCount = 104;
    static constexpr int kLogCapacity = 64;
    static constexpr int kChatterThresholdMs = 50;
    // APM 5 秒窗口与 30 秒直方图的独立时间戳保留窗口。
    // 日志 64 条会同时容纳 KeyDown/KeyUp，不能拿来算指标。
    static constexpr uint64_t kMetricWindowMs = 30000;

    KeyboardTestEngine();

    void Reset();
    // 只清除“按住”态：离开键盘页/切换模式/隐藏时使用，不伪造 KeyUp 或 Blur。
    void ReleaseAll();

    // 返回实际被接受（状态发生变化）的键下标；-1 = 未匹配/自动重复。
    int KeyDown(uint16_t vk, uint16_t makeCode, bool extended, uint64_t nowMs);
    int KeyUp(uint16_t vk, uint16_t makeCode, bool extended, uint64_t nowMs);
    void Blur(uint64_t nowMs);

    int ResolveKey(uint16_t vk, uint16_t makeCode, bool extended) const;
    const KeyDef& Def(int index) const;
    const KeyRuntime& Runtime(int index) const;
    KeyState State(int index) const;

    int KeyCount() const { return kKeyCount; }
    int DownCount() const;
    int TestedCount() const;
    int MaxSimultaneous() const { return maxSimultaneous_; }
    int ChatterCount() const { return chatterCount_; }
    int LastKeyIndex() const { return lastKeyIndex_; }
    int LastIntervalMs(int index) const;
    int Apm(uint64_t nowMs) const;
    std::vector<int> ApmHistory(uint64_t nowMs, int seconds = 30) const;

    const std::vector<Event>& Events() const { return events_; }

    // Win/Menu 在部分系统/浏览器会被占用；UI 用于提示，不影响统计。
    bool IsSystemReserved(int index) const;

private:
    void PushEvent(uint64_t nowMs, int keyIndex, EventType type);
    void PushDownTime(uint64_t nowMs);
    void PruneDownTimes(uint64_t nowMs);

    std::array<KeyDef, kKeyCount> defs_{};
    std::array<KeyRuntime, kKeyCount> runtime_{};
    // Reset 时仍按住的键：忽略其自动重复，直到物理 KeyUp。
    std::array<bool, kKeyCount> suppressRepeat_{};
    std::vector<Event> events_;
    // 已接受的 KeyDown 时间戳（升序，最多保留 kMetricWindowMs）。
    std::deque<uint64_t> downTimes_;
    int maxSimultaneous_ = 0;
    int chatterCount_ = 0;
    int lastKeyIndex_ = -1;
};

const wchar_t* EventName(EventType type);

}  // namespace keyboardtest
