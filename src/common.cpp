#include "common.h"

#include <shlobj.h>

#include <cwctype>
#include <mutex>

namespace {

std::mutex g_logMutex;

std::wstring MakeAppDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr,
                                   SHGFP_TYPE_CURRENT, buf))) {
        std::wstring dir = std::wstring(buf) + L"\\2.4GCenter";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"2.4GCenter";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

}  // namespace

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wchar_t small[512];
    int n = _vsnwprintf_s(small, ARRAYSIZE(small), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n >= 0) return std::wstring(small, (size_t)n);
    va_start(ap, fmt);
    int need = _vscwprintf(fmt, ap) + 2;
    va_end(ap);
    std::wstring out((size_t)need, L'\0');
    va_start(ap, fmt);
    _vsnwprintf_s(out.data(), out.size(), _TRUNCATE, fmt, ap);
    va_end(ap);
    out.resize(wcslen(out.c_str()));
    return out;
}

std::wstring NowTimeString() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Fmt(L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
}

bool SystemNotificationsEnabled() {
    // Windows 10/11 的全局通知开关；键不存在时按默认“开启”处理。
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LONG rc = RegGetValueW(HKEY_CURRENT_USER,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
                           L"ToastEnabled", RRF_RT_REG_DWORD, &type, &value, &size);
    if (rc != ERROR_SUCCESS) return true;
    return value != 0;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return buf;
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring AppDataDir() {
    static std::wstring dir = MakeAppDir();
    return dir;
}

std::wstring AppIniPath() { return AppDataDir() + L"\\settings.ini"; }
std::wstring AppLogPath() { return AppDataDir() + L"\\log.txt"; }

void LogLine(const wchar_t* fmt, ...) {
    wchar_t body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, ARRAYSIZE(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wstring path = AppLogPath();

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        (uint64_t)fad.nFileSizeHigh * (1ull << 32) + fad.nFileSizeLow > 1024 * 1024) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f) return;
    std::wstring line = NowTimeString() + L"  " + body + L"\r\n";
    std::string utf8 = WideToUtf8(line);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
}

void LogHex(const wchar_t* tag, const uint8_t* data, size_t len) {
    std::wstring s;
    s.reserve(len * 3 + 16);
    for (size_t i = 0; i < len; ++i) {
        wchar_t b[8];
        swprintf(b, ARRAYSIZE(b), L"%02X ", data[i]);
        s += b;
    }
    LogLine(L"%s %s", tag, s.c_str());
}

int ReadIniInt(const wchar_t* section, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(section, key, def, AppIniPath().c_str());
}

void WriteIniInt(const wchar_t* section, const wchar_t* key, int value) {
    std::wstring s = std::to_wstring(value);
    WritePrivateProfileStringW(section, key, s.c_str(), AppIniPath().c_str());
}

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, def, buf, ARRAYSIZE(buf), AppIniPath().c_str());
    return buf;
}

void WriteIniString(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), AppIniPath().c_str());
}

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    if (!needle || !*needle) return false;
    std::wstring h = Lower(hay);
    std::wstring n = Lower(needle);
    return h.find(n) != std::wstring::npos;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n\0", 0, 5);
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n\0", std::wstring::npos, 5);
    return s.substr(a, b - a + 1);
}

std::wstring Lower(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) c = (wchar_t)towlower(c);
    return out;
}

std::wstring JoinLines(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += L"\r\n";
        out += lines[i];
    }
    return out;
}

std::wstring FormatBytes(uint64_t n) {
    if (n > 1024ull * 1024 * 1024)
        return Fmt(L"%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
    if (n > 1024ull * 1024) return Fmt(L"%.1f MB", (double)n / (1024.0 * 1024.0));
    if (n > 1024) return Fmt(L"%.1f KB", (double)n / 1024.0);
    return Fmt(L"%llu B", (unsigned long long)n);
}
