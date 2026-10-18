#include "PCH.h"
#include "Blink.h"
#include "Config.h"
#include "Debug.h"
#include "Effects.h"
#include "Input.h"
#include "Log.h"
#include "SKSEMenuFramework.h"

namespace BlinkDash
{
	// What this file does to the world, and nothing else: it reads the player's
	// horizontal velocity, reads the collision filter, casts rays into the Havok world
	// (read-only queries), then writes the player's position, optionally damages the
	// stamina actor value and plays an effect shader. No animation graph, no ragdoll,
	// no force, no impulse, no velocity write: the blink is a dislocation.
	namespace
	{
		// Internal tuning that is not exposed in the menu.
		constexpr float kChestHeight = 64.0f;      // lower obstacle ray above the feet
		constexpr float kHeadHeight = 120.0f;      // upper obstacle ray above the feet
		constexpr float kMaxDistanceCap = 2020.0f;
		constexpr float kClearance = 40.0f;        // pull back from a wall we hit
		constexpr float kMinTravel = 10.0f;        // shortest blink worth doing
		// No blink at all when the nearest obstacle is inside this: it is what stops a
		// spammed hotkey from creeping forward a few units at a time through a wall.
		constexpr float kDeadzone = kClearance + kMinTravel;
		constexpr float kInputDeadzone = 0.15f;    // input below this counts as "not holding a direction"
		constexpr float kStickPriority = 0.5f;     // a stick pushed past this beats the movement keys
		constexpr float kVelocityThreshold = 25.0f;  // units/second below this counts as standing still
		constexpr float kGroundProbeUp = 120.0f;
		constexpr float kGroundProbeDown = 600.0f;
		constexpr float kGroundLift = 4.0f;        // land a hair above the floor
		constexpr float kMaxSnapDown = 256.0f;     // further down than this = the drop is ignored
		constexpr float kMaxSnapUp = 256.0f;       // further up than this = do not pop up onto a ledge
		constexpr float kGroundedTolerance = 32.0f;  // ground this close under the feet means "standing"
		constexpr std::uintptr_t kMinPlausibleVtable = 0x10000;

		// Destination body fit: the obstacle rays test a line, so a corner can still
		// clip a shoulder where that line is clear. Once a landing is picked, the body
		// volume there is probed sideways and the travel is stepped back until it fits.
		constexpr float kBodyProbeHeights[] = { 48.0f, kHeadHeight };
		constexpr float kBodyProbeLength = 24.0f;  // under the controller radius, so only real overlap trips it
		constexpr float kFitStep = 25.0f;
		constexpr int   kMaxFitPulls = 8;

		// Dash gate: while true, new requests are ignored.
		std::atomic<bool> blinkGate{ false };
		std::atomic<bool> blinkQueued{ false };
		std::atomic<std::uint64_t> requestSeq{ 0 };
		std::atomic<std::uint64_t> ignoreCount{ 0 };

		// Clock of the last successful blink, for the cooldown.
		std::atomic<float> lastBlinkClock{ -1.0e9f };

		std::string Fmt(const RE::NiPoint3& a_point)
		{
			return std::format("({:.1f}, {:.1f}, {:.1f})", a_point.x, a_point.y, a_point.z);
		}

		float Now()
		{
			static const auto start = std::chrono::steady_clock::now();
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
		}

		void AddSegment(Debug::Snapshot& a_snapshot, const char* a_label, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const RE::NiPoint3& a_point, bool a_hit, float a_fraction)
		{
			if (a_snapshot.count >= Debug::Snapshot::kMaxSegments) {
				return;
			}
			auto& segment = a_snapshot.segments[a_snapshot.count++];
			segment.from = a_from;
			segment.to = a_hit ? a_point : a_to;
			segment.hit = a_hit;
			segment.fraction = a_fraction;
			segment.label = a_label;
		}

		// -----------------------------------------------------------------------
		// Direction
		// -----------------------------------------------------------------------

		struct DirectionResult
		{
			RE::NiPoint3 dir{};
			const char*  source{ "unresolved" };
			bool         ok{ false };
		};

		struct InputAxis
		{
			float       x{ 0.0f };   // strafe, + right
			float       y{ 0.0f };   // forward, + forward
			bool        ok{ false };
			bool        fromStick{ false };
			const char* source{ "nothing held" };
		};

