#include "audio.h"

#include <windows.h>

// MinGW：在本 TU 定义 CLSID/IID，避免依赖 import lib。
#include <initguid.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <roapi.h>
#include <shellapi.h>
#include <winstring.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

#include "common.h"

namespace audio {
namespace {

bool EnsureCom() {
    // UI 线程只初始化一次；不主动 CoUninitialize，随进程退出回收。
    static thread_local bool initialized = false;
    static thread_local bool ok = false;
    if (!initialized) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        initialized = true;
    }
    return ok;
}

std::wstring EndpointIdOf(IMMDevice* dev) {
    if (!dev) return {};
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) return {};
    std::wstring out(id);
    CoTaskMemFree(id);
    return out;
}

std::wstring LowerWide(std::wstring s);

std::wstring FriendlyNameOf(IMMDevice* dev) {
    if (!dev) return {};
    IPropertyStore* props = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props)) || !props) return {};
    std::wstring name;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        name = v.pwszVal;
    }
    PropVariantClear(&v);
    props->Release();
    return name;
}

// 同一物理设备的渲染/采集端点共享 containerId；用于耳机麦克风匹配。
std::wstring ContainerIdOf(IMMDevice* dev) {
    if (!dev) return {};
    IPropertyStore* props = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &props)) || !props) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &v))) {
        if (v.vt == VT_CLSID && v.puuid) {
            wchar_t buf[64] = {};
            if (StringFromGUID2(*v.puuid, buf, ARRAYSIZE(buf)) > 0) out = buf;
        } else if (v.vt == VT_LPWSTR && v.pwszVal) {
            out = v.pwszVal;
        }
    }
    PropVariantClear(&v);
    props->Release();
    return LowerWide(out);
}

std::wstring ProcessImageName(DWORD pid) {
    if (pid == 0) return L"系统声音";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[1024] = {};
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
            CloseHandle(h);
            std::wstring full(path, n);
            const size_t slash = full.find_last_of(L"\\/");
            return slash == std::wstring::npos ? full : full.substr(slash + 1);
        }
        CloseHandle(h);
    }
    return Fmt(L"应用 (PID %lu)", (unsigned long)pid);
}

IAudioEndpointVolume* ActivateVolume(IMMDevice* dev) {
    if (!dev) return nullptr;
    IAudioEndpointVolume* vol = nullptr;
    if (FAILED(dev->Activate(IID_IAudioEndpointVolume, CLSCTX_ALL, nullptr, (void**)&vol))) {
        return nullptr;
    }
    return vol;
}

IMMDevice* FindById(IMMDeviceEnumerator* en, const std::wstring& id) {
    if (!en || id.empty()) return nullptr;
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDevice(id.c_str(), &dev))) return nullptr;
    return dev;
}

IMMDeviceEnumerator* MakeEnumerator() {
    if (!EnsureCom()) return nullptr;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator, (void**)&en))) {
        return nullptr;
    }
    return en;
}

std::wstring LowerWide(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// 去掉括号内容：端点名常写成 “耳机 (G733 LIGHTSPEED)”，去掉括号后仍保留型号。
std::wstring StripParens(const std::wstring& s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'（') {
            ++depth;
            continue;
        }
        if (c == L')' || c == L'）') {
            if (depth > 0) --depth;
            continue;
        }
        if (depth == 0) out.push_back(c);
    }
    return out;
}

bool IsTokenChar(wchar_t c) { return std::iswalnum(c) != 0; }

