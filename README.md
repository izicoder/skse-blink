# BlinkDash

SKSE plugin for Skyrim SE/AE (built against the Address Library, single binary for SE + AE) that adds a
blink/dash teleport on a hotkey, with Havok collision detection and an in-game settings menu via
SKSE Menu Framework. No animations, no VFX, no `.esp`, no Papyrus.

This is the **C++ SKSE plugin only**. The old Papyrus/PEX/Mutagen attempt has been deleted.

## Features

- **Blink hotkey** (default: **Mouse 4**). Hold a direction (WASD or stick) and **tap** the hotkey: the blink
  goes the way you are pushing. Hold nothing, or stand still, and nothing happens.
- **Collision detection**: Havok rays at chest and head height find the first wall/obstacle in the blink
  path, and a body-volume probe at the landing spot catches the corners a single line can slip past. The
  configured distance is a **maximum**: whatever the rays find shortens the blink instead of cancelling it.
  Close up against geometry there is a **deadzone** (50 units) where the blink refuses outright, which is
  what stops a spammed hotkey from creeping through a wall a few units at a time.
- **Momentum**: the velocity the character controller had before the teleport is put straight back
  afterwards, so blinking in mid air keeps the jump and the dash going instead of stopping dead.
- **Ground snap**: only while you are standing on something. A blink in mid air keeps your height, so a
  jump is not dragged back down to the floor.
- **Blink effect shader**: any vanilla `TESEffectShader` in your load order, picked from a dropdown.
  No ESP, no new records, no sound.
- **Stamina cost** (charged in combat only), a **tap window** and an optional **cooldown** (default 0,
  i.e. off) between blinks.
- **SKSE Menu Framework tab** ("Blink Dash"): hotkey rebind, distance, stamina cost, tap window, the
  effect shader dropdown and a debug view, each with a one line explanation, plus a Reset defaults button.
  There is no Test blink and no Save/Apply: every change saves itself.
- **Debug view** (off by default): draws the last blink's rays and landing spot, so a "why did it clip that
  wall" question can be answered by looking instead of by reading a log.
- Settings persist to `SKSE/Plugins/BlinkDash.json` next to the DLL (inside the mod folder under MO2).
- Works without SKSE Menu Framework installed (menu is skipped, hotkey still works).

## Direction and the tap window

Everything comes off the input the mod itself collects, so the keyboard and the pad are on the same list:

- **Movement inputs.** The game's own control map is asked which keys/buttons are bound to Forward, Back,
  Strafe Left and Strafe Right right now (so rebinding and non-QWERTY layouts work), and those are matched
  against the button state the input sink collected. The left thumbstick is read from the thumbstick events
  the same way — a stick pushed clearly beats the keys, otherwise the keys do, so a drifting stick never
  fights the keyboard.
  `PlayerControls::data.moveInputVec` is deliberately **not** used: in this runtime it reads as `(0, input
  length)`, never signed, so it cannot tell forward from back or left from right.
- **Movement.** With nothing held but the player moving anyway — root motion, a scripted slide, a knockback
  — the horizontal velocity is the direction.
- **Nothing.** Not holding anything and not moving: no blink. The hotkey does nothing.
- The input is applied in the camera's yaw, which is the frame the engine moves the character in.
- The hotkey can be bound to a keyboard key, a mouse button or a **gamepad button** (the rebind capture takes
  any of them).
- The hotkey is **tap to blink**: press and release inside the retrigger delay (default 200 ms) fires one
  blink. Holding it longer does nothing, so it is safe to put on sprint or jump. After a blink that actually
  happened, the optional **cooldown** (default 0) has to pass before the next tap is accepted.
- Blinks are queued from the input thread and executed on a game thread.

## What the blink touches

It reads the player's velocity and collision filter, casts read-only rays into the Havok world, then writes
the player's position, puts the character controller's velocity back the way it found it (that is the
momentum, not new movement), damages the stamina actor value (in combat) and plays an effect shader. No
animation graph call, no ragdoll, no force or impulse, no engine-driven locomotion.

## Effects

`effectShader` in `BlinkDash.json` selects a vanilla effect shader, played on you when you blink. The
plugin discovers **every** effect shader in the load order at runtime; the name file only provides the
readable label for the ones it knows:

| | |
|---|---|
| `Skyrim.esm:000E5C` | a known shader: `[plugin file]:[form id inside that plugin]` |
| `OCPA.esl:000801` | an unknown one (Creation Club, mods) - shown with its form id |
| `None` | off |

Keys are `plugin:formid`, not a runtime form id, so they survive load order changes. The empty string is
treated as `None`, and an unknown key falls back to the first shader whose name contains invis/blink/
teleport/summon/ghost (logged, and changeable in the menu).

**`SKSE/Plugins/BlinkDash.ShaderNames.json`** holds the readable names, keyed the same way:

```json
{
    "Dawnguard.esm:0028EE":  "Death Hound",
    "Skyrim.esm:000E5C":  "Invis Nightingale",
    "Skyrim.esm:01B212":  "Fire"
}
```

It is generated by `tools/GenerateShaderNames.ps1` from the vanilla masters (Skyrim.esm, Dawnguard.esm,
Dragonborn.esm - 276 shaders). Skyrim strips editor IDs at runtime, so the names have to be read out of the
plugin files; that script is the only thing that does it. Add your own entries by hand any time - the file
is read at load, missing names just fall back to the form id, and it is never overwritten by the build.