		// The movement inputs the game has bound right now, read against the button state
		// the input sink collected: the keyboard, and anyone who put movement on gamepad
		// buttons. Go through ControlMap so rebinding and non-QWERTY layouts work.
		InputAxis ReadBoundMovementAxis()
		{
			InputAxis axis;

			auto* controlMap = RE::ControlMap::GetSingleton();
			auto* userEvents = RE::UserEvents::GetSingleton();
			if (!controlMap || !userEvents) {
				BD("  input: no control map/user events, bound movement is unavailable");
				return axis;
			}

			const RE::INPUT_DEVICE devices[] = { RE::INPUT_DEVICE::kKeyboard, RE::INPUT_DEVICE::kGamepad };
			for (const auto device : devices) {
				const auto encode = [&](const RE::BSFixedString& a_event) {
					return Input::State::Encode(device, controlMap->GetMappedKey(a_event, device));
				};
				const auto forward = encode(userEvents->forward);
				const auto back = encode(userEvents->back);
				const auto strafeLeft = encode(userEvents->strafeLeft);
				const auto strafeRight = encode(userEvents->strafeRight);

				float x = 0.0f;
				float y = 0.0f;
				if (Input::State::IsHeld(forward)) { y += 1.0f; }
				if (Input::State::IsHeld(back)) { y -= 1.0f; }
				if (Input::State::IsHeld(strafeRight)) { x += 1.0f; }
				if (Input::State::IsHeld(strafeLeft)) { x -= 1.0f; }

				BD("  input: {} bound fwd=0x{:X} back=0x{:X} strafeL=0x{:X} strafeR=0x{:X} -> held (x {:.2f}, y {:.2f})",
					device == RE::INPUT_DEVICE::kKeyboard ? "keyboard" : "gamepad",
					forward, back, strafeLeft, strafeRight, x, y);

				if (x != 0.0f || y != 0.0f) {
					axis.x = x;
					axis.y = y;
					axis.ok = true;
					axis.source = device == RE::INPUT_DEVICE::kKeyboard ? "movement keys" : "gamepad movement buttons";
					return axis;
				}
			}
			return axis;
		}

		// The left stick, straight off the thumbstick events: raw device state, exactly
		// like the keys above. PlayerControls::data.moveInputVec is not usable for this -
		// in this runtime it reads as (0, input length), never signed, so it cannot tell
		// forward from back or left from right.
		InputAxis ReadStickAxis()
		{
			InputAxis axis;
			if (!Input::State::StickSeen()) {
				return axis;
			}

			axis.x = Input::State::s_stickX.load(std::memory_order_relaxed);
			axis.y = Input::State::s_stickY.load(std::memory_order_relaxed);
			const float magnitude = std::sqrt(axis.x * axis.x + axis.y * axis.y);
			BD("  input: left stick (x {:.3f}, y {:.3f}) magnitude {:.3f}", axis.x, axis.y, magnitude);

			if (magnitude >= kInputDeadzone) {
				axis.ok = true;
				axis.fromStick = true;
				axis.source = "left stick";
			}
			return axis;
		}

		// The same list as the keys: a stick that is pushed clearly wins, otherwise the
		// keys do, so a drifting stick never fights the keyboard.
		InputAxis ReadInputAxis()
		{
			const auto stick = ReadStickAxis();
			if (stick.ok && std::sqrt(stick.x * stick.x + stick.y * stick.y) >= kStickPriority) {
				return stick;
			}
			const auto keys = ReadBoundMovementAxis();
			if (keys.ok) {
				return keys;
			}
			return stick;
		}