std::vector<std::wstring> Tokens(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (IsTokenChar(c)) {
            cur.push_back(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int ScoreName(const std::wstring& epLower, const std::wstring& epNoParen,
              const std::wstring& wantLower) {
    if (wantLower.size() < 4) return 0;
    if (epLower.find(wantLower) != std::wstring::npos) {
        return 1000 + (int)wantLower.size();
    }
    if (!epNoParen.empty() && epNoParen.find(wantLower) != std::wstring::npos) {
        return 900 + (int)wantLower.size();
    }
    return 0;
}

}  // namespace

int ClampVolume(int percent) {
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

int ParseVolumePercent(const std::wstring& text, int fallback) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
    if (begin >= end) return fallback;

    long value = 0;
    for (size_t i = begin; i < end; ++i) {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'9') return fallback;
        value = value * 10 + (c - L'0');
        if (value > 1000) value = 1000;  // 防止超长输入溢出；最终仍夹到 100
    }
    return ClampVolume((int)value);
}

int LimitHeadsetVolume(int currentPercent, int requestedPercent,
                       bool allowAboveRecommended) {
    const int requested = ClampVolume(requestedPercent);
    if (allowAboveRecommended) return requested;

    const int current = ClampVolume(currentPercent);
    if (requested <= kRecommendedHeadsetVolume) return requested;
    // 当前已被外部设到推荐值以上：允许降低，但不再继续升高。
    if (current > kRecommendedHeadsetVolume) {
        return requested < current ? requested : current;
    }
    return kRecommendedHeadsetVolume;
}

namespace {

std::vector<Endpoint> ListEndpoints(EDataFlow flow) {
    std::vector<Endpoint> out;
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return out;

    std::wstring defaultId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &def)) && def) {
        defaultId = EndpointIdOf(def);
        def->Release();
    }

    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0;
        if (SUCCEEDED(coll->GetCount(&count))) {
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev)) || !dev) continue;
                Endpoint ep;
                ep.id = EndpointIdOf(dev);
                ep.name = FriendlyNameOf(dev);
                ep.containerId = ContainerIdOf(dev);
                ep.isCapture = flow == eCapture;
                ep.isDefault = !ep.id.empty() && ep.id == defaultId;
                if (IAudioEndpointVolume* vol = ActivateVolume(dev)) {
                    float level = 0.0f;
                    BOOL mute = FALSE;
                    if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&level))) {
                        ep.volume = ClampVolume((int)(level * 100.0f + 0.5f));
                    }
                    if (SUCCEEDED(vol->GetMute(&mute))) ep.mute = mute != FALSE;
                    vol->Release();
                }
                if (!ep.id.empty()) out.push_back(std::move(ep));
                dev->Release();
            }
        }
        coll->Release();
    }
    en->Release();
    return out;
}

}  // namespace

std::vector<Endpoint> ListRenderEndpoints() { return ListEndpoints(eRender); }

std::vector<Endpoint> ListCaptureEndpoints() { return ListEndpoints(eCapture); }

int MatchEndpoint(const std::wstring& deviceName, const std::wstring& product,
                  const std::vector<Endpoint>& endpoints) {
    const std::wstring wants[2] = {LowerWide(deviceName), LowerWide(product)};
    int best = -1;
    int bestScore = 0;

    for (int i = 0; i < (int)endpoints.size(); ++i) {
        const std::wstring epLower = LowerWide(endpoints[i].name);
        const std::wstring epNoParen = LowerWide(StripParens(endpoints[i].name));
        int score = 0;
        for (const auto& w : wants) {
            score = std::max(score, ScoreName(epLower, epNoParen, w));
        }
        if (score == 0) {
            // 型号片段匹配：端点名与设备名共有一个 ≥4 字符的字母数字片段。
            const auto epTokens = Tokens(epLower);
            for (const auto& tok : epTokens) {
                if (tok.size() < 4) continue;
                for (const auto& w : wants) {
                    if (w.find(tok) != std::wstring::npos) {
                        score = std::max(score, (int)tok.size());
                        break;
                    }
                }
            }
        }
        if (score > bestScore || (score == bestScore && score > 0 && endpoints[i].isDefault)) {
            bestScore = score;
            best = i;
        }
    }
    return bestScore > 0 ? best : -1;
}

int ResolveOutputEndpoint(const std::vector<Endpoint>& endpoints,
                          const std::wstring& pinnedId) {
    if (endpoints.empty()) return -1;
    if (!pinnedId.empty()) {
        for (int i = 0; i < (int)endpoints.size(); ++i) {
            if (endpoints[i].id == pinnedId) return i;
        }
    }
    for (int i = 0; i < (int)endpoints.size(); ++i) {
        if (endpoints[i].isDefault) return i;
    }
    return 0;
}

