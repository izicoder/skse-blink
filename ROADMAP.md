# Roadmap

What BlinkDash is: an SKSE plugin (no ESP, no Papyrus, no new records) that teleports the player with
collision-aware distance shortening, slope following, body fitting, ground snap and fall-tracker re-basing,
plus an optional SKSE Menu Framework tab.

Current release: **1.2.2** — settings file format **2**.

---

## Ground rules for everything below

- **Never break an existing install.** Old `BlinkDash.json` files are migrated on load and rewritten once
  (`Config::kSettingsFormatVersion` + `Migrate`); options this build does not know are preserved. Only break
  this for a genuinely significant change, and then migrate or rename with a logged warning. A new option must
  default to **today's behaviour**, so an upgrading user cannot tell the difference until they opt in.
- **A question is not a work order.** Answer, then wait for an explicit request.
- `/W4`, warning-clean, C++23, tabs/Allman, `namespace BlinkDash`, heavy "why" comments.
- No exception may cross an engine boundary; all logging through `BD*`.
- Nothing ships without being run in game on 1.6.1170 (SKSE 2.2.8) — the only runtime tested so far.

---

## 1. What users actually said (Nexus posts, 19 Oct 2026 — 9 comments)

The first real feedback on the page, read off the Posts tab. Everything below is quoted intent, not
paraphrase, because the priority order comes from here.

| # | Who | Said | State |
|---|---|---|---|
| 1 | Ph4sm (premium) | "mid-air blink keeps the jump going … Very welcome feature." | Shipped in 1.2.2. Do not regress it: the fall re-base and the kept controller velocity are the two things this person noticed. |
| 1 | Ph4sm | "I would appreciate magicka cost as an alternative to stamina cost, and a toggle for not in combat cost" | Shipped (`costResource`, `costOutsideCombat`); author replied "added" and the user confirmed "Thank you for the update". Loop closed — nothing to do. |
| 2 | Azreal015 | "Maybe add a seamless animation if possible?" | Open. A real animation needs records, so only the movement half is reachable — see `dashMs` below. |
| 3 | worfol (premium) | "make the dash a bit slower … it would be easier on the eyes if it weren't so instantaneous — or perhaps it could have a blur effect, so it doesn't feel quite as jarring." | Open, and now the highest-value request on the page. Two asks in one post; both are cheap and neither needs records. |
| 4 | Vaz2008 / wunderbutt | "why Adult?" | Not a code problem: the page carries the **Sexualised** and **AI-Generated Content** tags. Either the tags are wrong and should be cleaned up, or the answer belongs in the description/FAQ. Chasing the tag is a five-minute job; leaving it unexplained costs a joke thread on every first visit. |
| 5 | ArqueiroFreelancer | "Very nice!!" | Nothing to do. |

Net read: nobody reported a bug. The feedback is entirely "the feel is close, but the teleport reads as a
jarring pop". That is the theme for 1.3.

---

## 2. Next release (1.3.0): make the blink read as motion, not as a cut

Goal: answer post 3 fully and post 2 as far as a record-free plugin can. Everything here is opt-in and
defaults to current behaviour.

### 2.1 `dashMs` — spread the move over time instead of teleporting

- The blink already knows both ends of the path: `ResolveDirection` + the rays give a start and a landing
  point, the slope/body-fit/snap stages give the corrected endpoint. Instead of one position write, walk the
  player along that segment over `dashMs` (target range 0–200 ms, default **0 = the current instant blink**).
- Constraints, in the order they will bite:
  - The corridor is proven by rays from the *start* point only. Do not re-ray every step (cost, and jitter at
    the end); commit to the pre-computed endpoint and interpolate. Re-run the existing body-fit/snap on the
    final step only, as today.
  - The fall-tracker re-base must happen at the **end** of the interpolation, not the start, or a mid-air dash
    charges the interrupted fall.
  - Controller velocity is currently kept across the teleport; keep that behaviour and ignore per-step
    velocity while the dash is running.
  - Cooldown and the cost drain start when the key is pressed, not when the dash finishes, so spamming cannot
    squeeze two blinks out of one window.
  - The overlay (`Debug::Snapshot`) should record `dashMs` and the number of steps actually used.
- Risk: a per-frame position write is exactly the kind of thing SkyParkour's roll and the havok char controller
  dislike. It needs the most in-game testing of anything in this file — interiors, stairs, slopes, mid-air,
  and with SkyParkour installed.

### 2.2 `blur` — hide the cut with an image-space modifier

