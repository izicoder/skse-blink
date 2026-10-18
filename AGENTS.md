# AGENTS.md

Guidance for AI agents working in this repo. Read it before changing code.

## What this is

**BlinkDash** — an SKSE plugin (C++23, CommonLibSSE-NG, Address Library) that adds a blink/dash teleport to
Skyrim SE/AE. One binary targets SE and AE. There is no `.esp`, no Papyrus, no new records: the plugin reads
input, casts read-only Havok rays, teleports the player, optionally spends stamina and plays a vanilla effect
shader. A settings tab is exposed through the optional SKSE Menu Framework (`extern/SKSEMenuFramework.h`).

The game runs on Windows only, so all development, building and testing happens here.

## Repo layout

| Path | What it does |
|---|---|
| `src/Plugin.cpp` | SKSE entry point + messaging handler; wires everything up at `kDataLoaded`. |
| `src/Log.h` | Logging macros `BD`/`BDI`/`BDW`/`BDE`, forced to trace, flushed per line, `Log::SetEnabled`. |
| `src/Config.h/.cpp` | `Settings` struct (all tunables + defaults), JSON load/save, `Validate()` clamps, `Describe()`. |
| `src/Input.h` | Input sink: raw held-button state, hotkey tap detection, stick read, `RequestBlink`. |
| `src/Blink.h/.cpp` | The blink itself: direction, rays, slope handling, body fit, ground snap, teleport, fall re-base. ~1000 lines, the core. |
| `src/Effects.h/.cpp` | Discovers `TESEffectShader` forms and plays the selected one. |
| `src/Debug.h/.cpp` | Records the last blink as a `Snapshot` (segments + numbers) for the in-menu/overlay view. |
| `src/UI.h/.cpp` | The SKSE Menu Framework tab: sliders, hotkey rebind, effect dropdown, logging/debug toggles, in-game overlay. |
| `extern/SKSEMenuFramework.h` | Vendored framework header (do not edit; it wraps ImGui as `ImGuiMCP`). |
| `tools/GenerateShaderNames.ps1` | Generates `dist/BlinkDash.ShaderNames.json` from the vanilla masters. |
| `dist/` | Shipped default `BlinkDash.json` + the generated shader names file. |

## Build & deploy

Build from a **VS developer shell** (`vcvars64.bat`) — without it the compiler cannot find the Windows SDK,
and the two files that include `SKSEMenuFramework.h` fail on `windows.h`.

```powershell
$env:VCPKG_ROOT = "$PWD/.vcpkg"      # a vcpkg checkout
cmake --preset vs2022                # only when CMake config changed
cmake --build build/vs2022
```

CMake auto-clones `extern/CommonLibSSE` (alandtse/CommonLibSSE-NG, `ng` branch) on first configure. Do **not**
remove `directxtk`/`rapidcsv` from `vcpkg.json`: CommonLibSSE's own `find_package` needs them even though this
plugin does not use them directly.

Auto-deploy to a mod folder is opt-in via a git-ignored `CMakeUserPresets.json` that sets
`MOD_OUTPUT_FOLDER` (see README). A build must be configured with that variable for the post-build copy to
run; the DLL lands in `<MOD_OUTPUT_FOLDER>/SKSE/Plugins/`.

There are **no automated tests**. Verifying a change means building, deploying, launching Skyrim with the mod
enabled, and reading `…\Documents\My Games\Skyrim Special Edition\SKSE\BlinkDash.log`.

## Conventions

- **Tabs** for indentation, Allman braces, `namespace BlinkDash` (or a nested namespace).
- The code is deliberately **heavily commented**: explain *why* a ray, magic number or engine call exists,
  not what the next line does. Keep that style.
- Compiled with `/W4 /permissive- /utf-8`, C++23. Keep it warning-clean; only the vendored
  `SKSEMenuFramework.h` warnings are tolerated.
- Never let an exception cross an engine boundary: catch at entry points and log. All logging goes through
  the `BD*` macros (they swallow their own failures and flush), never raw `spdlog` calls.
- Prefer editing existing patterns over adding abstractions.

## How a change usually flows

1. **New setting** → add a field with a default in `Settings` (`Config.h`); add it to `ToJson`, the JSON keys
   list, the field-by-field load, and clamp it in `Validate()` (`Config.cpp`); add it to `Describe()`; add a
   slider with matching min/max constants in `UI.cpp`. UI slider bounds and `Config::Validate` bounds must
   agree.
2. **New blink behaviour** → `Blink.cpp`. Record anything the debug view should show into the
   `Debug::Snapshot` (`Debug::Record`).
3. **New UI** → `UI.cpp`. Slider writes are debounced via `MarkChanged`/`FlushPendingSave`; discrete controls
   (checkbox, dropdown, rebind, reset) call `SaveNow` immediately. There is no Apply button.
4. **New effect/engine call** → resolve the address by `RELOCATION_ID` by hand and check it before calling
   (see `Effects.cpp`). A `RELOCATION_ID` slot can be `0` on a runtime that lacks it, which would jump to
   null.

## Gotchas

- `Actor::IsInBleedout()` is **not called**: its AE address-library slot is `0` in CommonLibSSE and calling it
  crashed the game. Do not reintroduce it; the other gate checks are in `Blink.cpp`.
- The blink re-bases `bhkCharacterController` fall state on landing (SkyParkour's mid-air roll would otherwise
  charge the interrupted fall as instant death). Be careful when touching position/velocity writes.
- Settings live in `BlinkDash.json` **next to the DLL** and are intentionally never overwritten by the build.
- Target runtime is currently **1.6.1170**; the plugin is built with multi-runtime support (`ENABLE_SKYRIM_SE`
  + `ENABLE_SKYRIM_AE`).

## Git

- Commits are single-line, imperative and descriptive of the outcome (see `git log`).
- When a change is done and it builds, commit it yourself — do not ask whether to commit, and do not ask for
  message approval.