int MatchCaptureEndpoint(const std::wstring& deviceName, const std::wstring& product,
                         const std::wstring& renderContainerId,
                         const std::vector<Endpoint>& captures) {
    const std::wstring container = LowerWide(renderContainerId);
    // 虚拟/占位端点可能共用全零或 0000-0000-ffff-ffffffffffff 容器，
    // 不能据此认定是同一物理设备（否则会把桌面虚拟麦克风匹配到任意耳机）。
    auto usable = [](const std::wstring& id) {
        if (id.empty()) return false;
        std::wstring compact;
        for (wchar_t c : id) {
            if (std::iswxdigit(c)) compact.push_back((wchar_t)std::towlower(c));
        }
        if (compact.empty()) return false;
        if (compact.find_first_not_of(L'0') == std::wstring::npos) return false;
        if (compact == L"0000000000000000ffffffffffffffff") return false;
        return true;
    };
    if (usable(container)) {
        int best = -1;
        for (int i = 0; i < (int)captures.size(); ++i) {
            const std::wstring cap = LowerWide(captures[i].containerId);
            if (!usable(cap) || cap != container) continue;
            if (best < 0 || captures[i].isDefault) best = i;
        }
        // 同一物理容器的采集端点优先；多个时优先系统默认。
        if (best >= 0) return best;
    }
    return MatchEndpoint(deviceName, product, captures);
}

bool SetVolume(const std::wstring& endpointId, int percent) {
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return false;
    bool ok = false;
    if (IMMDevice* dev = FindById(en, endpointId)) {
        if (IAudioEndpointVolume* vol = ActivateVolume(dev)) {
            const float level = (float)ClampVolume(percent) / 100.0f;
            ok = SUCCEEDED(vol->SetMasterVolumeLevelScalar(level, nullptr));
            vol->Release();
        }
        dev->Release();
    }
    en->Release();
    return ok;
}

bool SetMute(const std::wstring& endpointId, bool mute) {
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return false;
    bool ok = false;
    if (IMMDevice* dev = FindById(en, endpointId)) {
        if (IAudioEndpointVolume* vol = ActivateVolume(dev)) {
            ok = SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr));
            vol->Release();
        }
        dev->Release();
    }
    en->Release();
    return ok;
}

bool GetVolume(const std::wstring& endpointId, int* percent, bool* mute) {
    if (percent) *percent = 100;
    if (mute) *mute = false;
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return false;
    bool ok = false;
    if (IMMDevice* dev = FindById(en, endpointId)) {
        if (IAudioEndpointVolume* vol = ActivateVolume(dev)) {
            float level = 0.0f;
            BOOL m = FALSE;
            if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&level))) {
                if (percent) *percent = ClampVolume((int)(level * 100.0f + 0.5f));
                ok = true;
            }
            if (SUCCEEDED(vol->GetMute(&m)) && mute) *mute = m != FALSE;
            vol->Release();
        }
        dev->Release();
    }
    en->Release();
    return ok;
}

namespace {

// 遍历所有活动渲染端点上的 ACTIVE 会话。回调拿到会话对象（仅回调内有效），
// 需要什么接口由调用方自己 QI：设置路径不必读显示名/当前值，省掉属性与读值开销。
template <typename Fn>
void ForEachActiveSession(Fn&& fn) {
    IMMDeviceEnumerator* en = MakeEnumerator();
    if (!en) return;
    IMMDeviceCollection* coll = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll) {
        en->Release();
        return;
    }
    UINT devCount = 0;
    coll->GetCount(&devCount);
    for (UINT d = 0; d < devCount; ++d) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(d, &dev)) || !dev) continue;
        const std::wstring endpointName = FriendlyNameOf(dev);
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
                                    (void**)&mgr)) &&
            mgr) {
            IAudioSessionEnumerator* list = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&list)) && list) {
                int n = 0;
                list->GetCount(&n);
                for (int i = 0; i < n; ++i) {
                    IAudioSessionControl* ctl = nullptr;
                    if (FAILED(list->GetSession(i, &ctl)) || !ctl) continue;
                    IAudioSessionControl2* ctl2 = nullptr;
                    DWORD pid = 0;
                    AudioSessionState state = AudioSessionStateExpired;
                    if (SUCCEEDED(ctl->QueryInterface(IID_IAudioSessionControl2,
                                                     (void**)&ctl2)) &&
                        ctl2) {
                        ctl2->GetProcessId(&pid);
                        ctl2->GetState(&state);
                        ctl2->Release();
                    }
                    if (state == AudioSessionStateActive) fn(pid, ctl, endpointName);
                    ctl->Release();
                }
                list->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    coll->Release();
    en->Release();
}

