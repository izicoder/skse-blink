# BlinkDash

A blink/dash teleport for **Skyrim SE/AE**, as a C++ SKSE plugin (Address Library, one binary for SE + AE).

Hold a movement direction and **tap** the hotkey to dash that way. Havok rays shorten the dash at walls and
follow walkable slopes instead of clipping through them. No `.esp`, no Papyrus, no new records — the plugin
plus an optional settings tab in [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352).

> **AI-generated.** Every line of this plugin was written by an AI — purely vibe-coded, slopped together.
> No human authored or thoroughly reviewed it, so treat it as a hobby project: expect rough edges, and check
> it yourself before trusting it in your load order.

## Features

- **Tap to blink.** Hold a direction (WASD or stick) and tap the hotkey; tap while standing still to blink
  where you face. Holding the hotkey does nothing, so sprint/jump keys are safe to bind.
- **Collision-aware.** Six body-height rays find the first obstacle and a body-volume probe at the landing
  spot catches corners a single line would slip past. The configured distance is a **maximum**: obstacles
  shorten the dash instead of cancelling it, and a 50-unit deadzone stops spamming the hotkey from creeping
  through a wall.
- **Slope-aware.** Hits are classified by surface angle: ground shallower than the **max slope** is followed
  up or down the hill; anything steeper is a wall the blink stops at.
- **Momentum kept.** The controller velocity from before the teleport is restored, so a mid-air blink keeps
  the jump/dash going.
- **Ground snap.** On the ground, the landing is placed on the surface below it (following a slope). In mid
  air the height is kept, so a jump is not dragged down.
- **Effect shader.** Any vanilla `TESEffectShader` in your load order, picked from a dropdown.
- **In-game menu** (SKSE Menu Framework): hotkey rebind, all settings, a logging toggle and a debug view,
  plus Reset defaults. Every change saves itself — no Apply button.
- **Debug view** (off by default): draws the last blink's rays and landing spot in the top-left while you
  play.
- **Logging** (on by default): a trace-level `BlinkDash.log` flushed after every line, toggled from the menu.
- **SkyParkour friendly.** Its crouch slide and mid-air landing roll keep working; the blink re-bases the
  engine's fall tracker so the interrupted fall is not charged on landing.
- Works without SKSE Menu Framework installed (menu skipped, hotkey still works).

## Settings

Stored in `SKSE/Plugins/BlinkDash.json` next to the DLL (inside the mod folder under MO2). Written with
these defaults on first run and never overwritten by builds.

Every number is an integer (units, degrees, points, milliseconds), so nothing in the file is a float.
The file carries a `version` and is migrated in place on launch: a file from an older release
(`staminaCost`, `retriggerDelay`/`cooldown` in seconds) is converted and rewritten once, and options this
build does not recognise are preserved rather than dropped.

| Setting | Default | Range |
|---|---|---|
| Hotkey | Mouse 4 (`0x103`) | keyboard key, mouse button or gamepad button |
| Distance (max) | 200 units | 100–1000 |
| Max slope | 45° | 20–80 |
| Cost per blink | 10 (charged in combat only) | 0–100 |
| Cost resource | Stamina | Stamina, Magicka |
| Charge outside combat | off | — |
| Tap window | 200 ms | 20–1000 |
| Cooldown | 1000 ms | 0–2000 |
| Effect shader | `Skyrim.esm:000E5C` | any discovered shader, or `None` |
| Debug view | off | — |
| Logging | on | — |

Hotkey encoding: `0x100 | n` = mouse button *n* (0=LMB, 1=RMB, 2=MMB, 3=MB4…), `0x1000 | n` = gamepad
button *n*, otherwise a DirectInput keyboard scancode — the same numbering SKSE's `InputMap` uses.

## Requirements

