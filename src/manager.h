#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "protocols.h"

constexpr UINT WM_APP_SNAPSHOT = WM_APP + 10;
constexpr UINT WM_APP_APPLY_RESULT = WM_APP + 11;

struct Snapshot {
    std::vector<Device> devices;
    std::wstring updated;
    std::wstring status;
    bool scanning = false;
    uint64_t applySerial = 0;
    bool applyOk = false;
    std::wstring applyMessage;
    std::wstring applyDeviceId;
};

class DeviceManager {
public:
    DeviceManager() = default;
    ~DeviceManager();

    void Start(HWND notifyWnd, bool demo);
    void Stop();
    void RequestRefresh();
    void SetPollSeconds(int seconds);
    int PollSeconds() const;
    void Apply(const ApplyRequest& req);
    Snapshot GetSnapshot() const;
    bool IsDemo() const { return demo_; }

private:
    void Run();
    void ScanOnce();
    void ProcessApply(const ApplyRequest& req);
    std::vector<Device> BuildDemoDevices() const;
    void ApplyDemo(const ApplyRequest& req, bool& ok, std::wstring& msg);
    void Publish(std::vector<Device> devices, const std::wstring& status);
    void PostSnapshot();

    HWND notifyWnd_ = nullptr;
    bool demo_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ApplyRequest> queue_;
    Snapshot snapshot_;
    int pollSeconds_ = 5;
    bool refreshRequested_ = false;
    std::vector<Device> demoDevices_;
};
