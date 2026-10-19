# Roadmap

What BlinkDash is: an SKSE plugin (no ESP, no Papyrus, no new records) that teleports the player with
collision-aware distance shortening, slope following, body fitting, ground snap and fall-tracker re-basing,
plus an optional SKSE Menu Framework tab.

Current release: **1.2.0** — settings file format **2**.

---

## Ground rules for everything below

- **Never break an existing install.** Old `BlinkDash.json` files are migrated on load and rewritten once
  (`Config::kSettingsFormatVersion` + `Migrate`); options this build does not know are preserved. Only break
  this for a genuinely significant change, and then migrate or rename with a logged warning.
- **A question is not a work order.** Answer, then wait for an explicit request.
- `/W4`, warning-clean, C++23, tabs/Allman, `namespace BlinkDash`, heavy "why" comments.
- No exception may cross an engine boundary; all logging through `BD*`.
- Nothing ships without being run in game on 1.6.1170 (SKSE 2.2.8) — the only runtime tested so far.

---

## 1. Debt and robustness (do before new features)

| Item | Why |
|---|---|
| Replace the `-1e9` cooldown sentinel with an explicit "has blinked" state (`std::optional` clock or an `int` ms clock plus a bool). | The sentinel caused a real regression in 1.2.0: `(now - (-1e9)) * 1000` overflowed the `int` cast and the cooldown rejected every blink forever. The clamp fixed the symptom; the sentinel is still a trap. |
| Decide whether the cost *probe* stays. | It exists because a modded setup can invert or refuse the damage modifier. It works, but it is the most speculative code in the plugin. Options: keep probing but expose the learned result in the menu ("cost is being charged: yes/no"), or drop the probe if the vanilla path proves reliable across setups. |
| Audit `Actor::GetActorValueModifier` (RELOCATION_ID 37524/38469). | Used by the cost probe and the diagnostics. A `0` slot on some runtime would be a crash; verify against the address library for every supported build or degrade to value-only detection. |
| Demote the "caught a fall that was already N units deep" warning. | It fires on nearly every airborne blink; it should be trace-level, or logged once per fall sequence. |
| Add a test target for `Config`. | Migration is pure JSON→JSON and runs without the game: a table of 1.0.x / 1.1.x / current / future / corrupt files would pin the compat rule down. Also covers clamping and unknown-key preservation. |
| Runtime coverage table. | Build supports SE+AE+VR but only 1.6.1170 has been run. Test 1.5.97 (SKSE 2.0.20) and 1.6.640, then state the result in the README. |
| Structured-ish log sections. | One blink produces 100+ trace lines; a `BLINKDASH_TRACE`-style switch (separate from `logging`) would keep normal logs readable. |
| `Debug::Snapshot` history. | Only the last blink is kept; a small ring buffer would let the overlay show the last N (and screenshot-friendly summaries). |

---

## 2. Feature candidates

### Already shipped from user reports
- Magicka as an alternative cost resource, and charging the cost outside combat (Nexus, 1.1.0).
- Required-perk gate with a readable perk picker, multi-rank numbering (1.1.1).
- Integer settings + versioned, self-migrating config (1.2.0).

### Most likely next (ranked)
1. **Backstep when no direction is held.** The most common ask for any dodge mod: blink along the
   character's back vector instead of refusing. Small change in `ResolveDirection`; needs a setting.
2. **Direction source option**: movement keys (today) vs camera forward. Some players run third-person
   camera mods that decouple the two.
3. **Cost/distance scaling hooks**: distance or cost multiplied by a skill (Alteration/Sneak) or modified by a
   perk entry point. Keeps the plugin record-free by reading values only.
4. **Hold-to-aim**: while the hotkey is held, draw the predicted landing (the debug rays already compute it);
   release to blink. Turns the current tap-only hotkey into an optional charge.
5. **Re-blink in the air**: a second blink while airborne, with its own cooldown and cost. Doable; must not
   fight the fall tracker.
6. **Sound effect**: optional 2D/3D sound next to the effect shader, discovered from vanilla like shaders.
7. **Cooldown/charge readout**: small HUD indicator (optional, no hard dependency on TrueHUD/SkyHUD).
8. **Per-context gating**: "interiors only", "combat only", "not while swimming/sprinting/mounted".
9. **Presets**: named setting files plus import/export, so users can switch between a "dodge" and a
   "mobility" profile without hand-editing JSON.
10. **Collision policy**: choose whether actors block a blink (today the filter excludes them). A "cannot
    blink past people" option is a plausible balance request.

### Deliberately out of scope
- ESP records, Papyrus scripts, new perks/spells — the plugin-only identity is the selling point.
- Animation-graph driven dashes: SkyParkour/Open Animation Replacer already own that space, and a custom
  animation would need records.
- Multiplayer/network sync.

---

## 3. Docs and release process

- README: add a short "how it behaves" GIF, an FAQ ("why the Adult tag?", "why the cost is not charged",
  "why does the blink refuse near walls") and the runtime coverage table.
- `CHANGELOG.md` mirroring the git log per release (the Nexus file description can link to it).
- Nexus: keep the file version in sync with `BLINKDASH_VERSION`; note the settings migration in every release
  that changes the format.
- Release checklist: `tools\Build-Release.ps1 -Version X.Y.Z` → `tools\Publish-Release.ps1 -Version X.Y.Z
  -Nexus -GitHub` (publishes only on command; `-DryRun` previews) → verify in game from a **fresh**
  config and from a **1.0.0-era** config.

---

## 4. Testing checklist (manual, per release)

1. Blink from flat ground, from a slope, into a slope, at a wall, under a low ceiling, in the air.
2. Stamina cost in combat; out of combat with and without `costOutsideCombat`; magicka cost the same way.
3. Empty bar → blink refuses with `not enough …`; cost drains by exactly the configured amount.
4. Perk gate: without the perk (refused), with it (allowed), then reset defaults (allowed again).
5. Reset defaults, then blink without restarting the game.
6. Cooldown: 0 ms, 1000 ms, 2000 ms — first blink of a session must work in all three cases.
7. Config: delete the file (defaults written), old-format file (migration logged, rewritten, values kept),
   hand-broken file (defaults, no crash), unknown key (preserved).
8. Read `BlinkDash.log` for `[E]`, and for `[W]` lines that are not expected.

---

## 5. Settings format history

| Format | Releases | Shape |
|---|---|---|
| 1 (implicit) | 1.0.x | `staminaCost`, `retriggerDelay`/`cooldown` in floating point seconds |
| 1 (implicit) | 1.1.x | adds `costAmount`/`costResource`/`costOutsideCombat`, timing still seconds |
| 2 | 1.2.0+ | everything integer, timing in `retriggerMs`/`cooldownMs`, `"version": 2` stamped |

Known risks worth remembering: mods that grant "infinite stamina out of combat" mask any cost; some setups
refuse or invert actor value writes (the probe exists for that); Immersive Equipment Displays detours the
`ActorValueOwner` vtable (verified harmless, but it is in the call path).
