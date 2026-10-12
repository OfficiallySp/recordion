# Recordion

A lightweight game and desktop recorder for Windows 11, in the spirit of ShadowPlay and Radeon ReLive.

- **Instant replay**: always-on buffer of the last X seconds/minutes, saved with a hotkey
- **Manual recording** to crash-safe "hybrid" MP4 (fragmented while recording, ordinary MP4 when done)
- **Automatic game detection** (Steam, Epic, GOG, Ubisoft, Xbox libraries and Windows' game list,
  plus fullscreen/GPU behaviour); clips go into a folder named after the game
- **Audio**: game-only (per-process) or system audio, optional microphone, optional separate tracks
- **Hardware encoding** with H.264, HEVC and AV1 (8/10-bit) on AMD AMF, NVIDIA NVENC and Intel Quick Sync
- **Zero-copy pipeline**: captured frames never touch the CPU; on AMD the encoder's fixed-function
  colour converter takes the captured image directly
- **Organised output**: `Videos\Recordion\<Game>\<Game> 2026-10-11 21-34-07 Replay.mp4`
- **Rebindable global hotkeys**, no process injection (anti-cheat safe)

Status: instant replay, recording, audio, game detection and the settings window work and are
tested on AMD (AMF). The NVENC and Quick Sync backends are implemented but not yet tested on
NVIDIA/Intel hardware.

## Build

Requirements: Windows 11 24H2+, Visual Studio 2026 Build Tools (C++ workload), CMake 3.28+, Ninja,
and [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
scripts\build.cmd release test
```

The executable is `build\release\recordion.exe`.

## Use

Run `recordion.exe`; it lives in the system tray.

| Hotkey | Action |
|---|---|
| Alt+Shift+F10 | Instant replay on/off (games turn it on automatically) |
| Alt+F10 | Save the replay |
| Alt+F9 | Start/stop recording |

Hotkeys, codec, bitrate, replay length and the output folder are in
`%APPDATA%\Recordion\settings.json` (tray menu: *Edit settings file*, then *Reload settings*).

## Benchmarks

`recordion --bench` runs the capture/encode pipeline headlessly and prints CPU and GPU cost:

```
recordion --bench --synthetic --seconds 10 --codec hevc --bitrate 20000
recordion --bench --session --synthetic --seconds 10 --replay 5 --out-dir C:\temp\clips
```

To measure what Recordion costs a game (average fps, 1% lows, present mode, with Recordion off and
then on), run from an elevated terminal while the game is running:

```
powershell -ExecutionPolicy Bypass -File scripts\measure-overhead.ps1 -Process Haste.exe
```
