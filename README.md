# BlinkDash

SKSE plugin for Skyrim SE/AE (built against the Address Library, single binary for SE + AE) that adds a
blink/dash teleport on a hotkey, with Havok collision detection and an in-game settings menu via
SKSE Menu Framework. No animations, no VFX, no `.esp`, no Papyrus.

This is the **C++ SKSE plugin only**. The old Papyrus/PEX/Mutagen attempt has been deleted.

## Features

- **Blink hotkey** (default: **Mouse 4**). Teleports you in your facing direction, up to a configurable distance.
- **Collision detection**: a Havok raycast finds the first wall/obstacle in the blink path and stops you just
  short of it (clearance), so you do not clip into geometry.
- **Ground snap**: a downward ray puts you on the floor instead of mid-air.
- **Two direction modes**: Facing / movement (default, horizontal only) or Camera aim.
- **Cooldown** and optional **stamina cost**.
- **SKSE Menu Framework tab** ("Blink Dash"): enable, hotkey rebind, mode, distance, cooldown, clearance,
  min distance, ground snap, stamina cost, Save, **Test blink**, Reset defaults.
- Settings persist to `SKSE/Plugins/BlinkDash.json` next to the DLL (inside the mod folder under MO2).
- Works without SKSE Menu Framework installed (menu is skipped, hotkey still works).

## Logging

Every single step of the plugin is logged at **trace** level and the log is **flushed after every line**,
so the last line in the file is the last line that actually ran before a crash.

Log file:

```
C:\Users\<you>\Documents\My Games\Skyrim Special Edition\SKSE\BlinkDash.log
```

What is in there:

- load banner: DLL path, game exe path, process/thread id, plugin version
- every SKSE message type received (`kPostLoad`, `kDataLoaded`, ...)
- config load/save: full path, raw file contents, every parsed field, every clamp
- input: every keyboard/mouse down event (first 400), then every hotkey match
- menu: every widget interaction, hotkey capture
- blink: every gate check, every raycast (from/to/filter/hit/fraction/point), ground probe,
  the teleport, the position delta afterwards
- all exceptions, caught and logged

Thread ids are logged on every step, since a blink is queued from the input (BSJobs) thread and
executed on a game thread.

## Requirements

- Skyrim SE / AE, currently running **1.6.1170**
- [SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- Optional: [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) for the settings UI

## Install

The build copies `BlinkDash.dll` + `BlinkDash.json` into `C:\modding\MO2\mods\BlinkDash\SKSE\Plugins\`.
Enable **BlinkDash** in MO2 (left pane) and make sure the mod is ticked.

## Building from source

Requires VS 2022/18 with C++ tools, CMake + Ninja, and the vcpkg checkout in `.vcpkg`:

```powershell
$env:VCPKG_ROOT = "E:\dev\vibe\skyrim-blink\.vcpkg"
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build E:\dev\vibe\skyrim-blink\build\vs2022
```

(Re-run configure with `--preset vs2022` only if `CMakeLists.txt` changed in a way that needs it.)

## Default settings

| Setting | Default |
|---|---|
| Hotkey | Mouse 4 (`0x103`) |
| Mode | Facing / movement |
| Distance | 800 units (~11 m) |
| Cooldown | 0.5 s |
| Clearance | 40 units |
| Min distance | 150 units |
| Ground snap | on |
| Stamina cost | 0 |

Hotkey encoding: `0x100 | n` = mouse button n (0=LMB, 1=RMB, 2=MMB, 3=MB4...), otherwise a DirectInput
keyboard scancode.

## Known crash (fixed by removal)

The previously shipped build crashed the first time the hotkey was pressed. CrashLogger showed:

```
Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at 0x000000000000
	[ 0][P] 0x7FF97FC1D821 BlinkDash.dll+001D821
	[ 1][P] 0x7FF97FC1DD54 BlinkDash.dll+001DD54
	[ 2][P] 0x7FF9835299AF skse64_1_6_1170.dll+00199AF
```

`+001D821` is `ExecuteBlink+0xF1`, i.e. the instruction right after
`call Actor::IsInBleedout()`. That stub tail-jumps through a cached function pointer
(`jmp rax`), and the pointer was null, so the game executed address 0.

`IsInBleedout()` is therefore **no longer called** (see the comment in `src/Blink.cpp`); the rest of
the gate checks (enabled, player, 3D, dead, console, paused, blocking window) are still in place and
each one is logged individually.