// 遍历所有活动渲染端点上的活动会话；会话对象在回调返回后释放。
template <typename Fn>
void ForEachAudioSession(Fn&& fn) {
    ForEachActiveSession([&](DWORD pid, IAudioSessionControl* ctl,
                             const std::wstring& endpointName) {
        ISimpleAudioVolume* vol = nullptr;
        if (FAILED(ctl->QueryInterface(IID_ISimpleAudioVolume, (void**)&vol)) || !vol) return;
        float v = 1.0f;
        BOOL m = FALSE;
        vol->GetMasterVolume(&v);
        vol->GetMute(&m);
        std::wstring display;
        LPWSTR dn = nullptr;
        if (SUCCEEDED(ctl->GetDisplayName(&dn)) && dn) {
            display = dn;
            CoTaskMemFree(dn);
        }
        fn(pid, ClampVolume((int)(v * 100.0f + 0.5f)), m != FALSE, display, endpointName, vol);
        vol->Release();
    });
}

// 写入路径专用：只取该 PID 的活动会话并 QI 音量接口。
template <typename Fn>
void ForEachPidVolume(DWORD pid, Fn&& fn) {
    ForEachActiveSession([&](DWORD sessionPid, IAudioSessionControl* ctl,
                             const std::wstring&) {
        if (sessionPid != pid) return;
        ISimpleAudioVolume* vol = nullptr;
        if (FAILED(ctl->QueryInterface(IID_ISimpleAudioVolume, (void**)&vol)) || !vol) return;
        fn(vol);
        vol->Release();
    });
}

// 麦克风采样峰值：混合格式通常是 float32，同时兼容 16/24/32-bit PCM。
float PeakOf(const BYTE* data, UINT32 frames, int channels, int bits, bool isFloat) {
    if (!data || !frames || channels <= 0) return 0.0f;
    const size_t samples = (size_t)frames * (size_t)channels;
    float peak = 0.0f;
    if (isFloat && bits == 32) {
        const float* p = (const float*)data;
        for (size_t i = 0; i < samples; ++i) peak = std::max(peak, std::fabs(p[i]));
    } else if (!isFloat && bits == 16) {
        const int16_t* p = (const int16_t*)data;
        for (size_t i = 0; i < samples; ++i) {
            peak = std::max(peak, (float)std::abs((int)p[i]) / 32768.0f);
        }
    } else if (!isFloat && bits == 32) {
        const int32_t* p = (const int32_t*)data;
        for (size_t i = 0; i < samples; ++i) {
            peak = std::max(peak, (float)(std::abs((double)p[i]) / 2147483648.0));
        }
    } else if (!isFloat && bits == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const BYTE* b = data + i * 3;
            int32_t v = (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 |
                                  (uint32_t)b[2] << 24) >> 8;
            peak = std::max(peak, (float)(std::abs((double)v) / 8388608.0));
        }
    }
    return std::min(peak, 1.0f);
}

}  // namespace

std::vector<AppSession> ListAppSessions() {
    std::map<DWORD, AppSession> byPid;
    ForEachAudioSession([&](DWORD pid, int volume, bool mute, const std::wstring& display,
                            const std::wstring& endpointName, ISimpleAudioVolume*) {
        auto it = byPid.find(pid);
        if (it == byPid.end()) {
            AppSession s;
            s.pid = pid;
            s.name = ProcessImageName(pid);
            s.displayName = display;
            s.endpointName = endpointName;
            s.volume = volume;
            s.mute = mute;
            s.sessionCount = 1;
            byPid.emplace(pid, std::move(s));
        } else {
            ++it->second.sessionCount;
            if (it->second.displayName.empty()) it->second.displayName = display;
            if (!endpointName.empty() &&
                it->second.endpointName.find(endpointName) == std::wstring::npos) {
                if (!it->second.endpointName.empty()) it->second.endpointName += L" / ";
                it->second.endpointName += endpointName;
            }
        }
    });

    std::vector<AppSession> out;
    out.reserve(byPid.size());
    for (auto& kv : byPid) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(), [](const AppSession& a, const AppSession& b) {
        return LowerWide(a.name) < LowerWide(b.name);
    });
    return out;
}