- The other half of worfol's post, and the only "animation" a record-free plugin can honestly offer.
- Mechanism: trigger a vanilla `TESImageSpaceModifier` (IMAD) on the same timer as `dashMs` and let it fade
  out, i.e. the same discovery trick the effect shaders use — enumerate `TESImageSpaceModifier` forms, keep a
  generated name list next to `BlinkDash.ShaderNames.json`, auto-pick a name containing "blur" on first run.
- Open questions to settle before writing code: which IMAD in the vanilla masters actually blurs (several are
  mip-blur or DOF, and DOF is jarring at this duration); whether the fade needs a per-frame hook or can be
  handed to the effect with a duration; and restoring cleanly on save load, menu open and worldspace change.
- If no vanilla IMAD is usable, the fallback is honest and cheap: a brighter/longer version of the existing
  effect shader plus a short FOV punch. Do not add an ENB or a shader-file dependency for this.

### 2.3 Menu shape: a preset, not two more sliders

Two raw integer sliders do not communicate "how does this feel". Ship `Dash feel` as a dropdown —
**Instant (default) / Quick / Smooth** — mapping onto `dashMs` + `blur`, with the sliders still available
under the advanced entries for people who want 137 ms. Existing users see "Instant", which is exactly what
they have today.

### 2.4 Config format 3

`dashMs`, `blur`, `blurStrength` + the preset. Defaults reproduce 1.2.2 exactly; migration stamps
`"version": 3` and keeps everything else. This is the first format bump with no behaviour change on upgrade,
which is the point.

---

## 3. Feature candidates after 1.3 (ranked, unchanged logic)

1. **Backstep when no direction is held.** The most common ask for any dodge mod: blink along the
   character's back vector instead of refusing. Small change in `ResolveDirection`; needs a setting.
2. **Direction source option**: movement keys (today) vs camera forward. Some players run third-person
   camera mods that decouple the two.
3. **Re-blink in the air**: a second blink while airborne, with its own cooldown and cost. Doable; must not
   fight the fall tracker (see 2.1).
4. **Hold-to-aim**: while the hotkey is held, draw the predicted landing (the debug rays already compute it);
   release to blink. Turns the current tap-only hotkey into an optional charge, and is the natural partner of
   `dashMs`.
5. **Cost/distance scaling hooks**: distance or cost multiplied by a skill (Alteration/Sneak) or modified by a
   perk entry point. Keeps the plugin record-free by reading values only.
6. **Sound effect**: optional 2D/3D sound next to the effect shader, discovered from vanilla like shaders.
   Pairs with 2.2 — a sound cue sells the dash more than a blur does.
7. **Cooldown/charge readout**: small HUD indicator (optional, no hard dependency on TrueHUD/SkyHUD).
8. **Per-context gating**: "interiors only", "combat only", "not while swimming/sprinting/mounted".
9. **Presets**: named setting files plus import/export, so users can switch between a "dodge" and a
   "mobility" profile without hand-editing JSON.
10. **Collision policy**: choose whether actors block a blink (today the filter excludes them). A "cannot
    blink past people" option is a plausible balance request.

### Deliberately out of scope

- ESP records, Papyrus scripts, new perks/spells — the plugin-only identity is the selling point. This is
  also the honest answer to Azreal015's "seamless animation": a real dodge animation needs records and/or an
  OAR integration, and SkyParkour/Open Animation Replacer already own that space. `dashMs` is the reachable
  half of the request; say so when replying rather than leaving it open again.
- Multiplayer/network sync.

---

## 4. Debt and robustness (do alongside, not before, 1.3)