- Skyrim SE/AE
- [SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- Optional: [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) for the settings UI

> **Tested on 1.6.1170 only** (SKSE 2.2.8). That is the one and only runtime this has been run on — no other
> version has been tried. It is built against the Address Library, so other Skyrim SE/AE builds may well work,
> but that is a guess, not a promise.

## Install

Put `BlinkDash.dll`, `BlinkDash.json` and `BlinkDash.ShaderNames.json` in
`…/mods/BlinkDash/SKSE/Plugins/` and enable the mod. Under MO2 the JSON files live inside the mod folder, so
they survive game updates.

## Building

Requires VS 2022/18 with the C++ workload, CMake + Ninja, and a vcpkg checkout
(`git clone https://github.com/microsoft/vcpkg .vcpkg`).

```powershell
$env:VCPKG_ROOT = "$PWD/.vcpkg"
cmake --preset vs2022
cmake --build build/vs2022
```

CommonLibSSE-NG is **not compiled**: the build links the prebuilt package its release publishes (a one-time
~128 MB download, cached under the build tree), so a build compiles this plugin's own sources only.

Build from a **VS developer shell** (`vcvars64.bat`), otherwise the compiler will not find the Windows SDK.

By default the DLL is not copied anywhere. Set `MOD_OUTPUT_FOLDER` to a mod folder to auto-deploy on build,
e.g. a git-ignored `CMakeUserPresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "vs2022-local", "inherits": "vs2022",
      "cacheVariables": { "MOD_OUTPUT_FOLDER": "C:/path/to/mods/BlinkDash" } }
  ]
}
```

## Releasing

`tools/Build-Release.ps1` builds the release and packages it in one step:

```powershell
.\tools\Build-Release.ps1 -Version 1.0.1
```

It configures and builds the `vs2022` preset with `-Version` stamped into the binary (so the log
line and the SKSE plugin record report it), then writes `packages/BlinkDash-<version>.zip`
containing `SKSE/Plugins` (DLL + both JSONs) and `docs/`, which MO2/Vortex install as-is. Upload
that zip by hand on the [mod page](https://www.nexusmods.com/skyrimspecialedition/mods/195907)
under **Files**. Nothing in the repo talks to the Nexus API.

- The MSVC environment is set up by the script itself (it runs `vcvars64.bat`, located with
  `vswhere`), so it can be started from a normal shell; `VCPKG_ROOT` or a `.vcpkg` checkout next
  to the repo is still required.
- `-SkipBuild` re-zips whatever is already in the build tree — for a failed upload, not for
  skipping the version stamp; rebuild when `-Version` changes.
- `-FromFolder <mod folder>` packages an already deployed mod folder (what `MOD_OUTPUT_FOLDER`
  deploys) instead of the build output.

## Direction

The mod reads the game's control map for the keys bound to Forward/Back/Strafe, plus the left stick, and
applies the result in the camera's yaw. A clearly pushed stick beats the keys, so a drifting stick never
fights the keyboard. With nothing held but the player moving anyway (root motion, a slide, a knockback) the
horizontal velocity is used. Nothing held and not moving: no blink.

## Effects

`effectShader` selects a vanilla effect shader played on you when you blink. The plugin discovers every
`TESEffectShader` in the load order at runtime; the optional `BlinkDash.ShaderNames.json` only adds readable
labels, keyed `plugin:formid` so they survive load-order changes. It is generated from the vanilla masters by
`tools/GenerateShaderNames.ps1`; add your own entries by hand any time.

## Logging

Every step is logged at **trace** level and flushed after every line, so the last line is the last thing that
ran before a crash. Untick **Write BlinkDash.log** (or set `"logging": false`) to silence it — the plugin
runs the same either way.

```
…\Documents\My Games\Skyrim Special Edition\SKSE\BlinkDash.log
```

The log covers load, every SKSE message, config load/save and clamps, input events, menu interaction, and
every blink step (gate checks, raycasts, teleport, fall-tracker re-base).

## Debug view

Tick **Draw last blink in game** and the last blink is drawn in the top-left while you play: a plan view of
the rays and landing with a scale bar and legend, plus the numbers (travel taken vs requested, blocked, body
fits, direction, climb, surface angle) and what actually hit. It hides while a menu is open or the game is
paused, so it never covers the cursor.

## Why a blink can refuse

Logged with the reason every time: nothing held and not moving · not enough stamina/magicka · inside the
cooldown · inside the deadzone (something solid within 50 units) · no clear landing spot for the body.

## License

[MIT](LICENSE).