At load the plugin logs the resolved `ApplyEffectShader` address and every shader it found:

```
effects: ApplyEffectShader (RELOCATION_ID(19446, 19872)) resolved to 0x7FF7051E2B10
effects: 281 TESEffectShader forms in the load order
effects: 276 shader names loaded from ...\BlinkDash.ShaderNames.json
effects:   #0 'Death Hound' 0x02028EE (Dawnguard.esm)
effects:   #196 'OCPA.esl:000801' 0xFE00801 (OCPA.esl, no name in the json)
```

`ApplyEffectShader` is resolved by hand so the address can be checked before the first call: a
`RELOCATION_ID` slot of 0 has no mapping for the current runtime and would be a jump into null (which is
exactly what crashed the old build through `Actor::IsInBleedout`, whose AE slot is 0 in CommonLibSSE).

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
| Distance | 300 units |
| Stamina cost | 10 (charged in combat only) |
| Retrigger delay | 200 ms |
| Cooldown | 0 ms (off) |
| Effect shader | `Skyrim.esm:000E5C` (Nightingale invisibility) |
| Debug view | off |

`BlinkDash.json` is written with these defaults on first run and is **not** overwritten by builds, so your
saved settings survive.

Hotkey encoding: `0x100 | n` = mouse button n (0=LMB, 1=RMB, 2=MMB, 3=MB4...), `0x1000 | n` = gamepad
button n (dpad up/down/left/right, start, back, thumb clicks, shoulders, A/B/X/Y, LT/RT), otherwise a
DirectInput keyboard scancode. That is the same numbering SKSE's `InputMap` uses, so the menu shows the
engine's own key name, `Mouse 4`, or `Gamepad A`.

Distance is clamped to 100..2020 units, loaded or typed: 2020 is the hard cap the blink itself applies, so
the setting can no longer ask for travel that will never happen. It is a maximum, not a promise: the rays
shorten the travel when something is in the way, and only give up when there is no room left at all.

There is no `enabled`, `mode`, `minDistance` or `groundSnap` option any more.
Clearance (40), the shortest blink (10), the **deadzone** (clearance + shortest = 50 units: inside it the
blink refuses), the two obstacle ray heights (64 / 120), the body probe (24 units at 48 / 120), the
step-back (25 units, 8 tries), the ground snap band (+/-256 units) and the "standing on ground" tolerance
(32 units) are internal constants.

## Why a blink can refuse

Logged with the reason every time:

| Reason | Means |
|---|---|
| nothing held and not moving | no direction to blink in |
| not enough stamina | in combat and below the configured cost |
| cooldown | inside the configured cooldown after the last successful blink |
| inside the blink deadzone | something solid is within 50 units |
| no clear landing spot for the body | the rays were clear but the body would not fit at the landing |
| no blink direction | the input produced a degenerate direction |

## Debug view

Tick **Draw what the last blink probed** in the menu's Debug section and the last blink is drawn **in game,
in the top left of the screen**, where it can be read while playing: blink, look, blink again. Nothing is
drawn in the menu itself. It is a labelled panel:

- a title line and two rows of numbers: travel taken vs requested, blocked or not, how many times the body
  fit had to step the landing back, the direction, and whether the ground was snapped or the height kept,
- a **plan view** (north up, `N` marked, with a scale bar): the chest/head rays in green when clear and red
  when they hit, orange for a body probe that found something, blue ticks for the vertical ground probes,
  a yellow square for where you were and a blue one for where you land,
- a legend and the list of what actually hit, with how far along each ray.

It is drawn through the menu framework's HUD element hook with plain 2D drawing in the corner of the screen:
no camera maths, no world projection, nothing that can disagree with what the rays actually did.

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

## Crash triage

A crash that lands with a `BlinkDash.dll+...` frame in the call stack is this plugin's; a crash that only
shows engine frames is not. Two example verdicts, from the same session log:

- **Older, this plugin's fault.** `BlinkDash.dll+001D821` (the bleedout stub, above) and
  `BlinkDash.dll+001D50E` both topped the stack *and* all had `skse64_...dll+00199AF` right below, i.e. the
  crash was inside the plugin's own blink task. Both are fixed in the current source.
- **Newer, not this plugin.** The `BlinkDash.log` ends on a successful blink, and ~1 s later CrashLogger
  reports:

  ```
  Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at 0x7FF70509AA54 SkyrimSE.exe+0B6AA54  mov rax, [rdx]   (rdx = 0)
  	[ 0][P] SkyrimSE.exe+0B6AA54   FUN_140b6aa50_B6AA54
  	[ 1][P] SkyrimSE.exe+0B47887   hkpConstraintInstance::sub_140A85CA0_B47887
  	[ 2][P] SkyrimSE.exe+0AAD7BF   hkbRagdollDriver::sub_1409EB010_AAD7BF
  	[ 3][P] Precision.dll+005890F  Hooks::HavokHooks::hkbRagdollDriver_DriveToPose
  ```

  That is a **Havok ragdoll step** on a world NPC (constraint `NPC COM` -> `NPC L Thigh` on "Nils" in an
  exterior cell), reached through Precision's own `hkbRagdollDriver::DriveToPose` hook, on the engine's
  physics job thread. No `BlinkDash.dll` frame appears anywhere in it.

