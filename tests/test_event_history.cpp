#include "event_log.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    // 1. 新格式：CONNECT 带 pct/mv/chg 字段。
    {
        EventRecord r;
        assert(ParseEventLogLine(
            L"2026-11-13 10:40:27  CONNECT  zaopin:z4d  Zaopin Z4D  "
            L"2.4G 无线 · pct=95 mv=4076 chg=0",
            r));
        assert(r.type == L"CONNECT" && r.identity == L"zaopin:z4d");
        assert(r.name == L"Zaopin Z4D");
        assert(r.pct == 95 && r.mv == 4076 && r.charging == 0);
        assert(r.year == 2026 && r.month == 11 && r.day == 13 && r.hour == 10);
    }

    // 2. 旧格式（只有 95% 文本）仍能解析。
    {
        EventRecord r;
        assert(ParseEventLogLine(
            L"2026-11-13 10:41:39  CONNECT  M:vid_1ea7&pid_0064  2.4G Mouse  "
            L"2.4G 无线 · 56% · 充电状态未知",
            r));
        assert(r.pct == 56 && r.charging == -1);
    }

    // 3. BATTERY 取箭头后的新值。
    {
        EventRecord r;
        assert(ParseEventLogLine(
            L"2026-11-13 11:02:00  BATTERY  zaopin:z4d  Zaopin Z4D  "
            L"pct 96->95 · mv 4080->4076 · chg=0",
            r));
        assert(r.pct == 95 && r.charging == 0);
    }

    // 4. 旧格式 BATTERY 取最后一个百分比（95% -> 94% 取 94）。
    {
        EventRecord r;
        assert(ParseEventLogLine(
            L"2026-11-13 11:03:00  BATTERY  zaopin:z4d  Zaopin Z4D  "
            L"95% -> 94% · 4076 mV -> 4050 mV",
            r));
        assert(r.pct == 94);
    }

    // 5. 没有 detail 的行也合法。
    {
        EventRecord r;
        assert(ParseEventLogLine(L"2026-11-13 11:04:00  SAMPLE  dev  Mouse", r));
        assert(r.detail.empty() && r.pct == -1);
    }

    // 6. 残缺行必须拒绝。
    {
        EventRecord r;
        assert(!ParseEventLogLine(L"2026-11-13 11:04:00", r));
        assert(!ParseEventLogLine(L"bad timestamp  SAMPLE  dev  Mouse", r));
        assert(!ParseEventLogLine(L"2026-11-13 11:04:00  SAMPLE", r));
    }

    // 7. EventFields 拼装/省略规则。
    assert(EventFields(95, 4076, 0) == L"pct=95 mv=4076 chg=0");
    assert(EventFields(56, 0, -1) == L"pct=56");
    assert(EventFields(-1, 0, -1).empty());

    // 8. 采样节流：首次允许，30 分钟内不重复。
    assert(ShouldLogBatterySample(0, 100, 1800));
    assert(!ShouldLogBatterySample(1000, 2000, 1800));
    assert(ShouldLogBatterySample(1000, 2800, 1800));

    // 9. 曲线只取带 pct 的指定设备记录，CHARGE 不单独成点。
    {
        std::vector<EventRecord> records;
        EventRecord r;
        r.epoch = 100;
        r.identity = L"a";
        r.type = L"CONNECT";
        r.pct = 90;
        r.charging = 0;
        records.push_back(r);
        r.epoch = 200;
        r.type = L"CHARGE";
        r.pct = -1;
        r.charging = 1;
        records.push_back(r);
        r.epoch = 300;
        r.type = L"SAMPLE";
        r.pct = 89;
        r.charging = 0;
        records.push_back(r);
        r.epoch = 400;
        r.identity = L"b";
        r.type = L"BATTERY";
        r.pct = 50;
        records.push_back(r);

        auto series = BuildBatterySeries(records, L"a");
        assert(series.size() == 2);
        assert(series[0].epoch == 100 && series[0].pct == 90);
        assert(series[1].epoch == 300 && series[1].pct == 89);
    }

    // 10. 同一秒多条只保留最后一条。
    {
        std::vector<EventRecord> records(2);
        records[0].epoch = 100;
        records[0].identity = L"a";
        records[0].type = L"CONNECT";
        records[0].pct = 90;
        records[1] = records[0];
        records[1].pct = 88;
        auto series = BuildBatterySeries(records, L"a");
        assert(series.size() == 1 && series[0].pct == 88);
    }

    // 11. 设备去重、取最近名字、按最近出现时间倒序。
    {
        std::vector<EventRecord> records(3);
        records[0].identity = L"a";
        records[0].name = L"Old Name";
        records[0].epoch = 100;
        records[1].identity = L"a";
        records[1].name = L"New Name";
        records[1].epoch = 300;
        records[2].identity = L"b";
        records[2].name = L"Mouse B";
        records[2].epoch = 200;
        auto devices = CollectEventDevices(records);
        assert(devices.size() == 2);
        assert(devices[0].identity == L"a" && devices[0].name == L"New Name");
        assert(devices[1].identity == L"b");
    }

    // 12. 日期换算跨天正好 86400 秒。
    assert(EventEpoch(2026, 11, 13, 0, 0, 0) - EventEpoch(2026, 11, 12, 0, 0, 0) == 86400);
    assert(EventEpoch(2026, 11, 13, 12, 0, 0) - EventEpoch(2026, 11, 13, 0, 0, 0) == 43200);

    // 13. epoch → 日期可逆。
    {
        int64_t e = EventEpoch(2026, 11, 13, 18, 30, 0);
        int y = 0, m = 0, d = 0;
        assert(EventDateFromEpoch(e, y, m, d));
        assert(y == 2026 && m == 11 && d == 13);
        assert(EventDateFromEpoch(EventEpoch(2028, 2, 29, 0, 0, 0), y, m, d));
        assert(y == 2028 && m == 2 && d == 29);
    }

    // 14. CSV 行：普通字段不加引号。
    {
        EventRecord r;
        r.year = 2026;
        r.month = 11;
        r.day = 13;
        r.hour = 10;
        r.minute = 40;
        r.second = 27;
        r.type = L"CONNECT";
        r.identity = L"zaopin:z4d";
        r.name = L"Zaopin Z4D";
        r.detail = L"2.4G 无线 · pct=95";
        assert(EventRecordCsvLine(r) ==
               L"2026-11-13 10:40:27,CONNECT,Zaopin Z4D,zaopin:z4d,2.4G 无线 · pct=95");
    }

    // 15. CSV 转义：逗号、引号按 RFC4180 处理。
    {
        EventRecord r;
        r.year = 2026;
        r.month = 11;
        r.day = 13;
        r.hour = 1;
        r.minute = 2;
        r.second = 3;
        r.type = L"SAMPLE";
        r.identity = L"a";
        r.name = L"Mouse, \"Pro\"";
        r.detail = L"x,y";
        assert(EventRecordCsvLine(r) ==
               L"2026-11-13 01:02:03,SAMPLE,\"Mouse, \"\"Pro\"\"\",a,\"x,y\"");
    }

    // 16. 文本行与 events-*.log 的格式一致。
    {
        EventRecord r;
        r.year = 2026;
        r.month = 11;
        r.day = 13;
        r.hour = 10;
        r.minute = 40;
        r.second = 27;
        r.type = L"CONNECT";
        r.identity = L"zaopin:z4d";
        r.name = L"Zaopin Z4D";
        r.detail = L"2.4G 无线 · pct=95";
        assert(EventRecordTextLine(r) ==
               L"2026-11-13 10:40:27  CONNECT  zaopin:z4d  Zaopin Z4D  2.4G 无线 · pct=95");
    }

    // 17. “最近 N 天”按本地自然日：今天在 1 天范围内，昨天不在。
    {
        int64_t today = EventEpoch(2026, 11, 14, 0, 0, 0);
        assert(EventWithinDays(today + 10 * 3600, 1, today));
        assert(!EventWithinDays(today - 1 * 3600, 1, today));
        assert(EventWithinDays(today - 1 * 3600, 2, today));
    }

    // 18. 7 天窗口的第一个自然日包含 00:00，前一秒不算。
    {
        int64_t today = EventEpoch(2026, 11, 14, 0, 0, 0);
        int64_t first = today - 6 * 86400;
        assert(EventWithinDays(first, 7, today));
        assert(!EventWithinDays(first - 1, 7, today));
    }

    // 19. 明天的记录不计入；days 超过 7 按 7 处理。
    {
        int64_t today = EventEpoch(2026, 11, 14, 0, 0, 0);
        assert(!EventWithinDays(today + 86400, 7, today));
        assert(!EventWithinDays(today - 7 * 86400, 9, today));
    }

    std::cout << "event_history: 19 tests passed\n";

    return 0;
}