bool SetAppVolume(unsigned long pid, int percent) {
    const float level = (float)ClampVolume(percent) / 100.0f;
    bool ok = false;
    ForEachPidVolume((DWORD)pid, [&](ISimpleAudioVolume* vol) {
        if (SUCCEEDED(vol->SetMasterVolume(level, nullptr))) ok = true;
    });
    return ok;
}

bool SetAppMute(unsigned long pid, bool mute) {
    bool ok = false;
    ForEachPidVolume((DWORD)pid, [&](ISimpleAudioVolume* vol) {
        if (SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr))) ok = true;
    });
    return ok;
}

AppWritePlan PlanAppWrite(int written, bool reopened) {
    if (written > 0) return AppWritePlan::Write;
    return reopened ? AppWritePlan::GiveUp : AppWritePlan::Reopen;
}

AppVolumeWriter::~AppVolumeWriter() { Close(); }

bool AppVolumeWriter::Open(unsigned long pid) {
    Close();
    pid_ = pid;
    if (pid == 0) return false;
    ForEachPidVolume((DWORD)pid, [this](ISimpleAudioVolume* vol) {
        vol->AddRef();  // 回调返回后会话对象被释放，缓存必须自己持有引用
        volumes_.push_back(vol);
    });
    return !volumes_.empty();
}

void AppVolumeWriter::Close() {
    for (ISimpleAudioVolume* vol : volumes_) vol->Release();
    volumes_.clear();
    pid_ = 0;
}

int AppVolumeWriter::SetVolume(int percent) {
    const float level = (float)ClampVolume(percent) / 100.0f;
    int written = 0;
    for (ISimpleAudioVolume* vol : volumes_) {
        if (SUCCEEDED(vol->SetMasterVolume(level, nullptr))) ++written;
    }
    return written;
}

int AppVolumeWriter::SetMute(bool mute) {
    int written = 0;
    for (ISimpleAudioVolume* vol : volumes_) {
        if (SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr))) ++written;
    }
    return written;
}

int AppVolumeWriter::SetVolumeUnmuted(int percent) {
    const float level = (float)ClampVolume(percent) / 100.0f;
    int written = 0;
    for (ISimpleAudioVolume* vol : volumes_) {
        if (SUCCEEDED(vol->SetMasterVolume(level, nullptr))) ++written;
        // 拖动语义就是“解除静音”；静音写失败不改变本次音量写入的判定。
        vol->SetMute(FALSE, nullptr);
    }
    return written;
}

namespace {

// Windows.Media.Internal.AudioPolicyConfig 的静态接口。
// IUnknown[0-2] + IInspectable[3-5] + 19 个未使用方法[6-24] +
// Set[25] / Get[26] / Clear[27]（旧版与 21H2 的槽位一致）。
const IID kPolicyIIDs[] = {
    {0xAB3D4648, 0xE242, 0x459F, {0xB0, 0x2F, 0x54, 0x1C, 0x70, 0x30, 0x63, 0x24}},
    {0x2A59116D, 0x6C4F, 0x45E0, {0xA7, 0x4F, 0x70, 0x7E, 0x3F, 0xEF, 0x92, 0x58}},
    {0x32AA8E18, 0x6496, 0x4E24, {0x9F, 0x94, 0xB8, 0x00, 0xE7, 0xEC, 0xCC, 0x45}},
};

using PolicySetFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT, int, int, HSTRING);
using PolicyGetFn = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, UINT, int, int, HSTRING*);

struct PolicyConfig {
    IUnknown* policy = nullptr;
    bool roInitialized = false;
    bool valid = false;

    PolicyConfig() {
        HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        roInitialized = SUCCEEDED(hr) && hr != S_FALSE;
        HSTRING_HEADER header{};
        HSTRING cls{};
        const wchar_t* name = L"Windows.Media.Internal.AudioPolicyConfig";
        if (FAILED(WindowsCreateStringReference(name, (UINT32)wcslen(name), &header, &cls))) return;
        IUnknown* factory = nullptr;
        if (FAILED(RoGetActivationFactory(cls, IID_IUnknown, (void**)&factory)) || !factory) return;
        for (const IID& iid : kPolicyIIDs) {
            void* p = nullptr;
            if (SUCCEEDED(factory->QueryInterface(iid, &p)) && p) {
                policy = (IUnknown*)p;
                valid = true;
                break;
            }
        }
        factory->Release();
    }

