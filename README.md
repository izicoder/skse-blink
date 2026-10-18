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

| Setting | Default | Range |
|---|---|---|
| Hotkey | Mouse 4 (`0x103`) | keyboard key, mouse button or gamepad button |
| Distance (max) | 200 units | 100–1000 |
| Max slope | 45° | 20–80 |
| Stamina cost | 10 (charged in combat only) | 0–100 |
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

The archive that goes on Nexus is built and uploaded by two scripts in `tools/`; both talk to
Nexus Mods' official **Upload API (v3)**, so a release is one command and nothing touches the
website. Get an API key from [your API keys page](https://www.nexusmods.com/settings/api-keys)
and put it in `NEXUSMODS_API_KEY`.

```powershell
# 1. build the archive: SKSE/Plugins (DLL + both JSONs) and docs/, as packages/BlinkDash-<version>.zip
.\tools\Package.ps1 -Version 1.0.0

# 2a. first release of the mod: add the archive as a new file
.\tools\Publish-Nexus.ps1 -Version 1.0.0 -ModId <mod id> -UpdateModVersion

# 2b. every release after that: add it as a new version of the existing file
.\tools\Publish-Nexus.ps1 -Version 1.0.1 -ModId <mod id> -FileId <file id> `
	-UpdateModVersion -Changelog "Fixed the landing offset on slopes."
```

- **Mod id** is the number in the mod page URL (`…/skyrimspecialedition/mods/**123456**`).
- **File id** is the v3 id of the file this release updates — the *update chain* id shown under
  the **Advanced** option on the mod's Files tab. Keep using the same one for every release.
- The script creates an upload session, PUTs the zip to the returned presigned URL (with the
  `Content-Disposition`/`Content-MD5` headers the signature requires), finalises it, waits for
  the file to become available, then creates the file/version and the changelog.

The API can only add files to a mod that already exists, so the mod page and its **first** file
are still created by hand on the site; everything after that is automated. Archives over 100 MiB
would need the multipart endpoints, which the script does not use (BlinkDash is well under it).

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

Logged with the reason every time: nothing held and not moving · not enough stamina · inside the cooldown ·
inside the deadzone (something solid within 50 units) · no clear landing spot for the body.

## License

[MIT](LICENSE).
