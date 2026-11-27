#pragma once

#include <atomic>
#include <string>
#include <vector>

// COM 会话音量接口（audiopolicy.h）；audio.h 不拉 Windows 头，这里前置声明。
struct ISimpleAudioVolume;

// Core Audio（WASAPI）包装：播放/采集端点、音量/静音、每应用会话、麦克风电平。
namespace audio {

struct Endpoint {
    std::wstring id;    // IMMDevice::GetId
    std::wstring name;  // PKEY_Device_FriendlyName
    std::wstring containerId;  // PKEY_Device_ContainerId（GUID 字符串，可能为空）
    bool isDefault = false;
    bool isCapture = false;
    int volume = 100;   // 0..100
    bool mute = false;
};

// 当前处于 ACTIVE 状态的播放/采集端点；失败返回空。
std::vector<Endpoint> ListRenderEndpoints();
std::vector<Endpoint> ListCaptureEndpoints();

// 纯逻辑（可单测）：在端点列表里按设备名/产品名找最匹配的一个。
// 返回下标，找不到返回 -1。匹配规则：
//   1) 端点名包含完整设备名/产品名（去括号后仍包含也算），优先最长匹配；
//   2) 否则做“≥4 字符的字母数字片段”交集匹配（如 G733 / Arctis）；
//   3) 分数相同则优先系统默认端点。
int MatchEndpoint(const std::wstring& deviceName, const std::wstring& product,
                  const std::vector<Endpoint>& endpoints);

// 纯逻辑（可单测）：选择“系统输出”目标端点。
// pinnedId 命中时优先；否则系统默认端点；否则第一个；空列表返回 -1。
int ResolveOutputEndpoint(const std::vector<Endpoint>& endpoints,
                          const std::wstring& pinnedId);

// 麦克风匹配：同一物理容器（renderContainerId）优先，其次名称/型号片段。
int MatchCaptureEndpoint(const std::wstring& deviceName, const std::wstring& product,
                         const std::wstring& renderContainerId,
                         const std::vector<Endpoint>& captures);

// 音量夹取到 0..100（可单测）。
int ClampVolume(int percent);
// 解析音量数字输入（可单测）：只接受纯数字（可带首尾空格），结果夹到 0..100；
// 空串/非数字返回 fallback，避免把误输入当成 0。
int ParseVolumePercent(const std::wstring& text, int fallback = -1);

// 耳机推荐音量上限（听力保护）。超过后需要用户显式同意。
inline constexpr int kRecommendedHeadsetVolume = 60;

// 耳机音量保护（可单测）：
// - 已同意时直接返回夹取后的请求值；
// - 未同意且当前 ≤60：请求 >60 时夹到 60；
// - 未同意且当前 >60（外部设置）：不允许继续升高，但允许降低。
int LimitHeadsetVolume(int currentPercent, int requestedPercent,
                       bool allowAboveRecommended);

bool SetVolume(const std::wstring& endpointId, int percent);
bool SetMute(const std::wstring& endpointId, bool mute);
bool GetVolume(const std::wstring& endpointId, int* percent, bool* mute);

// 每应用音量（按进程聚合所有活动会话）。
struct AppSession {
    unsigned long pid = 0;
    std::wstring name;       // 进程映像名；取不到时为“应用 (PID)”
    std::wstring displayName;  // 会话显示名（可能为空）
    std::wstring endpointName;  // 当前实际播放端点（多个时用 / 连接）
    int volume = 100;
    bool mute = false;
    int sessionCount = 0;
};

std::vector<AppSession> ListAppSessions();
bool SetAppVolume(unsigned long pid, int percent);
bool SetAppMute(unsigned long pid, bool mute);

// 写入 0 个会话后的处理策略（可单测）：
//   Write  —— 至少有一个会话写入成功，继续直接写；
//   Reopen —— 一个都没写成且还没重开过：会话可能刚被应用重建，重开缓存一次；
//   GiveUp —— 重开过仍写不进去，结束本次拖动，不再枚举。
enum class AppWritePlan { Write, Reopen, GiveUp };
AppWritePlan PlanAppWrite(int written, bool reopened);

// 应用音量拖动专用（B03）：按下时按 PID 解析一次活动会话并缓存 ISimpleAudioVolume，
// 拖动期间直接写缓存，不再每条 WM_MOUSEMOVE 枚举全部端点/会话。
// 缓存只缩短写入路径：显示值仍来自 ListAppSessions() 的刷新快照。
// 会话在拖动中结束时写入返回 0，由调用方按 PlanAppWrite 重开或结束拖动。
class AppVolumeWriter {
public:
    AppVolumeWriter() = default;
    ~AppVolumeWriter();
    AppVolumeWriter(const AppVolumeWriter&) = delete;
    AppVolumeWriter& operator=(const AppVolumeWriter&) = delete;

    // false 表示该进程当前没有活动会话（缓存为空，写入会返回 0）。
    bool Open(unsigned long pid);
    void Close();
    bool active() const { return !volumes_.empty(); }
    int sessionCount() const { return (int)volumes_.size(); }
    unsigned long pid() const { return pid_; }

    int SetVolume(int percent);  // 返回成功写入的会话数
    int SetMute(bool mute);
    // 拖动语义：一条消息里同时写音量并解除静音，避免两次遍历。
    int SetVolumeUnmuted(int percent);

private:
    unsigned long pid_ = 0;
    std::vector<ISimpleAudioVolume*> volumes_;
};

// 每应用输出/输入设备：Windows 未公开的 AudioPolicyConfig（Win10 1803+/Win11）。
// endpointId 为空表示恢复“默认”；只对当前有音频会话的进程有效。
// 返回 false 表示系统未提供该接口、进程无音频会话或写入失败。
bool SetAppEndpoint(unsigned long pid, bool capture, const std::wstring& endpointId);
bool GetAppEndpoint(unsigned long pid, bool capture, std::wstring& endpointId);

// AudioPolicyConfig 使用 SWD 设备接口路径，而 IMMDevice 给的是 {0.0.0...}。
// 两个纯函数单独暴露，便于单测路径拼接/还原。
std::wstring PolicyDevicePath(const std::wstring& endpointId, bool capture);
std::wstring NormalizePolicyDevicePath(const std::wstring& value);

// 麦克风实时电平（峰值 0..1）。独立线程，共享模式 100 ms 缓冲。
// 同一实例重复 Start 会先停止旧线程；Stop 可安全重复调用。
class MicMonitor {
public:
    MicMonitor();
    ~MicMonitor();
    MicMonitor(const MicMonitor&) = delete;
    MicMonitor& operator=(const MicMonitor&) = delete;

    bool Start(const std::wstring& captureEndpointId);
    void Stop();
    bool running() const;
    float Level() const;
    std::wstring error() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// 打开 Windows「声音」设置（ms-settings:sound）；失败退回传统 mmsys.cpl。
bool OpenSoundSettings();
// 打开 Windows 11「音量混合器/应用音量」所在的系统声音页。
bool OpenAppVolumeSettings();

}  // namespace audio