    ~PolicyConfig() {
        if (policy) policy->Release();
        if (roInitialized) RoUninitialize();
    }

    PolicySetFn SetFn() const { return (PolicySetFn)(*(void***)policy)[25]; }
    PolicyGetFn GetFn() const { return (PolicyGetFn)(*(void***)policy)[26]; }
};

}  // namespace

std::wstring PolicyDevicePath(const std::wstring& endpointId, bool capture) {
    if (endpointId.empty()) return {};
    // 策略接口要求 SWD 设备接口路径；参考 PCPanel：
    // \\?\SWD#MMDEVAPI#<IMMDevice id>#{render|capture interface GUID}
    const wchar_t* postfix = capture ? L"#{2eef81be-33fa-4800-9670-1cd474972c3f}"
                                     : L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
    return L"\\\\?\\SWD#MMDEVAPI#" + endpointId + postfix;
}

std::wstring NormalizePolicyDevicePath(const std::wstring& value) {
    std::wstring out = value;
    const std::wstring prefix = L"\\\\?\\SWD#MMDEVAPI#";
    if (out.rfind(prefix, 0) == 0) out.erase(0, prefix.size());
    const size_t hash = out.find_last_of(L'#');
    if (hash != std::wstring::npos) out.resize(hash);
    return out;
}

bool SetAppEndpoint(unsigned long pid, bool capture, const std::wstring& endpointId) {
    if (pid == 0) return false;
    PolicyConfig cfg;
    if (!cfg.valid) return false;
    HSTRING device = nullptr;
    if (!endpointId.empty()) {
        const std::wstring full = PolicyDevicePath(endpointId, capture);
        if (FAILED(WindowsCreateString(full.c_str(), (UINT32)full.size(), &device))) return false;
    }
    const int flow = capture ? eCapture : eRender;
    const int roles[] = {eConsole, eMultimedia, eCommunications};
    bool ok = false;
    for (int role : roles) {
        if (SUCCEEDED(cfg.SetFn()(cfg.policy, (UINT)pid, flow, role, device))) ok = true;
    }
    if (device) WindowsDeleteString(device);
    return ok;
}

bool GetAppEndpoint(unsigned long pid, bool capture, std::wstring& endpointId) {
    endpointId.clear();
    if (pid == 0) return false;
    PolicyConfig cfg;
    if (!cfg.valid) return false;
    const int flow = capture ? eCapture : eRender;
    const int roles[] = {eConsole, eMultimedia, eCommunications};
    bool queried = false;
    for (int role : roles) {
        HSTRING device = nullptr;
        HRESULT hr = cfg.GetFn()(cfg.policy, (UINT)pid, flow, role, &device);
        if (hr == 0x80070057) continue;  // PROCESS_NO_AUDIO：该进程没有该方向的会话
        if (SUCCEEDED(hr)) {
            queried = true;
            if (device) {
                UINT32 len = 0;
                const wchar_t* raw = WindowsGetStringRawBuffer(device, &len);
                std::wstring value = raw ? std::wstring(raw, len) : std::wstring();
                WindowsDeleteString(device);
                // 返回值是 SWD 路径；还原成 IMMDevice 的 {0.0.0...} id 供 UI 匹配。
                value = NormalizePolicyDevicePath(value);
                if (!value.empty()) {
                    endpointId = std::move(value);
                    return true;
                }
                continue;
            }
        }
        if (device) WindowsDeleteString(device);
    }
    return queried;
}

struct MicMonitor::Impl {
    std::atomic<bool> stop{true};
    std::atomic<bool> running{false};
    std::atomic<float> level{0.0f};
    mutable std::mutex errMutex;
    std::wstring error;
    std::thread thread;

    ~Impl() { Stop(); }

    void SetError(const std::wstring& text) {
        std::lock_guard<std::mutex> lock(errMutex);
        error = text;
    }
    std::wstring Error() const {
        std::lock_guard<std::mutex> lock(errMutex);
        return error;
    }

    void Stop() {
        stop.store(true);
        if (thread.joinable()) thread.join();
        running.store(false);
        level.store(0.0f);
    }