		DirectionResult ResolveDirection(RE::PlayerCharacter* a_player)
		{
			DirectionResult result;

			auto axis = ReadInputAxis();
			float inputMagnitude = std::sqrt(axis.x * axis.x + axis.y * axis.y);
			BD("  direction: input axis (x {:.3f}, y {:.3f}) magnitude {:.3f} source='{}'",
				axis.x, axis.y, inputMagnitude, axis.source);

			float cameraYaw = 0.0f;
			bool haveCamera = false;
			if (auto* camera = RE::PlayerCamera::GetSingleton()) {
				cameraYaw = camera->GetRuntimeData2().yaw;
				haveCamera = true;
			}
			const float playerYaw = a_player->data.angle.z;
			const float frameYaw = haveCamera ? cameraYaw : playerYaw;
			BD("  direction: cameraYaw={:.4f} ({:.1f} deg) playerYaw={:.4f} ({:.1f} deg)",
				cameraYaw, cameraYaw * 57.29578f, playerYaw, playerYaw * 57.29578f);

			RE::NiPoint3 velocity{};
			a_player->GetLinearVelocity(velocity);
			velocity.z = 0.0f;
			const float speed = velocity.Length();
			BD("  direction: horizontal velocity = {} speed = {:.1f}", Fmt(velocity), speed);

			// The frame the engine moves the character in.
			const RE::NiPoint3 forward{ std::sin(frameYaw), std::cos(frameYaw), 0.0f };
			const RE::NiPoint3 right{ std::cos(frameYaw), -std::sin(frameYaw), 0.0f };

			if (inputMagnitude >= kInputDeadzone) {
				if (axis.fromStick && speed > kVelocityThreshold) {
					// Nothing documents the sign the stick arrives in, and the engine's own
					// vector cannot be used to find out. While the character is moving the
					// movement is the ground truth, so settle each axis against it.
					const float movedForward = velocity.x * forward.x + velocity.y * forward.y;
					const float movedRight = velocity.x * right.x + velocity.y * right.y;
					if (axis.y * movedForward < 0.0f) {
						BD("  direction: stick y {:.3f} points against the movement ({:.1f}), flipping", axis.y, movedForward);
						axis.y = -axis.y;
					}
					if (axis.x * movedRight < 0.0f) {
						BD("  direction: stick x {:.3f} points against the movement ({:.1f}), flipping", axis.x, movedRight);
						axis.x = -axis.x;
					}
				}
				result.dir = forward * axis.y + right * axis.x;
				result.source = axis.source;
				BD("  direction: input in yaw frame, forward={} right={}", Fmt(forward), Fmt(right));
			} else if (speed > kVelocityThreshold) {
				// Nothing held but the player is moving anyway: root motion, a scripted
				// slide, a knockback. The velocity is the only honest answer there.
				result.dir = velocity;
				result.source = "horizontal velocity (nothing held)";
			} else {
				result.source = "nothing held and not moving";
				BD("  direction: REJECTED, {}", result.source);
				return result;
			}

			result.dir.z = 0.0f;
			const float length = result.dir.Unitize();
			BD("  direction: candidate {} from '{}' (length before normalize {:.4f})",
				Fmt(result.dir), result.source, length);

			if (length <= 1e-6f) {
				result.source = "degenerate direction";
				BD("  direction: REJECTED, {}", result.source);
				return result;
			}

			result.ok = true;
			BD("  direction: RESOLVED {} (source: {})", Fmt(result.dir), result.source);
			return result;
		}

		// -----------------------------------------------------------------------
		// Collision
		// -----------------------------------------------------------------------

		// Keeps the parent group in the high word but forces the layer to the
		// character controller so the ray cannot hit the player itself.
		std::uint32_t BuildRayFilter(std::uint32_t a_rawFilter)
		{
			const std::uint32_t filter = ((a_rawFilter >> 16) << 16) |
				static_cast<std::uint32_t>(RE::COL_LAYER::kCharController);
			BD("  filter: actor raw=0x{:08X} -> ray filter=0x{:08X}", a_rawFilter, filter);
			return filter;
		}

		struct RayResult
		{
			RE::NiPoint3 point{};
			bool         hit{ false };
			float        fraction{ 1.0f };
		};

		RayResult Raycast(const char* a_label, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, std::uint32_t a_filter)
		{
			RayResult result;

			const float scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick{};
			pick.rayInput.from = a_from * scale;
			pick.rayInput.to = a_to * scale;
			pick.rayInput.filterInfo.filter = a_filter;
			BD("  ray[{}]: from={} to={} filter=0x{:08X} scale={:.6f}", a_label, Fmt(a_from), Fmt(a_to), a_filter, scale);

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				BDE("  ray[{}]: no player singleton, treating the path as clear", a_label);
				result.point = a_to;
				return result;
			}

			auto* cell = player->GetParentCell();
			RE::bhkWorld* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				BDW("  ray[{}]: no havok world for this cell (0x{:X}), treating the path as clear",
					a_label, reinterpret_cast<std::uintptr_t>(cell));
				result.point = a_to;
				return result;
			}

			world->PickObject(pick);