| Item | Why |
|---|---|
| Replace the `-1e9` cooldown sentinel with an explicit "has blinked" state (`std::optional` clock or an `int` ms clock plus a bool). | The sentinel caused a real regression in 1.2.0: `(now - (-1e9)) * 1000` overflowed the `int` cast and the cooldown rejected every blink forever. The clamp fixed the symptom; the sentinel is still a trap — and 2.1 starts the cooldown from a new place, so do not carry the sentinel into it. |
| Decide whether the cost *probe* stays. | It exists because a modded setup can invert or refuse the damage modifier. It works, but it is the most speculative code in the plugin. Options: keep probing but expose the learned result in the menu ("cost is being charged: yes/no"), or drop the probe if the vanilla path proves reliable across setups. |
| Audit `Actor::GetActorValueModifier` (RELOCATION_ID 37524/38469). | Used by the cost probe and the diagnostics. A `0` slot on some runtime would be a crash; verify against the address library for every supported build or degrade to value-only detection. |
| Verify the IMAD trigger address before 2.2 ships. | Same rule as `ApplyEffectShader` (19446/19872): resolve by `RELOCATION_ID` by hand, check for `0`, log it. A blur that only works on 1.6.1170 is acceptable; a jump into null is not. |
| Demote the "caught a fall that was already N units deep" warning. | It fires on nearly every airborne blink; it should be trace-level, or logged once per fall sequence. |
| Add a test target for `Config`. | Migration is pure JSON→JSON and runs without the game: a table of 1.0.x / 1.1.x / 1.2.x / current / future / corrupt files would pin the compat rule down. Also covers clamping and unknown-key preservation — and format 3 is the first test of the "no behaviour change" promise. |
| Runtime coverage table. | Build supports SE+AE+VR but only 1.6.1170 has been run. Test 1.5.97 (SKSE 2.0.20) and 1.6.640, then state the result in the README. |
| Structured-ish log sections. | One blink produces 100+ trace lines; a `BLINKDASH_TRACE`-style switch (separate from `logging`) would keep normal logs readable. A `dashMs` dash multiplies those lines per step — bound the per-step logging now. |
| `Debug::Snapshot` history. | Only the last blink is kept; a small ring buffer would let the overlay show the last N (and screenshot-friendly summaries). |

---

## 5. Docs and release process

- **Answer the Adult question on the page** (post 4): either fix the tags or add one line to the description.
  It is the single most visible thing a new visitor sees and it is currently answered only by a joke.
- README: an FAQ ("why the Adult tag?", "why is the blink instant?", "why does the blink refuse near walls",
  "why no animation?") and a short "how it behaves" GIF. A GIF is also the cheapest way to show `dashMs`.
- `CHANGELOG.md` mirroring the git log per release (the Nexus file description can link to it).
- Nexus: keep the file version in sync with `BLINKDASH_VERSION`; note the settings migration in every release
  that changes the format. When 1.3 lands, reply in post 3 and post 2 in the same pass — the thread is small
  enough that every reply is read.
- Release checklist: `tools\Build-Release.ps1 -Version X.Y.Z` → `tools\Publish-Release.ps1 -Version X.Y.Z
  -Nexus -GitHub` (publishes only on command; `-DryRun` previews) → verify in game from a **fresh** config and
  from a **1.0.0-era** config.

---

## 6. Testing checklist (manual, per release)

1. Blink from flat ground, from a slope, into a slope, at a wall, under a low ceiling, in the air.
2. Stamina cost in combat; out of combat with and without `costOutsideCombat`; magicka cost the same way.
3. Empty bar → blink refuses with `not enough …`; cost drains by exactly the configured amount.
4. Perk gate: without the perk (refused), with it (allowed), then reset defaults (allowed again).
5. Reset defaults, then blink without restarting the game.
6. Cooldown: 0 ms, 1000 ms, 2000 ms — first blink of a session must work in all three cases.
7. Config: delete the file (defaults written), old-format file (migration logged, rewritten, values kept),
   hand-broken file (defaults, no crash), unknown key (preserved).
8. Read `BlinkDash.log` for `[E]`, and for `[W]` lines that are not expected.
9. **1.3 additions**: `dashMs` 0 / 60 / 120 / 200 on flat ground, up and down stairs, into a wall (the
   authored distance must still shorten), mid-air over a drop (no fall damage on landing), and with
   SkyParkour installed. Blur on/off, and a save load while a blur is fading. A 1.2.2 config must load and
   feel identical.

---

## 7. Settings format history

| Format | Releases | Shape |
|---|---|---|
| 1 (implicit) | 1.0.x | `staminaCost`, `retriggerDelay`/`cooldown` in floating point seconds |
| 1 (implicit) | 1.1.x | adds `costAmount`/`costResource`/`costOutsideCombat`, timing still seconds |
| 2 | 1.2.0+ | everything integer, timing in `retriggerMs`/`cooldownMs`, `"version": 2` stamped |
| 3 | 1.3.0 (planned) | adds `dashMs`/`blur`/`blurStrength`/`dashFeel`, all defaulting to 1.2.2 behaviour |

Known risks worth remembering: mods that grant "infinite stamina out of combat" mask any cost; some setups
refuse or invert actor value writes (the probe exists for that); Immersive Equipment Displays detours the
`ActorValueOwner` vtable (verified harmless, but it is in the call path); a per-frame position write (2.1) puts
every such detour on a much hotter path than a single teleport did.
