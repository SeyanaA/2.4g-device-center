#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

// "Win32" is the Windows API family, not a 32-bit-only target. This project is
// built as x86-64 (PE32+) and relies on 64-bit handle/pointer semantics.
#if !defined(_WIN64)
#error "2.4G Device Center targets 64-bit Windows (x86_64). Build with x86_64-w64-mingw32-g++ -m64."
#endif
static_assert(sizeof(void*) == 8, "This project requires a 64-bit pointer size.");

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring NowTimeString();
std::wstring ExePath();
std::wstring ExeDir();
std::wstring AppDataDir();
std::wstring AppIniPath();
std::wstring AppLogPath();

void LogLine(const wchar_t* fmt, ...);
void LogHex(const wchar_t* tag, const uint8_t* data, size_t len);

int ReadIniInt(const wchar_t* section, const wchar_t* key, int def);
void WriteIniInt(const wchar_t* section, const wchar_t* key, int value);
std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* def);
void WriteIniString(const wchar_t* section, const wchar_t* key, const std::wstring& value);

int ClampInt(int v, int lo, int hi);
bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle);
std::wstring Trim(const std::wstring& s);
std::wstring Lower(const std::wstring& s);
std::wstring JoinLines(const std::vector<std::wstring>& lines);

std::wstring FormatBytes(uint64_t n);
