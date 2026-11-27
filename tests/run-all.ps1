param(
    # 仓库根目录；默认取本脚本的上一级（tests/ 的父目录）。
    [string]$Root = ""
)

# 编译并运行全部测试套件。用法（仓库根目录）：
#   pwsh -NoProfile -File .\tests\run-all.ps1
# 输出：每个套件的 Build/Run/Exit 与通过行；日志写到 $env:TEMP\zc_tests\。
$ErrorActionPreference = 'Continue'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }

$g   = Join-Path $Root 'tools\w64devkit\bin\g++.exe'
$src = Join-Path $Root 'src'
$tst = Join-Path $Root 'tests'
if (-not (Test-Path $g)) { throw "g++ not found: $g" }

$common = @('-std=c++20', '-m64', '-I', $src)

$suites = @(
    @{ Name='test_device_merge';   Files=@('test_device_merge.cpp','src\device_merge.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32') },
    @{ Name='test_battery_notify'; Files=@('test_battery_notify.cpp','src\battery_notify.cpp','src\device_merge.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32') },
    @{ Name='test_compx';          Files=@('test_compx.cpp','src\proto_compx.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32') },
    @{ Name='test_event_log';      Files=@('test_event_log.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32') },
    @{ Name='test_event_history';  Files=@('test_event_history.cpp','src\event_log.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32') },
    @{ Name='test_valkyrie';       Files=@('test_valkyrie.cpp','src\proto_valkyrie.cpp','src\protocols.cpp','src\hid.cpp','src\samsung_buds.cpp','src\io_cancel.cpp','src\common.cpp'); LIBS=@('-lhid','-lsetupapi','-lbthprops','-lbluetoothapis','-lws2_32','-lshell32','-lole32','-luuid','-lcfgmgr32') },
    @{ Name='test_morebetter';     Files=@('test_morebetter.cpp','src\proto_morebetter.cpp','src\io_cancel.cpp','src\common.cpp'); LIBS=@('-lhid','-lshell32','-lole32') },
    @{ Name='test_wired_devices';  Files=@('test_wired_devices.cpp','src\proto_wired.cpp','src\protocols.cpp','src\hid.cpp','src\samsung_buds.cpp','src\io_cancel.cpp','src\common.cpp'); LIBS=@('-lhid','-lsetupapi','-lbthprops','-lbluetoothapis','-lws2_32','-lshell32','-lole32','-luuid','-lcfgmgr32') },
    @{ Name='test_hid_bus';        Files=@('test_hid_bus.cpp','src\hid.cpp','src\samsung_buds.cpp','src\io_cancel.cpp','src\common.cpp'); LIBS=@('-lhid','-lsetupapi','-lbthprops','-lbluetoothapis','-lws2_32','-lshell32','-lole32','-luuid','-lcfgmgr32') },
    @{ Name='test_system_battery'; Files=@('test_system_battery.cpp','src\proto_generic.cpp','src\proto_compx.cpp','src\hid.cpp','src\samsung_buds.cpp','src\io_cancel.cpp','src\common.cpp'); LIBS=@('-lhid','-lsetupapi','-lbthprops','-lbluetoothapis','-lws2_32','-lshell32','-lole32','-luuid','-lcfgmgr32') },
    @{ Name='test_mouse_test';     Files=@('test_mouse_test.cpp','src\mouse_test.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32','-luser32') },
    @{ Name='test_keyboard_test';  Files=@('test_keyboard_test.cpp','src\keyboard_test.cpp','src\common.cpp'); LIBS=@('-lshell32','-lole32','-luser32') },
    @{ Name='test_animator';       Files=@('test_animator.cpp','src\ui_anim.cpp'); LIBS=@('-luser32') },
    @{ Name='test_ui_keyboard';    Files=@('test_ui_keyboard.cpp','src\ui_keyboard.cpp'); LIBS=@() },
    @{ Name='test_paint_region';   Files=@('test_paint_region.cpp','src\paint_region.cpp'); LIBS=@() },
    @{ Name='test_io_cancel';      Files=@('test_io_cancel.cpp','src\io_cancel.cpp'); LIBS=@() },
    @{ Name='test_svg_icon';       Files=@('test_svg_icon.cpp','src\svg_icon.cpp'); LIBS=@('-lgdiplus','-lgdi32','-luser32') },
    @{ Name='test_audio_match';    Files=@('test_audio_match.cpp','src\audio.cpp','src\common.cpp'); LIBS=@('-lole32','-luuid','-lshell32','-luser32','-lruntimeobject') }
)

$logDir = Join-Path $env:TEMP 'zc_tests'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$rows = @()
foreach ($s in $suites) {
    $exe = Join-Path $tst ($s.Name + '.exe')
    $paths = $s.Files | ForEach-Object {
        if ($_ -like 'src\*') { Join-Path $Root $_ } else { Join-Path $tst $_ }
    }
    $outFile = Join-Path $logDir ($s.Name + '.build.log')
    & $g @($common + $paths + @('-o', $exe, '-static') + $s.LIBS) *> $outFile
    $buildRc = $LASTEXITCODE
    if ($buildRc -ne 0) {
        $rows += [pscustomobject]@{ Suite=$s.Name; Build='FAIL'; Run='-'; Exit=$buildRc; Head='(build error)' }
        continue
    }
    $warnCount = (Select-String -Path $outFile -Pattern 'warning:' -SimpleMatch -ErrorAction SilentlyContinue | Measure-Object).Count
    $runOut = & $exe 2>&1 | Out-String
    $runRc = $LASTEXITCODE
    $runOut | Set-Content -Path (Join-Path $logDir ($s.Name + '.run.log')) -Encoding utf8
    $lines = @(($runOut -split "`r?`n") | Where-Object { $_.Trim() -ne '' })
    $head = if ($lines.Count -gt 0) { $lines[-1].Trim() } else { '(no output)' }
    if ($head.Length -gt 100) { $head = $head.Substring(0, 100) }
    $rows += [pscustomobject]@{
        Suite = $s.Name
        Build = if ($warnCount -gt 0) { "OK($warnCount warn)" } else { 'OK' }
        Run   = if ($runRc -eq 0) { 'PASS' } else { 'FAIL' }
        Exit  = $runRc
        Head  = $head
    }
}

$rows | Format-Table -AutoSize | Out-String -Width 220
$fail = ($rows | Where-Object { $_.Run -ne 'PASS' }).Count
Write-Host ""
Write-Host ("SUITES: {0}  FAILED: {1}  LOGS: {2}" -f $rows.Count, $fail, $logDir)
if ($fail -gt 0) { exit 1 }
