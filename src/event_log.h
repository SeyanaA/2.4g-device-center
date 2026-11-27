#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "common.h"

// 设备事件日志的解析与聚合（纯逻辑，不依赖窗口）。规格见 docs/SPEC-history-ui.md。

struct EventRecord {
    int64_t epoch = 0;  // 本地墙钟按 UTC 解释的秒数，仅用于排序与画图坐标
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    std::wstring type;      // CONNECT / DISCONNECT / BATTERY / CHARGE / SAMPLE
    std::wstring identity;
    std::wstring name;
    std::wstring detail;
    int pct = -1;
    int mv = -1;
    int charging = -1;  // -1 未知，0 放电，1 充电
};

struct BatterySample {
    int64_t epoch = 0;
    int pct = 0;
    int charging = 0;
};

struct EventDevice {
    std::wstring identity;
    std::wstring name;
    int64_t lastEpoch = 0;
};

// 解析一行事件日志；时间/类型/identity 不完整时返回 false。
bool ParseEventLogLine(const std::wstring& line, EventRecord& out);

// 读取最近 days 天（默认 7）的日志文件，按时间升序返回。
std::vector<EventRecord> LoadEventRecords(int days = 7);

// 取某设备带电量数值的采样点（CONNECT/BATTERY/SAMPLE/DISCONNECT），按时间升序。
// identity 为空时返回全部设备。
std::vector<BatterySample> BuildBatterySeries(const std::vector<EventRecord>& records,
                                              const std::wstring& identity);

// identity 去重后的设备列表，按最近出现时间倒序。
std::vector<EventDevice> CollectEventDevices(const std::vector<EventRecord>& records);

// 采样节流：首次（lastEpoch<=0）立即允许，之后需间隔 intervalSec。
bool ShouldLogBatterySample(int64_t lastEpoch, int64_t nowEpoch, int intervalSec = 1800);

// 本地日期 → 自 1970-01-01 起的秒数（按 UTC 解释墙钟）。
int64_t EventEpoch(int year, int month, int day, int hour, int minute, int second);

// 反向换算，供坐标轴标签使用。
bool EventDateFromEpoch(int64_t epoch, int& year, int& month, int& day);

// 机器可读字段：pct=95 mv=4076 chg=0；未知值自动省略。
std::wstring EventFields(int pct, int mv, int charging);

// 导出用：时间文本、CSV 行（含转义）、文本行。
std::wstring EventTimeText(const EventRecord& r);
std::wstring EventRecordCsvLine(const EventRecord& r);
std::wstring EventRecordTextLine(const EventRecord& r);

// 7 天曲线视窗：保证 [start, start+span] 落在 [minStart, maxEnd] 内，span 不超过总窗口。
// 记录是否落在“最近 days 天”（按本地自然日，todayStart 为今天 00:00）。
bool EventWithinDays(int64_t epoch, int days, int64_t todayStart);
