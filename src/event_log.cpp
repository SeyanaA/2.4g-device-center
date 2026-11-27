#include "event_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

void CivilFromDays(int z, int& y, int& m, int& d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += m <= 2;
}

bool ReadNumber(const std::wstring& s, size_t pos, int count, int& value) {
    value = 0;
    for (int i = 0; i < count; ++i) {
        wchar_t c = s[pos + (size_t)i];
        if (c < L'0' || c > L'9') return false;
        value = value * 10 + (c - L'0');
    }
    return true;
}

bool ReadSeparated(const std::wstring& s, size_t& pos, std::wstring& out) {
    size_t next = s.find(L"  ", pos);
    if (next == std::wstring::npos) return false;
    out = s.substr(pos, next - pos);
    pos = next + 2;
    return true;
}

int FindToken(const std::wstring& s, const wchar_t* key) {
    size_t p = s.find(key);
    if (p == std::wstring::npos) return -1;
    p += wcslen(key);
    int value = 0;
    bool any = false;
    while (p < s.size() && s[p] >= L'0' && s[p] <= L'9') {
        value = value * 10 + (s[p] - L'0');
        p += 1;
        any = true;
    }
    return any ? value : -1;
}

// 旧格式兜底：从 “95%”“最后读数 94%” 里取百分比。
int LegacyPercent(const std::wstring& detail, bool takeLast) {
    int value = -1;
    for (size_t i = 0; i < detail.size(); ++i) {
        if (detail[i] != L'%' || i == 0) continue;
        size_t start = i;
        while (start > 0 && detail[start - 1] >= L'0' && detail[start - 1] <= L'9') --start;
        if (start == i) continue;
        int v = 0;
        for (size_t k = start; k < i; ++k) v = v * 10 + (detail[k] - L'0');
        if (v < 0 || v > 100) continue;
        value = v;
        if (!takeLast) return value;
    }
    return value;
}

int LegacyCharging(const std::wstring& detail) {
    if (detail.find(L"充电中") != std::wstring::npos ||
        detail.find(L"已充满") != std::wstring::npos) {
        return 1;
    }
    if (detail.find(L"电池供电") != std::wstring::npos) return 0;
    return -1;
}

// 新格式的 BATTERY 明细是 “pct 96->95”，CONNECT/SAMPLE 是 “pct=95”；
// 两种都要解析，旧日志再回退到 “95%” 文本。
int PercentFromDetail(const std::wstring& detail) {
    int value = FindToken(detail, L"pct=");
    if (value >= 0) return value;
    size_t p = detail.find(L"pct ");
    if (p != std::wstring::npos) {
        p += 4;
        int first = 0;
        bool any = false;
        while (p < detail.size() && detail[p] >= L'0' && detail[p] <= L'9') {
            first = first * 10 + (detail[p] - L'0');
            ++p;
            any = true;
        }
        if (!any) return -1;
        while (p < detail.size() && detail[p] == L' ') ++p;
        if (detail.compare(p, 2, L"->") == 0) {
            p += 2;
            while (p < detail.size() && detail[p] == L' ') ++p;
            int second = 0;
            bool anySecond = false;
            while (p < detail.size() && detail[p] >= L'0' && detail[p] <= L'9') {
                second = second * 10 + (detail[p] - L'0');
                ++p;
                anySecond = true;
            }
            if (anySecond) return second;
        }
        return first;
    }
    return -1;
}

void AppendFile(const std::wstring& path, std::vector<EventRecord>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return;
    }
    std::string bytes((size_t)size, '\0');
    size_t got = fread(bytes.data(), 1, (size_t)size, f);
    fclose(f);
    bytes.resize(got);

    std::wstring text = Utf8ToWide(bytes);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        EventRecord rec;
        if (!line.empty() && ParseEventLogLine(line, rec)) out.push_back(std::move(rec));
        pos = end + 1;
    }
}

}  // namespace