    void Start(std::wstring id) {
        Stop();
        {
            std::lock_guard<std::mutex> lock(errMutex);
            error.clear();
        }
        stop.store(false);
        level.store(0.0f);
        thread = std::thread([this, id = std::move(id)] { Run(id); });
    }

    void Run(std::wstring id) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* mix = nullptr;
        bool started = false;
        int channels = 0;
        int bits = 0;
        bool isFloat = false;

        auto fail = [&](const std::wstring& text) {
            SetError(text);
            running.store(false);
        };

        if (!com) {
            fail(L"无法初始化麦克风线程");
            return;
        }
        if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                    IID_IMMDeviceEnumerator, (void**)&en)) ||
            !en) {
            fail(L"无法枚举音频设备");
        } else if (FAILED(en->GetDevice(id.c_str(), &dev)) || !dev) {
            fail(L"找不到麦克风端点");
        } else if (FAILED(dev->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                                        (void**)&client)) ||
                   !client) {
            fail(L"无法打开麦克风");
        } else if (FAILED(client->GetMixFormat(&mix)) || !mix) {
            fail(L"无法读取麦克风格式");
        } else {
            channels = mix->nChannels;
            bits = mix->wBitsPerSample;
            isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
                auto* ext = (WAVEFORMATEXTENSIBLE*)mix;
                // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 的 Data1 = 3。
                isFloat = ext->SubFormat.Data1 == 3;
            }
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr);
            if (FAILED(hr)) {
                if (hr == E_ACCESSDENIED) {
                    fail(L"麦克风被 Windows 隐私设置阻止");
                } else if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    fail(L"麦克风已断开");
                } else {
                    fail(Fmt(L"麦克风初始化失败 (0x%08lX)", (unsigned long)hr));
                }
            } else if (FAILED(client->GetService(IID_IAudioCaptureClient,
                                                 (void**)&capture)) ||
                       !capture) {
                fail(L"无法读取麦克风数据");
            } else if (FAILED(client->Start())) {
                fail(L"麦克风启动失败");
            } else {
                started = true;
                running.store(true);
            }
        }

        if (started) {
            while (!stop.load()) {
                UINT32 packet = 0;
                if (FAILED(capture->GetNextPacketSize(&packet))) break;
                float peak = 0.0f;
                while (packet > 0 && !stop.load()) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                        break;
                    }
                    if (frames && data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        peak = std::max(peak, PeakOf(data, frames, channels, bits, isFloat));
                    }
                    capture->ReleaseBuffer(frames);
                    if (FAILED(capture->GetNextPacketSize(&packet))) break;
                }
                // 峰值保持 + 缓慢回落，UI 的柱状图不会闪烁。
                const float old = level.load();
                level.store(std::max(peak, old * 0.82f));
                Sleep(20);
            }
            // 不是用户主动停止却退出循环：设备被拔掉/隐私策略变化。
            if (!stop.load()) SetError(L"麦克风已断开或停止输出");
        }

        if (client && started) client->Stop();
        if (capture) capture->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        if (mix) CoTaskMemFree(mix);
        if (com) CoUninitialize();
        level.store(0.0f);
        running.store(false);
    }
};

MicMonitor::MicMonitor() : impl_(new Impl()) {}
MicMonitor::~MicMonitor() {
    delete impl_;
    impl_ = nullptr;
}

bool MicMonitor::Start(const std::wstring& captureEndpointId) {
    if (captureEndpointId.empty()) {
        impl_->SetError(L"没有可用的麦克风端点");
        return false;
    }
    impl_->Start(captureEndpointId);
    return true;
}

void MicMonitor::Stop() { impl_->Stop(); }
bool MicMonitor::running() const { return impl_->running.load(); }
float MicMonitor::Level() const { return impl_->level.load(); }
std::wstring MicMonitor::error() const { return impl_->Error(); }

bool OpenSoundSettings() {
    // Windows 10/11 的“声音”设置页；旧系统退回控制面板的声音属性。
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr,
                                SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) return true;
    r = ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl,,0", nullptr,
                      SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

bool OpenAppVolumeSettings() {
    // Windows 11 的每应用音量/输出设备在“设置 → 系统 → 声音 → 音量混合器”，
    // 没有独立 ms-settings URI；打开声音页后由用户在系统 UI 中调整。
    return OpenSoundSettings();
}

}  // namespace audio
