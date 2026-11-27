#include <cassert>
#include <iostream>
#include <vector>

#include "audio.h"

namespace {

int g_checks = 0;

#define CHECK(expr)     \
    do {                \
        ++g_checks;     \
        assert(expr);   \
    } while (0)

audio::Endpoint MakeEp(const wchar_t* name, bool isDefault = false,
                       const wchar_t* container = nullptr) {
    audio::Endpoint e;
    e.id = name;
    e.name = name;
    e.isDefault = isDefault;
    if (container) e.containerId = container;
    return e;
}

void TestClampVolume() {
    CHECK(audio::ClampVolume(-1) == 0);
    CHECK(audio::ClampVolume(0) == 0);
    CHECK(audio::ClampVolume(43) == 43);
    CHECK(audio::ClampVolume(100) == 100);
    CHECK(audio::ClampVolume(101) == 100);
}

void TestParseVolumePercent() {
    CHECK(audio::ParseVolumePercent(L"0") == 0);
    CHECK(audio::ParseVolumePercent(L"37") == 37);
    CHECK(audio::ParseVolumePercent(L"100") == 100);
    CHECK(audio::ParseVolumePercent(L"150") == 100);  // 越界夹取
    CHECK(audio::ParseVolumePercent(L" 42 ") == 42);  // 首尾空格
    CHECK(audio::ParseVolumePercent(L"") == -1);      // 空串用 fallback
    CHECK(audio::ParseVolumePercent(L"abc") == -1);
    CHECK(audio::ParseVolumePercent(L"-5") == -1);
    CHECK(audio::ParseVolumePercent(L"12x") == -1);
    CHECK(audio::ParseVolumePercent(L"", 77) == 77);
}

void TestExactAndParenMatch() {
    std::vector<audio::Endpoint> eps{
        MakeEp(L"Speakers (Realtek(R) Audio)"),
        MakeEp(L"耳机 (G733 LIGHTSPEED)"),
    };
    CHECK(audio::MatchEndpoint(L"G733 LIGHTSPEED", L"G733 LIGHTSPEED", eps) == 1);
    // 只有产品名匹配也要能找到
    CHECK(audio::MatchEndpoint(L"", L"G733 LIGHTSPEED", eps) == 1);
    // 大小写不敏感
    CHECK(audio::MatchEndpoint(L"g733 lightSpeed", L"", eps) == 1);
}

void TestTokenMatch() {
    std::vector<audio::Endpoint> eps{
        MakeEp(L"Speakers (Realtek(R) Audio)"),
        MakeEp(L"Headset Earphone (Arctis Nova Pro)"),
    };
    // 完整名字不同，但共有型号片段 "arctis"
    CHECK(audio::MatchEndpoint(L"Arctis Nova Pro Wireless", L"", eps) == 1);
}

void TestNoMatch() {
    std::vector<audio::Endpoint> eps{MakeEp(L"耳机 (G733 LIGHTSPEED)")};
    CHECK(audio::MatchEndpoint(L"PRO X SUPERLIGHT 2", L"", eps) == -1);
    CHECK(audio::MatchEndpoint(L"", L"", eps) == -1);
    // 太短的词不参与匹配，避免误命中
    std::vector<audio::Endpoint> eps2{MakeEp(L"耳机 (USB Audio)")};
    CHECK(audio::MatchEndpoint(L"USB", L"", eps2) == -1);
}

void TestDefaultPreferredOnTie() {
    std::vector<audio::Endpoint> eps{
        MakeEp(L"耳机 (G733 LIGHTSPEED)"),
        MakeEp(L"耳机 (G733 LIGHTSPEED)", true),
    };
    // 分数相同（同一名字）时优先默认端点
    CHECK(audio::MatchEndpoint(L"G733 LIGHTSPEED", L"", eps) == 1);
}

void TestBetterScoreWins() {
    std::vector<audio::Endpoint> eps{
        MakeEp(L"耳机 (G733)"),
        MakeEp(L"耳机 (G733 LIGHTSPEED)"),
    };
    // 更长的完整匹配优先
    CHECK(audio::MatchEndpoint(L"G733 LIGHTSPEED", L"", eps) == 1);
}

// AudioPolicyConfig 的设备 ID 必须是 SWD 路径；错误格式会让 Set 直接失败。
void TestPolicyDevicePath() {
    const std::wstring id = L"{0.0.0.00000000}.{183c43ed-b4c8-4709-a04d-8909b0d424db}";
    const std::wstring prefix = L"\\\\?\\SWD#MMDEVAPI#";
    CHECK(audio::PolicyDevicePath(id, false) ==
          prefix + id + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}");
    CHECK(audio::PolicyDevicePath(id, true) ==
          prefix + id + L"#{2eef81be-33fa-4800-9670-1cd474972c3f}");
    CHECK(audio::PolicyDevicePath(L"", false).empty());
    CHECK(audio::NormalizePolicyDevicePath(audio::PolicyDevicePath(id, false)) == id);
    CHECK(audio::NormalizePolicyDevicePath(id) == id);
    CHECK(audio::NormalizePolicyDevicePath(L"").empty());
}
void TestCaptureContainerWins() {
    std::vector<audio::Endpoint> caps{
        MakeEp(L"Microphone (USB Audio)", false, L"{11111111-1111-1111-1111-111111111111}"),
        MakeEp(L"Headset Microphone (G733 LIGHTSPEED)", false,
               L"{22222222-2222-2222-2222-222222222222}"),
    };
    CHECK(audio::MatchCaptureEndpoint(L"G733 LIGHTSPEED", L"", L"{22222222-2222-2222-2222-222222222222}",
                                      caps) == 1);
    // 容器 id 大小写归一化后仍命中
    CHECK(audio::MatchCaptureEndpoint(L"G733 LIGHTSPEED", L"",
                                      L"{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}",
                                      {MakeEp(L"Mic A", false, L"{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}"),
                                       MakeEp(L"Mic B", false, L"{bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb}")}) == 0);
}

// 容器缺失时退回名称/型号片段匹配；默认端点同分优先。
void TestCaptureFallsBackToName() {
    std::vector<audio::Endpoint> caps{
        MakeEp(L"Microphone Array (Realtek(R) Audio)"),
        MakeEp(L"Headset Microphone (Arctis Nova Pro)", true),
    };
    CHECK(audio::MatchCaptureEndpoint(L"Arctis Nova Pro Wireless", L"", L"", caps) == 1);
    std::vector<audio::Endpoint> dup{
        MakeEp(L"Headset Microphone (G733 LIGHTSPEED)"),
        MakeEp(L"Headset Microphone (G733 LIGHTSPEED)", true),
    };
    CHECK(audio::MatchCaptureEndpoint(L"G733 LIGHTSPEED", L"", L"", dup) == 1);
    CHECK(audio::MatchCaptureEndpoint(L"PRO X SUPERLIGHT 2", L"", L"", caps) == -1);
    // 全零容器 GUID 是占位值，不能把虚拟麦克风误认为同一设备。
    std::vector<audio::Endpoint> virt{
        MakeEp(L"Microphone (ToDesk Virtual Audio)", false,
               L"{00000000-0000-0000-0000-000000000000}")};
    CHECK(audio::MatchCaptureEndpoint(L"G733 LIGHTSPEED", L"",
                                      L"{00000000-0000-0000-0000-000000000000}", virt) == -1);
    // Windows 音频栈还会用 0000-0000-ffff-ffffffffffff 作为占位容器。
    std::vector<audio::Endpoint> sentinel{
        MakeEp(L"麦克风 (ToDesk Virtual Audio)", true,
               L"{00000000-0000-0000-ffff-ffffffffffff}")};
    CHECK(audio::MatchCaptureEndpoint(L"G733 LIGHTSPEED", L"",
                                      L"{00000000-0000-0000-ffff-ffffffffffff}", sentinel) == -1);
}

// 系统输出目标端点：固定端点有效时优先，否则默认端点，再否则第一个。
void TestResolveOutputEndpoint() {
    std::vector<audio::Endpoint> eps{
        MakeEp(L"扬声器 (Realtek(R) Audio)"),
        MakeEp(L"耳机 (Buds4 Pro)", true),
        MakeEp(L"数字输出 (HDMI)"),
    };
    CHECK(audio::ResolveOutputEndpoint(eps, L"") == 1);
    CHECK(audio::ResolveOutputEndpoint(eps, L"扬声器 (Realtek(R) Audio)") == 0);
    CHECK(audio::ResolveOutputEndpoint(eps, L"missing") == 1);

    std::vector<audio::Endpoint> noDefault{MakeEp(L"A"), MakeEp(L"B")};
    CHECK(audio::ResolveOutputEndpoint(noDefault, L"") == 0);
    CHECK(audio::ResolveOutputEndpoint({}, L"x") == -1);
}

// 耳机推荐音量：未同意时 >60 夹到 60；已 >60 时不允许继续升高，但允许降低。
void TestLimitHeadsetVolume() {
    CHECK(audio::LimitHeadsetVolume(57, 80, false) == 60);
    CHECK(audio::LimitHeadsetVolume(57, 60, false) == 60);
    CHECK(audio::LimitHeadsetVolume(0, 100, false) == 60);
    CHECK(audio::LimitHeadsetVolume(80, 90, false) == 80);
    CHECK(audio::LimitHeadsetVolume(80, 70, false) == 70);
    CHECK(audio::LimitHeadsetVolume(80, 50, false) == 50);
    CHECK(audio::LimitHeadsetVolume(80, 90, true) == 90);
    CHECK(audio::LimitHeadsetVolume(57, 80, true) == 80);
    CHECK(audio::LimitHeadsetVolume(57, 150, true) == 100);
}

// B03：应用音量拖动的写入失败策略——成功就继续写；一个都没写成先重开一次缓存；
// 重开过仍失败就放弃，绝不能退化成“每条鼠标消息枚举一遍”。
void TestPlanAppWrite() {
    CHECK(audio::PlanAppWrite(1, false) == audio::AppWritePlan::Write);
    CHECK(audio::PlanAppWrite(3, true) == audio::AppWritePlan::Write);
    CHECK(audio::PlanAppWrite(0, false) == audio::AppWritePlan::Reopen);
    CHECK(audio::PlanAppWrite(0, true) == audio::AppWritePlan::GiveUp);
    // 负值不可能来自实现，但策略上等同于“没写成”
    CHECK(audio::PlanAppWrite(-1, false) == audio::AppWritePlan::Reopen);
}

}  // namespace

int main() {
    TestClampVolume();
    TestParseVolumePercent();
    TestExactAndParenMatch();
    TestTokenMatch();
    TestNoMatch();
    TestDefaultPreferredOnTie();
    TestBetterScoreWins();
    TestPolicyDevicePath();
    TestCaptureContainerWins();
    TestCaptureFallsBackToName();
    TestResolveOutputEndpoint();
    TestLimitHeadsetVolume();
    TestPlanAppWrite();
    std::cout << "audio_match: 13 tests, " << g_checks << " checks passed\n";
    return 0;
}
