#pragma once

#include <functional>

#include "common.h"

struct HidInterface {
    std::wstring path;
    std::wstring basePath;      // path before &col (all collections of one interface)
    std::wstring product;
    std::wstring manufacturer;
    uint16_t vid = 0;
    uint16_t pid = 0;
    int interfaceNumber = -1;
    int collection = -1;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint16_t inLen = 0;
    uint16_t outLen = 0;
    uint16_t featLen = 0;
    bool canOpenRead = false;
    bool canOpenRW = false;
};

struct SystemBattery {
    std::wstring id;
    std::wstring name;
    int percent = -1;
    bool charging = false;
    int millivolts = 0;
    uint32_t capacity = 0;
    uint32_t fullCapacity = 0;
};

std::vector<HidInterface> EnumerateHidInterfaces();
std::vector<SystemBattery> EnumerateSystemBatteries();

bool ParseVidPid(const std::wstring& path, uint16_t& vid, uint16_t& pid);
std::wstring HidPathKey(const std::wstring& path);
std::wstring DeviceKindFromText(const std::wstring& text);

class HidHandle {
public:
    HidHandle() = default;
    ~HidHandle();
    HidHandle(const HidHandle&) = delete;
    HidHandle& operator=(const HidHandle&) = delete;
    HidHandle(HidHandle&& other) noexcept;
    HidHandle& operator=(HidHandle&& other) noexcept;

    bool Open(const HidInterface& info, DWORD access = GENERIC_READ | GENERIC_WRITE,
              bool overlapped = true);
    bool OpenPath(const std::wstring& path, DWORD access, bool overlapped);
    void Close();

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE handle() const { return handle_; }
    const HidInterface& info() const { return info_; }
    bool overlapped() const { return overlapped_; }

    bool sendRaw(const uint8_t* data, size_t len, DWORD timeoutMs);
    bool writeReport(uint8_t reportId, const uint8_t* data, size_t len, DWORD timeoutMs);
    int readRaw(uint8_t* buf, size_t len, DWORD timeoutMs);
    int readMatching(uint8_t* buf, size_t len,
                     const std::function<bool(const uint8_t*, int)>& match, DWORD timeoutMs);
    bool getFeature(uint8_t reportId, uint8_t* data, size_t len, DWORD timeoutMs);
    bool setFeature(uint8_t reportId, const uint8_t* data, size_t len, DWORD timeoutMs);

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HidInterface info_{};
    bool overlapped_ = false;
};