			result.hit = pick.rayOutput.HasHit();
			result.fraction = result.hit ? pick.rayOutput.hitFraction : 1.0f;
			result.point = result.hit ? (a_from + (a_to - a_from) * pick.rayOutput.hitFraction) : a_to;
			BD("  ray[{}]: hasHit={} fraction={:.4f} point={} pickFailed={}",
				a_label, result.hit, result.fraction, Fmt(result.point), pick.pickFailed);
			return result;
		}

		// Casts the two body-height obstacle rays and returns the nearest fraction, with
		// the geometry captured for the debug view.
		float ObstacleFraction(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_feet, const RE::NiPoint3& a_direction, float a_distance, std::uint32_t a_filter, Debug::Snapshot& a_snapshot)
		{
			const float heights[] = { kChestHeight, kHeadHeight };
			const char* labels[] = { "chest", "head" };

			float nearest = 2.0f;
			for (std::size_t i = 0; i < std::size(heights); ++i) {
				const RE::NiPoint3 from = a_feet + RE::NiPoint3{ 0.0f, 0.0f, heights[i] };
				const RE::NiPoint3 to = from + a_direction * a_distance;
				const auto ray = Raycast(labels[i], from, to, a_filter);
				AddSegment(a_snapshot, labels[i], from, to, ray.point, ray.hit, ray.fraction);
				if (ray.hit) {
					nearest = (std::min)(nearest, ray.fraction);
				}
			}
			UNUSED(a_player);
			return nearest;
		}

		// True when the body volume at a_center is clear of geometry: short sideways
		// probes at two heights, shorter than the character radius so that merely
		// standing near a wall does not trip it, only landing inside one does. The
		// forward direction is deliberately not probed: the obstacle rays own that axis,
		// and probing it again would reject the "wall right in front, blink what is left"
		// case that the shortened travel exists for.
		bool BodyFits(const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction, std::uint32_t a_filter, Debug::Snapshot& a_snapshot, const char* a_label)
		{
			const RE::NiPoint3 right{ a_direction.y, -a_direction.x, 0.0f };
			const RE::NiPoint3 directions[] = { a_direction * -1.0f, right, right * -1.0f };
			const char* names[] = { "back", "right", "left" };

			for (const float height : kBodyProbeHeights) {
				const RE::NiPoint3 from = a_center + RE::NiPoint3{ 0.0f, 0.0f, height };
				for (std::size_t i = 0; i < std::size(directions); ++i) {
					const RE::NiPoint3 to = from + directions[i] * kBodyProbeLength;
					const auto ray = Raycast(a_label, from, to, a_filter);
					AddSegment(a_snapshot, a_label, from, to, ray.point, ray.hit, ray.fraction);
					if (ray.hit) {
						BD("  fit: {} of the landing is blocked at height {:.0f} (fraction {:.3f})",
							names[i], height, ray.fraction);
						return false;
					}
				}
			}
			return true;
		}

		struct GroundProbe
		{
			RE::NiPoint3 point{};
			float        delta{ 0.0f };   // landed z minus the reference z
			bool         hit{ false };
		};

		GroundProbe ProbeGround(const RE::NiPoint3& a_reference, std::uint32_t a_filter, Debug::Snapshot& a_snapshot, const char* a_label)
		{
			const RE::NiPoint3 start{ a_reference.x, a_reference.y, a_reference.z + kGroundProbeUp };
			const RE::NiPoint3 end = start - RE::NiPoint3{ 0.0f, 0.0f, kGroundProbeUp + kGroundProbeDown };
			const auto ray = Raycast(a_label, start, end, a_filter);
			AddSegment(a_snapshot, a_label, start, end, ray.point, ray.hit, ray.fraction);

			GroundProbe probe;
			probe.hit = ray.hit;
			probe.point = ray.hit ? ray.point : end;
			probe.delta = probe.point.z + kGroundLift - a_reference.z;
			return probe;
		}

		bool CanBlinkNow(RE::PlayerCharacter* a_player, const char* a_source)
		{
			BD("blink gate checks (source={}):", a_source);

			BD("  player=0x{:X}", reinterpret_cast<std::uintptr_t>(a_player));
			if (!a_player) {
				BD("  -> REJECTED: no player singleton");
				return false;
			}

			auto* object3D = a_player->Get3D();
			BD("  player 3D=0x{:X}", reinterpret_cast<std::uintptr_t>(object3D));
			if (!object3D) {
				BD("  -> REJECTED: player has no 3D loaded");
				return false;
			}

			const bool dead = a_player->IsDead();
			BD("  IsDead={}", dead);
			if (dead) {
				BD("  -> REJECTED: player is dead");
				return false;
			}

			// player->IsInBleedout() is deliberately NOT called: in the earlier
			// build that call jumped to a null function pointer and crashed the
			// game (CrashLogger: BlinkDash.dll+001D821, RIP=0x0).
			BD("  bleedout check: skipped on purpose, see Blink.cpp");

			auto* ui = RE::UI::GetSingleton();
			if (ui) {
				const bool consoleOpen = ui->IsMenuOpen(RE::Console::MENU_NAME);
				const bool paused = ui->GameIsPaused();
				BD("  console menu open={} game paused={}", consoleOpen, paused);
				if (consoleOpen) {
					BD("  -> REJECTED: console is open");
					return false;
				}
				if (paused) {
					BD("  -> REJECTED: game is paused");
					return false;
				}
			}

			if (SKSEMenuFramework::IsInstalled()) {
				const bool blocking = SKSEMenuFramework::IsAnyBlockingWindowOpened();
				BD("  blocking menu window open={}", blocking);
				if (blocking) {
					BD("  -> REJECTED: a blocking menu window is open");
					return false;
				}
			}

			BD("  -> all gate checks passed");
			return true;
		}

		bool ExecuteBlink(std::uint64_t a_seq, const char* a_source)
		{
			BD("blink[{}] ===== ExecuteBlink begin (source={}, thread={}) =====", a_seq, a_source, Log::ThreadId());

			const auto& settings = Config::Get();
			BD("blink[{}] settings: {}", a_seq, Config::Describe());

			Debug::Snapshot snapshot;
			snapshot.summary = std::format("blink #{} ({})", a_seq, a_source);

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!CanBlinkNow(player, a_source)) {
				BD("blink[{}] aborted by the gate checks", a_seq);
				return false;
			}
			if (!player) {
				BDE("blink[{}] player became null after the gate checks", a_seq);
				return false;
			}

			const float now = Now();
			BD("blink[{}] clock={:.3f}s", a_seq, now);

			// Cooldown: off unless configured, and only successful blinks count.
			if (settings.cooldown > 0.0f) {
				const float since = now - lastBlinkClock.load(std::memory_order_acquire);
				if (since < settings.cooldown) {
					BDI("blink[{}] REJECTED: cooldown, {:.0f}ms since the last blink of {:.0f}ms",
						a_seq, since * 1000.0f, settings.cooldown * 1000.0f);
					return false;
				}
			}

			RE::ActorValueOwner* staminaOwner = nullptr;

			// The cost exists only in combat: out of combat the blink is free, which is
			// what a "no stamina drain out of combat" setup expects from anything that
			// spends stamina.
			const bool inCombat = player->IsInCombat();
			BD("blink[{}] in combat={}", a_seq, inCombat);

			if (settings.staminaCost <= 0.0f) {
				BD("blink[{}] stamina: no cost configured, skipping the check", a_seq);
			} else if (!inCombat) {
				// Log the value as well: a report of "stamina went down out of combat" can
				// then be checked against what the blink actually saw at that moment.
				float stamina = -1.0f;
				if (auto* owner = player->AsActorValueOwner()) {
					const auto vtable = *reinterpret_cast<const std::uintptr_t*>(owner);
					if (vtable >= kMinPlausibleVtable) {
						stamina = owner->GetActorValue(RE::ActorValue::kStamina);
					}
				}
				BD("blink[{}] stamina: out of combat, no cost charged (stamina {:.1f}, untouched)", a_seq, stamina);
			} else {
				// NOTE: never call player->GetActorValue() / player->DamageActorValue()
				// directly. Those use the ActorValueOwner base offset baked into the
				// class layout (0x98 in this build), which does not match this runtime
				// (0xB8 on 1.6.x) and crashed the game with a garbage vtable.
				// AsActorValueOwner() is the runtime-versioned accessor and is correct.
				BD("blink[{}] stamina: calling player->AsActorValueOwner()", a_seq);
				auto* owner = player->AsActorValueOwner();
				const auto ownerAddress = reinterpret_cast<std::uintptr_t>(owner);
				const auto vtable = owner ? *reinterpret_cast<const std::uintptr_t*>(owner) : 0;
				BD("blink[{}] stamina: owner=0x{:X} (player+0x{:X}) vtable=0x{:X}",
					a_seq, ownerAddress, ownerAddress - reinterpret_cast<std::uintptr_t>(player), vtable);

				if (owner == nullptr || vtable < kMinPlausibleVtable) {
					BDE("blink[{}] stamina: actor value owner looks wrong (vtable 0x{:X}), skipping the cost instead of crashing",
						a_seq, vtable);
				} else {
					const float stamina = owner->GetActorValue(RE::ActorValue::kStamina);
					BD("blink[{}] stamina: current={:.1f} cost={:.1f}", a_seq, stamina, settings.staminaCost);
					if (stamina < settings.staminaCost) {
						BDI("blink[{}] REJECTED: not enough stamina", a_seq);
						return false;
					}
					staminaOwner = owner;  // spent after the teleport, so a rejected blink is free
				}
			}

			const auto direction = ResolveDirection(player);
			BD("blink[{}] direction ok={} source='{}' dir={}", a_seq, direction.ok, direction.source, Fmt(direction.dir));
			if (!direction.ok) {
				BDI("blink[{}] REJECTED: no blink direction ({})", a_seq, direction.source);
				return false;
			}

			const RE::NiPoint3 feet = player->GetPosition();
			BD("blink[{}] feet position = {}", a_seq, Fmt(feet));

			const float distance = std::clamp(settings.distance, 0.0f, kMaxDistanceCap);
			if (distance < kMinTravel) {
				BD("blink[{}] REJECTED: configured distance {:.0f} is below the minimum travel {:.0f}",
					a_seq, distance, kMinTravel);
				return false;
			}
			BD("blink[{}] distance request={:.1f} clamped={:.1f} (cap {:.1f})", a_seq, settings.distance, distance, kMaxDistanceCap);

			std::uint32_t rawFilter = 0;
			{
				RE::CFilter cfilter;
				player->GetCollisionFilterInfo(cfilter);
				rawFilter = cfilter.filter;
				BD("blink[{}] raw collision filter = 0x{:08X}", a_seq, rawFilter);
			}
			const auto rayFilter = BuildRayFilter(rawFilter);

			// --- obstacle rays ------------------------------------------------------
			// The distance is a maximum: whatever the rays find shortens the travel, it
			// never turns a blink into a no-op.
			BD("blink[{}] --- obstacle rays (chest +{:.0f}, head +{:.0f}) ---", a_seq, kChestHeight, kHeadHeight);
			const float nearest = ObstacleFraction(player, feet, direction.dir, distance, rayFilter, snapshot);
			const bool blocked = nearest <= 1.0f;

			float travel = distance;
			if (blocked) {
				const float clear = distance * nearest;
				if (clear < kDeadzone) {
					BDI("blink[{}] REJECTED: obstacle is {:.1f} units away, inside the {:.0f} unit blink deadzone",
						a_seq, clear, kDeadzone);
					return false;
				}
				travel = clear - kClearance;
				snapshot.shortened = true;
				BD("blink[{}] blocked at {:.1f} units, pulled back by clearance {:.0f} -> travelling {:.1f}",
					a_seq, clear, kClearance, travel);
			} else {
				BD("blink[{}] path is clear, landing at the full distance", a_seq);
			}

			// --- destination body fit ------------------------------------------------
			RE::NiPoint3 landing{};
			int pulls = 0;
			for (;;) {
				landing = feet + direction.dir * travel;
				landing.z = feet.z;
				BD("blink[{}] fit check #{} at {} (travel {:.1f})", a_seq, pulls, Fmt(landing), travel);
				if (BodyFits(landing, direction.dir, rayFilter, snapshot, pulls == 0 ? "fit" : "fit-retry")) {
					break;
				}
				if (pulls >= kMaxFitPulls || travel <= kMinTravel) {
					BDI("blink[{}] REJECTED: no clear landing spot for the body (last try {:.0f} units out)", a_seq, travel);
					return false;
				}
				++pulls;
				travel = (std::max)(travel - kFitStep, kMinTravel);
				snapshot.shortened = true;
				BDW("blink[{}] landing overlapped geometry, stepping back to {:.1f}", a_seq, travel);
			}

			// --- ground ---------------------------------------------------------------
			// Snapping only happens when the player is standing on something. Mid air the
			// height is kept, so a blink during a jump keeps the jump instead of being
			// dragged down to the floor. Standing still, the usual snap applies.
			const auto underFeet = ProbeGround(feet, rayFilter, snapshot, "ground-here");
			const bool grounded = underFeet.hit && std::abs(underFeet.delta) <= kGroundedTolerance;
			BD("blink[{}] standing on ground={} (probe hit={} delta={:.1f})", a_seq, grounded, underFeet.hit, underFeet.delta);
			snapshot.grounded = grounded;

			float landingZ = feet.z;
			const char* heightSource = grounded ? "kept height (no ground at the landing spot)" : "kept height (airborne)";
			if (grounded) {
				const auto ground = ProbeGround(landing, rayFilter, snapshot, "ground");
				snapshot.groundHit = ground.hit;
				if (ground.hit && ground.delta >= -kMaxSnapDown && ground.delta <= kMaxSnapUp) {
					landingZ = ground.point.z + kGroundLift;
					heightSource = std::abs(ground.delta) < 2.0f ? "ground snap (level)" : "ground snap";
					BD("blink[{}] ground snap: delta {:.1f} units, landing z {:.1f}", a_seq, ground.delta, landingZ);
				} else if (ground.hit) {
					BDW("blink[{}] ground is {:.1f} units away, too far to snap, staying at z {:.1f}", a_seq, ground.delta, feet.z);
				} else {
					BD("blink[{}] no ground under the landing spot, keeping the current height", a_seq);
				}
			} else {
				BD("blink[{}] airborne, keeping the current height (no snap)", a_seq);
			}
			landing.z = landingZ;

			const float travelled = (landing - feet).Length();
			BD("blink[{}] travel from the feet = {:.1f} (minimum {:.1f})", a_seq, travelled, kMinTravel);
			if (travelled < kMinTravel) {
				BDI("blink[{}] REJECTED: only {:.1f} units of room (wedged against geometry)", a_seq, travelled);
				return false;
			}
			BD("blink[{}] landing point = {} ({})", a_seq, Fmt(landing), heightSource);

			// Momentum: moving the player resets the velocity the character controller
			// carries, and that velocity is what a jump and a dash are made of. Whatever it
			// was before the teleport is put back afterwards, so blinking in mid air keeps
			// going instead of stopping dead. Read and written as-is: same units, same
			// source, so nothing here has to guess a scale.
			auto*    controller = player->GetCharController();
			RE::hkVector4 velocityBefore{};
			bool     velocityViaGetter = false;
			if (controller) {
				controller->GetLinearVelocityImpl(velocityBefore);
				velocityViaGetter = velocityBefore.Dot3(velocityBefore) > 1e-6f;
				if (!velocityViaGetter) {
					// Some controllers keep the integrated velocity in these instead.
					if (controller->initialVelocity.Dot3(controller->initialVelocity) > 1e-6f) {
						velocityBefore = controller->initialVelocity;
					} else if (controller->outVelocity.Dot3(controller->outVelocity) > 1e-6f) {
						velocityBefore = controller->outVelocity;
					}
				}
				BD("blink[{}] momentum: controller 0x{:X} velocity {:.3f} (via getter={}, initial={:.3f}, out={:.3f})",
					a_seq, reinterpret_cast<std::uintptr_t>(controller),
					std::sqrt(velocityBefore.Dot3(velocityBefore)), velocityViaGetter,
					std::sqrt(controller->initialVelocity.Dot3(controller->initialVelocity)),
					std::sqrt(controller->outVelocity.Dot3(controller->outVelocity)));
			} else {
				BDW("blink[{}] momentum: no character controller, the velocity cannot be kept", a_seq);
			}
			const bool haveVelocity = velocityBefore.Dot3(velocityBefore) > 1e-6f;

			player->SetPosition(landing, true);

			if (controller && haveVelocity) {
				if (velocityViaGetter) {
					controller->SetLinearVelocityImpl(velocityBefore);
				} else {
					controller->initialVelocity = velocityBefore;
					controller->outVelocity = velocityBefore;
				}
				RE::hkVector4 check{};
				controller->GetLinearVelocityImpl(check);
				BD("blink[{}] momentum: restored {:.3f} (now reads {:.3f})",
					a_seq, std::sqrt(velocityBefore.Dot3(velocityBefore)), std::sqrt(check.Dot3(check)));
			}

			const RE::NiPoint3 after = player->GetPosition();
			BD("blink[{}] position after the teleport = {} (delta {})", a_seq, Fmt(after), Fmt(after - feet));

			if (staminaOwner) {
				// The damage modifier is a signed offset on the current value, so the
				// cost has to be passed negative to make it a drain.
				const float drain = -settings.staminaCost;
				BD("blink[{}] stamina: calling ModActorValue(kDamage, kStamina, {:.1f})", a_seq, drain);
				staminaOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, drain);
				BD("blink[{}] stamina: after={:.1f}", a_seq, staminaOwner->GetActorValue(RE::ActorValue::kStamina));
			}

			Effects::PlayOnPlayer("blink");
			lastBlinkClock.store(now, std::memory_order_release);

			snapshot.feet = feet;
			snapshot.direction = direction.dir;
			snapshot.landing = landing;
			snapshot.blocked = blocked;
			snapshot.requested = distance;
			snapshot.travel = travel;
			snapshot.pulls = pulls;
			snapshot.summary = std::format(
				"blink #{}  {}  dir {}  source {}\nrequested {:.0f}  travel {:.0f}  blocked {}  pulls {}  height {}",
				a_seq, grounded ? "on ground" : "airborne", Fmt(direction.dir), direction.source,
				distance, travel, blocked, pulls, heightSource);
			snapshot.headline = std::format(
				"blink #{} {}  travel {:.0f}/{:.0f}  blocked {}  pulls {}  dir {}  src {}",
				a_seq, grounded ? "ground" : "air", travel, distance, blocked, pulls, Fmt(direction.dir), direction.source);
			Debug::Record(std::move(snapshot));

			BDI("blink[{}] SUCCESS source={} direction={} travel={:.1f} blocked={} height={} from={} to={}",
				a_seq, a_source, direction.source, travelled, blocked, heightSource, Fmt(feet), Fmt(after));
			BD("blink[{}] ===== ExecuteBlink end =====", a_seq);
			return true;
		}

		// Runs on a game thread (SKSE task interface). Owns the dash gate.
		void GameThreadBlink()
		{
			const auto seq = requestSeq.load(std::memory_order_acquire);
			BD("task: GameThreadBlink started (seq={}, thread={})", seq, Log::ThreadId());
			blinkQueued.store(false, std::memory_order_release);

			bool acquired = false;
			if (blinkGate.compare_exchange_strong(acquired, true)) {
				try {
					ExecuteBlink(seq, "hotkey-tap");
				} catch (const std::exception& e) {
					BDE("task: blink task threw: {}", e.what());
				} catch (...) {
					BDE("task: blink task threw a non-std exception");
				}
				blinkGate.store(false, std::memory_order_release);
			} else {
				BD("task: the dash gate was already taken, dropping this request");
			}
			BD("task: GameThreadBlink finished (seq={})", seq);
		}
	}

	bool IsBlinkQueued()
	{
		return blinkQueued.load(std::memory_order_acquire) || blinkGate.load(std::memory_order_acquire);
	}

	void RequestBlink(const char* a_source)
	{
		const auto seq = requestSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
		BD("request[{}]: received (source={}, thread={})", seq, a_source, Log::ThreadId());

		if (IsBlinkQueued()) {
			const auto ignored = ignoreCount.fetch_add(1, std::memory_order_acq_rel) + 1;
			BD("request[{}]: ignored, the dash gate is already active (total ignored {})", seq, ignored);
			return;
		}

		bool expected = false;
		if (!blinkQueued.compare_exchange_strong(expected, true)) {
			BD("request[{}]: ignored, lost the queue race", seq);
			return;
		}

		if (const auto task = SKSE::GetTaskInterface()) {
			task->AddTask(GameThreadBlink);
			BD("request[{}]: task queued", seq);
		} else {
			BDE("request[{}]: no task interface available, dropping the request", seq);
			blinkQueued.store(false, std::memory_order_release);
		}
	}

	bool DoBlink(const char* a_source)
	{
		const auto seq = requestSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
		BD("request[{}]: direct DoBlink (source={}, thread={})", seq, a_source, Log::ThreadId());

		if (blinkGate.load(std::memory_order_acquire)) {
			BD("request[{}]: ignored, the dash gate is already active", seq);
			return false;
		}
		blinkGate.store(true, std::memory_order_release);

		bool result = false;
		try {
			result = ExecuteBlink(seq, a_source);
		} catch (const std::exception& e) {
			BDE("request[{}]: DoBlink threw: {}", seq, e.what());
		} catch (...) {
			BDE("request[{}]: DoBlink threw a non-std exception", seq);
		}

		blinkGate.store(false, std::memory_order_release);
		BD("request[{}]: DoBlink returning {}", seq, result);
		return result;
	}
}