int64_t EventEpoch(int year, int month, int day, int hour, int minute, int second) {
    return (int64_t)DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

bool EventDateFromEpoch(int64_t epoch, int& year, int& month, int& day) {
    int64_t days = epoch / 86400;
    if (epoch < 0 && epoch % 86400 != 0) days -= 1;
    int y = 0, m = 0, d = 0;
    CivilFromDays((int)days, y, m, d);
    if (y < 1970 || y > 2200) return false;
    year = y;
    month = m;
    day = d;
    return true;
}

bool ParseEventLogLine(const std::wstring& line, EventRecord& out) {
    // 最短：时间 19 + 两个空格 + 类型 + 两个空格 + identity
    if (line.size() < 25) return false;
    if (line[4] != L'-' || line[7] != L'-' || line[10] != L' ' || line[13] != L':' ||
        line[16] != L':') {
        return false;
    }
    EventRecord rec;
    if (!ReadNumber(line, 0, 4, rec.year) || !ReadNumber(line, 5, 2, rec.month) ||
        !ReadNumber(line, 8, 2, rec.day) || !ReadNumber(line, 11, 2, rec.hour) ||
        !ReadNumber(line, 14, 2, rec.minute) || !ReadNumber(line, 17, 2, rec.second)) {
        return false;
    }
    if (rec.month < 1 || rec.month > 12 || rec.day < 1 || rec.day > 31) return false;
    if (line.compare(19, 2, L"  ") != 0) return false;

    size_t pos = 21;
    if (!ReadSeparated(line, pos, rec.type) || rec.type.empty()) return false;
    if (!ReadSeparated(line, pos, rec.identity) || rec.identity.empty()) return false;

    // 剩下 name [两个空格 detail]；名字本身允许单空格。
    std::wstring rest = line.substr(pos);
    size_t split = rest.rfind(L"  ");
    if (split != std::wstring::npos) {
        rec.name = rest.substr(0, split);
        rec.detail = rest.substr(split + 2);
    } else {
        rec.name = rest;
    }
    if (rec.name.empty()) return false;

    rec.epoch = EventEpoch(rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second);
    rec.pct = PercentFromDetail(rec.detail);
    rec.mv = FindToken(rec.detail, L"mv=");
    rec.charging = FindToken(rec.detail, L"chg=");
    if (rec.charging > 1) rec.charging = -1;
    if (rec.pct < 0) rec.pct = LegacyPercent(rec.detail, rec.type == L"BATTERY");
    if (rec.charging < 0) rec.charging = LegacyCharging(rec.detail);
    out = std::move(rec);
    return true;
}

std::vector<EventRecord> LoadEventRecords(int days) {
    std::vector<EventRecord> out;
    if (days < 1) days = 1;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int64_t todayStart = EventEpoch(st.wYear, st.wMonth, st.wDay, 0, 0, 0);
    for (int i = days - 1; i >= 0; --i) {
        int y = 0, m = 0, d = 0;
        if (!EventDateFromEpoch(todayStart - (int64_t)i * 86400, y, m, d)) continue;
        std::wstring path = EventLogDir() + Fmt(L"\\events-%04d-%02d-%02d.log", y, m, d);
        AppendFile(path, out);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const EventRecord& a, const EventRecord& b) { return a.epoch < b.epoch; });
    return out;
}

std::vector<BatterySample> BuildBatterySeries(const std::vector<EventRecord>& records,
                                              const std::wstring& identity) {
    std::vector<BatterySample> out;
    for (const auto& r : records) {
        if (!identity.empty() && r.identity != identity) continue;
        if (r.pct < 0 || r.pct > 100) continue;
        if (r.type != L"CONNECT" && r.type != L"BATTERY" && r.type != L"SAMPLE" &&
            r.type != L"DISCONNECT") {
            continue;
        }
        BatterySample sample{r.epoch, r.pct, r.charging == 1 ? 1 : 0};
        if (!out.empty() && out.back().epoch == sample.epoch) {
            out.back() = sample;  // 同一秒多条只保留最后一条
        } else {
            out.push_back(sample);
        }
    }
    return out;
}

std::vector<EventDevice> CollectEventDevices(const std::vector<EventRecord>& records) {
    std::map<std::wstring, EventDevice> map;
    for (const auto& r : records) {
        EventDevice& dev = map[r.identity];
        dev.identity = r.identity;
        if (!r.name.empty()) dev.name = r.name;
        if (r.epoch >= dev.lastEpoch) dev.lastEpoch = r.epoch;
    }
    std::vector<EventDevice> out;
    out.reserve(map.size());
    for (auto& [key, dev] : map) out.push_back(std::move(dev));
    std::stable_sort(out.begin(), out.end(), [](const EventDevice& a, const EventDevice& b) {
        if (a.lastEpoch != b.lastEpoch) return a.lastEpoch > b.lastEpoch;
        return Lower(a.name) < Lower(b.name);
    });
    return out;
}

bool ShouldLogBatterySample(int64_t lastEpoch, int64_t nowEpoch, int intervalSec) {
    if (intervalSec < 1) intervalSec = 1;
    if (lastEpoch <= 0) return true;
    return nowEpoch - lastEpoch >= intervalSec;
}

std::wstring EventFields(int pct, int mv, int charging) {
    std::wstring s;
    if (pct >= 0) s += Fmt(L"pct=%d", pct);
    if (mv > 0) {
        if (!s.empty()) s += L" ";
        s += Fmt(L"mv=%d", mv);
    }
    if (charging >= 0) {
        if (!s.empty()) s += L" ";
        s += Fmt(L"chg=%d", charging);
    }
    return s;
}

std::wstring EventTimeText(const EventRecord& r) {
    return Fmt(L"%04d-%02d-%02d %02d:%02d:%02d", r.year, r.month, r.day, r.hour, r.minute,
               r.second);
}

namespace {

std::wstring CsvEscape(const std::wstring& s) {
    if (s.find_first_of(L",\"\r\n") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L"\"\"";
        else out += c;
    }
    out += L"\"";
    return out;
}

}  // namespace

std::wstring EventRecordCsvLine(const EventRecord& r) {
    return CsvEscape(EventTimeText(r)) + L"," + CsvEscape(r.type) + L"," + CsvEscape(r.name) +
           L"," + CsvEscape(r.identity) + L"," + CsvEscape(r.detail);
}

std::wstring EventRecordTextLine(const EventRecord& r) {
    std::wstring line = EventTimeText(r) + L"  " + r.type + L"  " + r.identity + L"  " + r.name;
    if (!r.detail.empty()) line += L"  " + r.detail;
    return line;
}

bool EventWithinDays(int64_t epoch, int days, int64_t todayStart) {
    if (days < 1) days = 1;
    if (days > 7) days = 7;
    int64_t first = todayStart - (int64_t)(days - 1) * 86400;
    int64_t end = todayStart + 86400;
    return epoch >= first && epoch < end;
}
