# 2.4G Device Center

[中文](README.md) | English

A tiny native Windows utility for 2.4 GHz wireless peripherals. It talks to vendor HID interfaces
directly and shows battery, charging state, DPI, polling rate and key settings. No WebView, no
Electron, no .NET/Python runtime: a single ~400 KB x64 executable built with C++20 and Win32/GDI+.

![Screenshot](docs/screenshot.png)

## Features

- Native Win32 UI with large circular battery gauges; device list and tray icon also show battery.
- Tray icon is a high-DPI circular badge with a large percentage and a colored level arc.
- GDI+ vector device icons for mouse/keyboard/headset/generic, anti-aliased at any DPI.
- DPI slider uses a logarithmic scale; presets/stages are clickable.
- Background polling keeps the UI responsive; sleeping devices show the last reading or `--`.
- Tray menu: open, refresh, start with Windows, open log, exit.
- Demo mode: `--demo`; diagnostics: `--once` and `--diag`.
- No macros, lighting or firmware operations. No macro editor.

## Supported protocols

| Protocol / brand | Battery | DPI | Polling | Other settings |
| --- | --- | --- | --- | --- |
| Logitech HID++ 2.0 | ✅ | ✅ R/W | ✅ R/W | receiver pairing, names, onboard stages |
| ASUS ROG private HID | ✅ | ✅ 4 stages R/W | ✅ R/W | debounce, angle snapping, save |
| Razer HID | ✅ | ✅ R/W | ✅ R/W | up to 8000 Hz where supported |
| Rapoo 2.4G | ✅ | ✅ 7 stages R/W | ✅ R/W | debounce, angle snapping |
| Compx family | ✅ | partial: Zaopin enabled stages R/W | ❌ | Zaopin / LAMZU / VXE / Redragon / G-Wolves / Endgame |
| Nordic 54 | ✅ | ❌ | ❌ | ATK/VXE receivers |
| Aurora / mouse.xyz | ✅ | ❌ | ❌ | LAMZU / G-Wolves web-driver devices |
| Cherry HID | ✅ | ❌ | ❌ | wireless keyboards |
| Lofree WebHID | ✅ | ❌ | ❌ | Hyzen keyboards |
| Windows HID battery | ✅ | ❌ | ❌ | generic fallback |

### Zaopin Z4D (tested locally)

- `VID_3554 / PID_F527`, product `Z4D`, Compx `0xFF02` 17-byte config pipe.
- Verified read-only: 50 %, 3883 mV, current stage 0 → 3100 DPI, 4000 Hz polling, 8 ms debounce.
- Flash layout: DPI records at `0x0C + 4*i` (`x, y, DPIex, checksum`), current stage at `0x04`,
  `maxDPI` at `0x02`, polling bitmask at `0x00`, debounce at `0xA9`.
- DPI decode: `code = x | (((DPIex >> 6) & 3) << 8)`, `unit = (DPIex & 0x22) ? 100 : 50`,
  `DPI = (code + 1) * unit`, `* 2` if `DPIex & 0x11`.
- **DPI write for Zaopin**: enabled stages and the current stage can be written. The app first
  writes the old record back as a self-test, then writes the new record and verifies it by reading
  it back; on failure it attempts to restore the old value. A confirmation dialog is shown before
  writing. Polling/debounce and macros/lighting remain read-only; use `Mouse Drive Beta` to add
  stages or change other settings.

## Quick start

Download `2.4G-Device-Center-v1.0.1-win64.zip` from
[Releases](https://github.com/SeyanaA/2.4g-device-center/releases/latest), extract and run
`WirelessCenter.exe`.

```text
WirelessCenter.exe            start UI
WirelessCenter.exe --demo     demo devices, no hardware
WirelessCenter.exe --tray     start minimized to tray
WirelessCenter.exe --once     enumerate and print devices, then exit
WirelessCenter.exe --diag     dump HID interfaces for debugging
```

`--once` and `--diag` also write `%APPDATA%\2.4GCenter\last-console.txt`. Logs are in
`%APPDATA%\2.4GCenter\log.txt`.

Notes:

- A sleeping wireless mouse often does not answer until you move it. The app polls every 5 s.
- Close vendor software (Logitech G HUB, Razer Synapse, Zaopin Mouse Drive, etc.) if it holds the
  HID interface exclusively.
- Automatic polling only sends read/status requests. Writes happen only after you press
  **Apply settings** and confirm the Zaopin flash warning.
- Zaopin flash writes are limited to enabled DPI stages/current stage and are read-back verified.

## Build

Requires MinGW-w64 (GCC 13+) or w64devkit with `g++` and `windres`.

```powershell
$env:W64DEVKIT = "C:\path\to\w64devkit"
.\build.ps1
```

or with MinGW in `PATH`:

```powershell
.\build.ps1 -Compiler "C:\msys64\mingw64\bin\g++.exe"
```

Makefile:

```powershell
mingw32-make -f Makefile
```

The output is `WirelessCenter.exe` (x64, statically linked, no MinGW/VC++ runtime DLLs).
Only system DLLs are imported: kernel32, user32, gdi32, gdiplus, shell32, advapi32, hid,
setupapi, comctl32, ole32.

## Protocol notes

See [PROTOCOLS.md](PROTOCOLS.md) for report layouts, checksums, feature IDs and references to
Solaar, libratbag, OpenRazer, Rapoo Linux, VXE/LAMZU/G-Wolves reverse-engineering projects.

## Safety

- Automatic queries: status, battery, version, flash reads (read-only).
- Writes: only on explicit **Apply settings** for Logitech/ASUS/Razer/Rapoo.
- No lighting, macros, firmware or bootloader commands.
- Zaopin/Compx/Nordic/Aurora/Cherry/Lofree are display-only.
- No admin rights required; the only optional registry write is the per-user autostart entry.

## License

MIT. Brand names and trademarks belong to their respective owners. This project is not affiliated
with any vendor.
