#include "PCH.h"
#include "Blink.h"
#include "Config.h"
#include "Debug.h"
#include "Effects.h"
#include "Input.h"
#include "Log.h"
#include "Perks.h"
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
		constexpr float kMaxDistanceCap = 1000.0f;
		// Slope bounds for the clamp below: the same numbers Config::Validate and the menu use.
		constexpr int   kMinSlopeDegrees = 20;
		constexpr int   kMaxSlopeDegrees = 80;
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
		constexpr float kMaxSnapUp = 256.0f;       // floor under the climb band: a level landing may still be this far above the feet
		constexpr float kMaxClimbFollow = 1024.0f;  // furthest up a followed slope is allowed to climb over one blink
		// SkyParkour's landing roll waits for half a second of fall before it fires; the
		// fall timer is only pulled back to just past that, never zeroed, so it still does.
		constexpr float kRollFallTimeGate = 0.6f;
		constexpr float kGroundedTolerance = 32.0f;  // ground this close under the feet means "standing"
		constexpr std::uintptr_t kMinPlausibleVtable = 0x10000;
		constexpr float kRadiansPerDegree = 3.14159265f / 180.0f;
		constexpr float kTwoPi = 6.28318531f;

		// Obstacle rays: one per body height, so a crate, a step or a boulder shorter
		// than the chest is still in the way. The first is ankle height, which is what
		// makes a slope climbable and a cliff not: on a slope it hits the ground of the
		// slope (walked along), on a cliff it hits the face (solid).
		constexpr float kPathHeights[] = { 8.0f, 32.0f, 56.0f, 80.0f, 104.0f, 128.0f };
		constexpr const char* kPathLabels[] = { "ankle", "knee", "waist", "chest", "shoulder", "head" };

		// A stair is a sawtooth: each tread is walkable but the riser between two of them is
		// ~90deg, so every slope limit still reads it as a wall. A steep contact is therefore
		// only treated as a wall when there is no room above it: the air just above the contact
		// is probed forward, and when it stays clear for a step's depth the surface is something
		// to walk up (a riser, a kerb, a low fence) rather than something to stop at.
		constexpr float kStepLift = 48.0f;        // how far above the contact the air is tested
		constexpr float kStepClearDepth = 32.0f;  // clear distance that makes it a step
		constexpr float kStepProbeLength = 64.0f;  // longest forward probe

		// Destination body fit: the obstacle rays test a line, so a corner can still
		// clip a shoulder where that line is clear. Once a landing is picked, the body
		// volume there is probed in a circle and the travel is stepped back until it fits.
		constexpr float kBodyProbeHeights[] = { 16.0f, 48.0f, 80.0f, 112.0f };
		constexpr int   kBodyProbeDirections = 8;
		constexpr float kBodyProbeLength = 28.0f;  // under the controller radius, so only real overlap trips it
		constexpr float kBodyProbeTop = 128.0f;    // a ceiling below this is an overlap no sideways probe can see
		constexpr float kFitStep = 25.0f;
		constexpr int   kMaxFitPulls = 8;

		// Dash gate: while true, new requests are ignored.
		std::atomic<bool> blinkGate{ false };
		std::atomic<bool> blinkQueued{ false };
		std::atomic<std::uint64_t> requestSeq{ 0 };
		std::atomic<std::uint64_t> ignoreCount{ 0 };

		// Clock of the last successful blink, for the cooldown, plus whether that clock means
		// anything yet. A sentinel value in the clock itself forced a clamp on every comparison
		// (the difference from -1e9s does not fit an int) and only worked because nothing else
		// moved the clock - a trap for whoever touched it next.
		std::atomic<bool>  hasBlinked{ false };
		std::atomic<float> lastBlinkClock{ 0.0f };

		// How the cost has to be pushed into an actor value on this setup. Vanilla wants a
		// negative damage-modifier delta to spend points, but a value that is rewritten or
		// inverted by another mod turns that into a free heal, so the direction is probed
		// once per resource and remembered. See the spend block in ExecuteBlink.
		enum class CostSign : int
		{
			kUnknown = 0,
			kNegative = -1,  // vanilla: delta -cost takes points away
			kPositive = 1,   // inverted value: delta +cost takes points away
			kBlocked = 2     // neither direction moves it: the cost cannot be charged
		};

		std::atomic<CostSign> costSign[2]{ CostSign::kUnknown, CostSign::kUnknown };  // one per CostResource

		std::atomic<CostSign>& CostSignSlot(CostResource a_resource)
		{
			return costSign[a_resource == CostResource::kMagicka ? 1 : 0];
		}

		std::string Fmt(const RE::NiPoint3& a_point)
		{
			return std::format("({:.1f}, {:.1f}, {:.1f})", a_point.x, a_point.y, a_point.z);
		}

		float Now()
		{
			static const auto start = std::chrono::steady_clock::now();
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
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
			float        slopeDegrees{ 90.0f };  // angle of the hit surface from flat; 90 = a wall
		};

		// Havok reports the hit normal in world space, and the havok world is a uniform
		// scale of the game world, so the direction needs no conversion. A normal that
		// is missing or unusable reads as a wall: that keeps the old "everything blocks"
		// behaviour on a runtime that does not fill it in, instead of letting blinks
		// through geometry.
		float SlopeFromNormal(const RE::hkVector4& a_normal)
		{
			alignas(16) float raw[4]{};
			_mm_storeu_ps(raw, a_normal.quad);
			const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
			if (!std::isfinite(length) || length < 1e-4f) {
				return 90.0f;
			}
			const float cos = std::clamp(raw[2] / length, -1.0f, 1.0f);
			return std::acos(cos) / kRadiansPerDegree;
		}

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
				// Never report a clear path that was not actually tested: a hit at the origin
				// shortens the blink to nothing, which is the safe failure.
				BDE("  ray[{}]: no player singleton, treating the path as blocked", a_label);
				result.hit = true;
				result.fraction = 0.0f;
				result.point = a_from;
				return result;
			}

			auto* cell = player->GetParentCell();
			RE::bhkWorld* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				BDW("  ray[{}]: no havok world for this cell (0x{:X}), treating the path as blocked",
					a_label, reinterpret_cast<std::uintptr_t>(cell));
				result.hit = true;
				result.fraction = 0.0f;
				result.point = a_from;
				return result;
			}

			world->PickObject(pick);

			result.hit = pick.rayOutput.HasHit();
			result.fraction = result.hit ? pick.rayOutput.hitFraction : 1.0f;
			result.point = result.hit ? (a_from + (a_to - a_from) * pick.rayOutput.hitFraction) : a_to;
			result.slopeDegrees = result.hit ? SlopeFromNormal(pick.rayOutput.normal) : 90.0f;
			BD("  ray[{}]: hasHit={} fraction={:.4f} point={} slope={:.1f}deg pickFailed={}",
				a_label, result.hit, result.fraction, Fmt(result.point), result.slopeDegrees, pick.pickFailed);
			return result;
		}

		bool Walkable(float a_slopeDegrees, float a_maxSlope)
		{
			return a_slopeDegrees <= a_maxSlope;
		}

		void AddSegment(Debug::Snapshot& a_snapshot, const char* a_label, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, const RayResult& a_ray, bool a_walked = false)
		{
			if (a_snapshot.count >= Debug::Snapshot::kMaxSegments) {
				return;
			}
			auto& segment = a_snapshot.segments[a_snapshot.count++];
			segment.from = a_from;
			segment.to = a_ray.hit ? a_ray.point : a_to;
			segment.hit = a_ray.hit;
			segment.walked = a_walked;
			segment.slopeDegrees = a_ray.slopeDegrees;
			segment.fraction = a_ray.fraction;
			segment.label = a_label;
		}

		// True when a steep contact is only a step: the air kStepLift above the contact point has
		// to stay clear for kStepClearDepth along the blink direction. A wall has no such air (the
		// probe hits it immediately), a riser does (the next treads are below the probe).
		bool IsStepOver(const RayResult& a_ray, const RE::NiPoint3& a_direction, float a_distance,
			std::uint32_t a_filter, float a_maxSlope)
		{
			const RE::NiPoint3 from = a_ray.point + RE::NiPoint3{ 0.0f, 0.0f, kStepLift };
			const float remaining = (std::max)(a_distance * (1.0f - a_ray.fraction), kStepClearDepth);
			const float length = (std::min)(remaining, kStepProbeLength);
			const auto  probe = Raycast("step", from, from + a_direction * length, a_filter);

			if (!probe.hit) {
				return true;  // nothing above the contact at all
			}
			if (Walkable(probe.slopeDegrees, a_maxSlope)) {
				return true;  // the air above ends on a walkable surface, i.e. the next tread
			}
			return probe.fraction * length >= kStepClearDepth;
		}

		// The obstacle rays, one per body height, and the nearest fraction that is
		// actually solid. A hit is classified by the surface angle: ground shallow
		// enough to walk on is not an obstacle, the blink goes over it and the landing
		// follows the surface, while anything steeper blocks like a wall, so a cliff
		// stops the blink instead of being climbed. Returns 2.0 when the path is clear.
		float PathFraction(const RE::NiPoint3& a_feet, const RE::NiPoint3& a_direction, float a_distance,
			std::uint32_t a_filter, float a_maxSlope, Debug::Snapshot& a_snapshot)
		{
			static_assert(std::size(kPathHeights) == std::size(kPathLabels));

			float nearest = 2.0f;
			for (std::size_t i = 0; i < std::size(kPathHeights); ++i) {
				const RE::NiPoint3 from = a_feet + RE::NiPoint3{ 0.0f, 0.0f, kPathHeights[i] };
				const RE::NiPoint3 to = from + a_direction * a_distance;
				const auto ray = Raycast(kPathLabels[i], from, to, a_filter);
				const bool walked = ray.hit && Walkable(ray.slopeDegrees, a_maxSlope);
				AddSegment(a_snapshot, kPathLabels[i], from, to, ray, walked);
				if (walked) {
					++a_snapshot.walked;
					BD("  path[{}]: hit a {:.1f}deg surface at {:.1f} units, shallow enough to blink over",
						kPathLabels[i], ray.slopeDegrees, a_distance * ray.fraction);
					continue;
				}
				if (ray.hit) {
					if (IsStepOver(ray, a_direction, a_distance, a_filter, a_maxSlope)) {
						BD("  path[{}]: hit a {:.1f}deg surface at {:.1f} units, but the air above it is clear: step, not a wall",
							kPathLabels[i], ray.slopeDegrees, a_distance * ray.fraction);
						continue;
					}
					nearest = (std::min)(nearest, ray.fraction);
				}
			}
			return nearest;
		}

		// True when the body volume at a_center is clear of geometry: rays out from the
		// body centre in a circle and at four heights, each shorter than the character
		// radius so that merely standing next to a wall is not an overlap. Probing out
		// from the centre is what catches a landing inside a boulder: a ray that starts
		// inside a shape can miss its back faces, a ray that reaches it from outside
		// cannot. Only hits are recorded, the clean probes are noise.
		bool BodyFits(const RE::NiPoint3& a_center, const RE::NiPoint3& a_direction, std::uint32_t a_filter,
			float a_maxSlope, Debug::Snapshot& a_snapshot, const char* a_label)
		{
			for (const float height : kBodyProbeHeights) {
				const RE::NiPoint3 from = a_center + RE::NiPoint3{ 0.0f, 0.0f, height };
				for (int i = 0; i < kBodyProbeDirections; ++i) {
					const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(kBodyProbeDirections);
					const float c = std::cos(angle);
					const float s = std::sin(angle);
					const RE::NiPoint3 outward{ a_direction.x * c - a_direction.y * s,
						a_direction.x * s + a_direction.y * c, 0.0f };
					const RE::NiPoint3 to = from + outward * kBodyProbeLength;
					const auto ray = Raycast(a_label, from, to, a_filter);
					if (!ray.hit) {
						continue;
					}
					if (Walkable(ray.slopeDegrees, a_maxSlope)) {
						AddSegment(a_snapshot, a_label, from, to, ray, true);
						++a_snapshot.walked;
						continue;
					}
					AddSegment(a_snapshot, a_label, from, to, ray);
					BD("  fit: the landing is blocked at height {:.0f}, {:.0f}deg surface {:.1f} units out",
						height, ray.slopeDegrees, kBodyProbeLength * ray.fraction);
					return false;
				}
			}

			// A ceiling below the body height is an overlap no sideways probe can see.
			const RE::NiPoint3 low = a_center + RE::NiPoint3{ 0.0f, 0.0f, kBodyProbeHeights[0] };
			const RE::NiPoint3 top = a_center + RE::NiPoint3{ 0.0f, 0.0f, kBodyProbeTop };
			const auto ceiling = Raycast(a_label, low, top, a_filter);
			AddSegment(a_snapshot, a_label, low, top, ceiling);
			if (ceiling.hit) {
				BD("  fit: a ceiling {:.1f} units above the feet is in the way", kBodyProbeTop * ceiling.fraction);
				return false;
			}
			return true;
		}

		struct GroundProbe
		{
			RE::NiPoint3 point{};
			float        delta{ 0.0f };   // landed z minus the reference z
			float        slopeDegrees{ 90.0f };
			bool         hit{ false };
		};

		GroundProbe ProbeGround(const RE::NiPoint3& a_reference, std::uint32_t a_filter, Debug::Snapshot& a_snapshot, const char* a_label, float a_up)
		{
			const RE::NiPoint3 start{ a_reference.x, a_reference.y, a_reference.z + a_up };
			const RE::NiPoint3 end = start - RE::NiPoint3{ 0.0f, 0.0f, a_up + kGroundProbeDown };
			const auto ray = Raycast(a_label, start, end, a_filter);
			AddSegment(a_snapshot, a_label, start, end, ray);

			GroundProbe probe;
			probe.hit = ray.hit;
			probe.point = ray.hit ? ray.point : end;
			probe.slopeDegrees = ray.slopeDegrees;
			probe.delta = probe.point.z + kGroundLift - a_reference.z;
			return probe;
		}

		// Vanilla threshold under which a landing is free (fJumpFallHeightMin, 600).
		// Read from the game settings rather than hardcoded, because mods move it.
		float FallDamageHeight()
		{
			if (auto* collection = RE::GameSettingCollection::GetSingleton()) {
				if (auto* setting = collection->GetSetting("fJumpFallHeightMin")) {
					return setting->GetFloat();
				}
			}
			return 600.0f;
		}

		// The engine charges fall damage at the next landing from the height it recorded
		// when the fall began. SkyParkour's landing roll deliberately starts while the
		// player is still in the air and puts the character on the ground itself, so a
		// blink in the middle of that fall is a discontinuity the engine knows nothing
		// about and the roll's landing gets charged for a fall the player already left -
		// which kills. Re-basing the tracker on the blink point drops that charge.
		void RebaseFallTracker(RE::bhkCharacterController* a_controller, const RE::NiPoint3& a_landing, float a_damageHeight, std::uint64_t a_seq)
		{
			const float pendingHeight = a_controller->fallStartHeight - a_landing.z;
			const float pendingTime = a_controller->fallTime;

			a_controller->fallStartHeight = a_landing.z;
			a_controller->fallTime = (std::min)(pendingTime, kRollFallTimeGate);

			BD("blink[{}] fall tracker: {:.1f} units above the feet, falling {:.2f}s -> re-based on the landing, fallTime now {:.2f}s",
				a_seq, pendingHeight, pendingTime, a_controller->fallTime);

			if (pendingHeight > a_damageHeight) {
				BDW("blink[{}] the blink caught a fall that was already {:.1f} units deep (damage from {:.0f}): that charge is dropped instead of landing on the next one",
					a_seq, pendingHeight, a_damageHeight);
			}
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

			// Required-perk gate: "None" (or an unresolvable key) always passes, see
			// Perks::PlayerHasRequired. Runs after the cheap checks so an ordinary
			// rejected blink does not pay for the perk lookup.
			if (!Perks::PlayerHasRequired(a_player, a_source)) {
				BD("  -> REJECTED: required perk not unlocked");
				return false;
			}

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

			const auto settings = Config::Snapshot();
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

			// Every ray below needs the cell's havok world. Without one the picks are
			// meaningless, and treating them as clear would teleport through the whole cell;
			// this is the one case where refusing to blink is the only safe answer.
			{
				auto* cell = player->GetParentCell();
				auto* world = cell ? cell->GetbhkWorld() : nullptr;
				BD("blink[{}] cell=0x{:X} havok world=0x{:X}", a_seq,
					reinterpret_cast<std::uintptr_t>(cell), reinterpret_cast<std::uintptr_t>(world));
				if (!world) {
					BDI("blink[{}] REJECTED: this cell has no havok world, the collision rays cannot be cast", a_seq);
					return false;
				}
			}

			const float now = Now();
			BD("blink[{}] clock={:.3f}s", a_seq, now);

			// Cooldown: off unless configured, and only successful blinks count. Both sides of
			// the comparison are whole milliseconds, so a 1000ms cooldown means exactly that.
			if (settings.cooldownMs > 0 && hasBlinked.load(std::memory_order_acquire)) {
				// Both clocks come from the same monotonic source and this one is always at or
				// after the stored one, but the conversion to int is still clamped: a float to
				// int cast outside the int range is undefined behaviour.
				const double sinceRaw = static_cast<double>(now - lastBlinkClock.load(std::memory_order_acquire)) * 1000.0;
				const int    sinceMs = static_cast<int>(std::clamp(sinceRaw, 0.0, 1.0e9));
				if (sinceMs < settings.cooldownMs) {
					BDI("blink[{}] REJECTED: cooldown, {}ms since the last blink of {}ms",
						a_seq, sinceMs, settings.cooldownMs);
					return false;
				}
			}

			// The cost is paid from stamina or magicka, and by default only in combat: out
			// of combat the blink is free, which is what a "no drain out of combat" setup
			// expects from anything that spends a bar. costOutsideCombat disables that
			// filter for players who want the blink to cost something at all times.
			const auto costValue = settings.costResource == CostResource::kMagicka ? RE::ActorValue::kMagicka : RE::ActorValue::kStamina;
			const char* costName = settings.costResource == CostResource::kMagicka ? "magicka" : "stamina";

			RE::ActorValueOwner* costOwner = nullptr;
			const int            costAmount = settings.costAmount;

			const bool inCombat = player->IsInCombat();
			BD("blink[{}] in combat={} cost resource={} amount={} outsideCombat={}",
				a_seq, inCombat, costName, costAmount, settings.costOutsideCombat);

			if (costAmount <= 0) {
				BD("blink[{}] cost: no cost configured, skipping the {} check", a_seq, costName);
			} else if (!inCombat && !settings.costOutsideCombat) {
				// Log the value as well: a report of "the bar went down out of combat" can
				// then be checked against what the blink actually saw at that moment.
				int current = -1;
				if (auto* owner = player->AsActorValueOwner()) {
					const auto vtable = *reinterpret_cast<const std::uintptr_t*>(owner);
					if (vtable >= kMinPlausibleVtable) {
						current = static_cast<int>(std::lround(owner->GetActorValue(costValue)));
					}
				}
				BD("blink[{}] cost: out of combat, no {} charged ({} {}, untouched)", a_seq, costName, costName, current);
			} else {
				// NOTE: never call player->GetActorValue() / player->DamageActorValue()
				// directly. Those use the ActorValueOwner base offset baked into the
				// class layout (0x98 in this build), which does not match this runtime
				// (0xB8 on 1.6.x) and crashed the game with a garbage vtable.
				// AsActorValueOwner() is the runtime-versioned accessor and is correct.
				BD("blink[{}] cost: calling player->AsActorValueOwner()", a_seq);
				auto* owner = player->AsActorValueOwner();
				const auto ownerAddress = reinterpret_cast<std::uintptr_t>(owner);
				const auto vtable = owner ? *reinterpret_cast<const std::uintptr_t*>(owner) : 0;
				BD("blink[{}] cost: owner=0x{:X} (player+0x{:X}) vtable=0x{:X}",
					a_seq, ownerAddress, ownerAddress - reinterpret_cast<std::uintptr_t>(player), vtable);

				if (owner == nullptr || vtable < kMinPlausibleVtable) {
					BDE("blink[{}] cost: actor value owner looks wrong (vtable 0x{:X}), skipping the cost instead of crashing",
						a_seq, vtable);
				} else {
					const int current = static_cast<int>(std::lround(owner->GetActorValue(costValue)));
					BD("blink[{}] cost: current {}={} cost={}", a_seq, costName, current, costAmount);
					if (current < costAmount) {
						BDI("blink[{}] REJECTED: not enough {}", a_seq, costName);
						return false;
					}
					costOwner = owner;  // spent after the teleport, so a rejected blink is free
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

			// The configured distance is an integer; the geometry below is float, so it is
			// clamped as an integer first and converted once.
			const int distanceCap = static_cast<int>(kMaxDistanceCap);
			const int clampedDistance = std::clamp(settings.distance, 0, distanceCap);
			const float distance = static_cast<float>(clampedDistance);
			if (clampedDistance < static_cast<int>(kMinTravel)) {
				BD("blink[{}] REJECTED: configured distance {} is below the minimum travel {}",
					a_seq, clampedDistance, static_cast<int>(kMinTravel));
				return false;
			}
			BD("blink[{}] distance request={} clamped={} (cap {})", a_seq, settings.distance, clampedDistance, distanceCap);

			std::uint32_t rawFilter = 0;
			{
				RE::CFilter cfilter;
				player->GetCollisionFilterInfo(cfilter);
				rawFilter = cfilter.filter;
				BD("blink[{}] raw collision filter = 0x{:08X}", a_seq, rawFilter);
			}
			const auto rayFilter = BuildRayFilter(rawFilter);
			const int maxSlopeDegrees = std::clamp(settings.maxSlope, kMinSlopeDegrees, kMaxSlopeDegrees);
			const float maxSlope = static_cast<float>(maxSlopeDegrees);
			const float slopeTan = std::tan(maxSlope * kRadiansPerDegree);
			const float fallDamageHeight = FallDamageHeight();

			// --- obstacle rays ------------------------------------------------------
			// The distance is a maximum: whatever the rays find shortens the travel, it
			// never turns a blink into a no-op. Only surfaces too steep to walk on count
			// as obstacles, so a hill keeps the whole distance while a cliff blocks it.
			BD("blink[{}] --- obstacle rays ({} heights, slope limit {}deg) ---", a_seq, std::size(kPathHeights), maxSlopeDegrees);
			const float nearest = PathFraction(feet, direction.dir, distance, rayFilter, maxSlope, snapshot);
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

			// Standing on something is what decides whether the landing height is taken
			// from the ground: mid air the height is kept, so a blink during a jump keeps
			// the jump instead of being dragged down to the floor.
			const auto underFeet = ProbeGround(feet, rayFilter, snapshot, "ground-here", kGroundProbeUp);
			const bool grounded = underFeet.hit && std::abs(underFeet.delta) <= kGroundedTolerance;
			BD("blink[{}] standing on ground={} (probe hit={} delta={:.1f})", a_seq, grounded, underFeet.hit, underFeet.delta);
			snapshot.grounded = grounded;

			float landingZ = feet.z;
			const char* heightSource = grounded ? "kept height (no ground at the landing spot)" : "kept height (airborne)";

			// --- landing: ground under it, then the body volume ---------------------
			// The ground is probed first so the body fit runs at the height the player
			// would really stand at: probing a slope at the old feet height would put the
			// probes inside the hill and reject every landing on it.
			RE::NiPoint3 landing{};
			int pulls = 0;
			for (;;) {
				landing = feet + direction.dir * travel;
				landing.z = feet.z;

				// A slope at the configured limit climbs this much over the blink, so the
				// band the landing may climb scales with the distance; a fixed band would
				// read a hill as a ledge.
				const float climbBand = std::clamp(travel * slopeTan + kGroundLift, kMaxSnapUp, kMaxClimbFollow);
				landingZ = feet.z;
				heightSource = grounded ? "kept height (no ground at the landing spot)" : "kept height (airborne)";
				if (grounded) {
					auto ground = ProbeGround(landing, rayFilter, snapshot, "ground", kGroundProbeUp);
					if (!ground.hit) {
						// Nothing within reach means the ground is either above the probe or
						// a long way down. Only then look higher, so a bridge over flat
						// ground - which the first probe finds from below - is never read as
						// a hill to climb onto.
						ground = ProbeGround(landing, rayFilter, snapshot, "ground-high", kGroundProbeUp + climbBand);
					}
					snapshot.groundHit = ground.hit;
					snapshot.groundSlopeDegrees = ground.slopeDegrees;
					if (ground.hit && Walkable(ground.slopeDegrees, maxSlope) && ground.delta >= -kMaxSnapDown && ground.delta <= climbBand) {
						landingZ = ground.point.z + kGroundLift;
						const bool sloped = ground.slopeDegrees > 2.0f && std::abs(ground.delta) > 2.0f;
						heightSource = sloped ? "followed the slope" : (std::abs(ground.delta) < 2.0f ? "ground snap (level)" : "ground snap");
						snapshot.slopeFollowed = sloped;
						BD("blink[{}] ground snap: delta {:.1f} units, surface {:.1f}deg (band {:.1f}), landing z {:.1f}",
							a_seq, ground.delta, ground.slopeDegrees, climbBand, landingZ);
					} else if (ground.hit && !Walkable(ground.slopeDegrees, maxSlope)) {
						BDW("blink[{}] the ground under the landing is {:.1f}deg, too steep to stand on, staying at z {:.1f}",
							a_seq, ground.slopeDegrees, feet.z);
					} else if (ground.hit) {
						BDW("blink[{}] ground is {:.1f} units away, too far to snap, staying at z {:.1f}", a_seq, ground.delta, feet.z);
					} else {
						BD("blink[{}] no ground under the landing spot, keeping the current height", a_seq);
					}
				} else {
					BD("blink[{}] airborne, keeping the current height (no snap)", a_seq);
				}
				landing.z = landingZ;

				const char* fitLabel = pulls == 0 ? "fit" : "fit-retry";
				BD("blink[{}] fit check #{} at {} (travel {:.1f})", a_seq, pulls, Fmt(landing), travel);
				if (BodyFits(landing, direction.dir, rayFilter, maxSlope, snapshot, fitLabel)) {
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

			// A blink is a discontinuity in the middle of a fall, so the engine's fall
			// tracker is re-based on the landing: see RebaseFallTracker.
			if (controller) {
				RebaseFallTracker(controller, landing, fallDamageHeight, a_seq);
			}

			const RE::NiPoint3 after = player->GetPosition();
			BD("blink[{}] position after the teleport = {} (delta {})", a_seq, Fmt(after), Fmt(after - feet));

			if (costOwner) {
				// The cost is a delta on the damage modifier, which is a signed offset on the
				// current value. A negative delta is the vanilla drain, but a setup can store it
				// and still hand points back (an inverted or rewritten actor value), which is a
				// free heal - worse than no cost at all. So the delta is measured, not assumed:
				// the direction is probed once per resource, remembered, and the modifier is
				// logged next to the value so the log shows what the engine actually stored.
				// The configured cost is an integer; the actor value the engine hands back is not,
				// so only this one conversion is left.
				const float amount = static_cast<float>(costAmount);
				const float valueBefore = costOwner->GetActorValue(costValue);
				const float modifierBefore = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue);

				// One write, reported as both the value the player sees and the modifier the engine
				// stored: the two are what tells a real drain from a free heal.
				struct Probe
				{
					float value{ 0.0f };
					float modifier{ 0.0f };
				};

				auto spend = [&](float a_delta) {
					costOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue, a_delta);
					return Probe{ costOwner->GetActorValue(costValue),
						player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue) };
				};

				auto& sign = CostSignSlot(settings.costResource);
				auto learnedSign = sign.load(std::memory_order_acquire);
				if (learnedSign == CostSign::kUnknown) {
					const char* learned = "none";

					// First paid blink on this setup: the direction of the delta is measured, not
					// assumed. The guess is undone by exactly what the engine stored (the modifier
					// is the tell, not the value), so each probe leaves the bar where it started
					// and only the accepted one is a charge. The earlier version tried +2x after a
					// failed -1x, which charged two costs on a value whose sign is inverted.
					const float modifierBeforeGuess = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue);
					const auto  guess = spend(-amount);
					const float guessStored = guess.modifier - modifierBeforeGuess;

					if (guess.value < valueBefore - amount * 0.5f) {
						learnedSign = CostSign::kNegative;
						learned = "negative";
					} else {
						if (std::abs(guessStored) > 0.01f) {
							spend(-guessStored);
						}

						const float modifierBeforeSecond = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue);
						const auto  second = spend(amount);
						const float secondStored = second.modifier - modifierBeforeSecond;

						if (second.value < valueBefore - amount * 0.5f) {
							learnedSign = CostSign::kPositive;
							learned = "positive";
						} else {
							// Neither direction drains: hand back what the probe stored and leave
							// the blink free rather than charging on a guess.
							if (std::abs(secondStored) > 0.01f) {
								spend(-secondStored);
							}
							learnedSign = CostSign::kBlocked;
						}
					}
					sign.store(learnedSign, std::memory_order_release);
					BDI("blink[{}] cost: {} takes a {} delta on this setup, damage modifier {:.1f} -> {:.1f}",
						a_seq, costName, learned, modifierBefore,
						player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue));
				} else if (learnedSign == CostSign::kNegative) {
					spend(-amount);
				} else if (learnedSign == CostSign::kPositive) {
					spend(amount);
				}

				const float valueAfter = costOwner->GetActorValue(costValue);
				const float modifierAfter = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, costValue);
				BD("blink[{}] cost: {} {:.1f} -> {:.1f} (cost {:.1f}, damage modifier {:.1f} -> {:.1f})",
					a_seq, costName, valueBefore, valueAfter, amount, modifierBefore, modifierAfter);

				if (learnedSign == CostSign::kBlocked) {
					BDW("blink[{}] cost: this setup refuses the {} cost in both directions, so the blink stays free; another mod or a stuck effect is holding that actor value",
						a_seq, costName);
				} else if (valueAfter > valueBefore - 0.01f) {
					BDW("blink[{}] cost: the {} cost did not land (value {:.1f} -> {:.1f}, damage modifier {:.1f} -> {:.1f})",
						a_seq, costName, valueBefore, valueAfter, modifierBefore, modifierAfter);
				}
			}

			Effects::PlayOnPlayer("blink");
			lastBlinkClock.store(now, std::memory_order_release);
			hasBlinked.store(true, std::memory_order_release);

			snapshot.feet = feet;
			snapshot.direction = direction.dir;
			snapshot.landing = landing;
			snapshot.blocked = blocked;
			snapshot.requested = distance;
			snapshot.travel = travel;
			snapshot.pulls = pulls;
			snapshot.climb = landing.z - feet.z;
			snapshot.summary = std::format(
				"blink #{}  {}  dir {}  source {}\nrequested {:.0f}  travel {:.0f}  blocked {}  pulls {}  height {}\nslope limit {:.0f}deg  landing surface {:.1f}deg  walked over {}  climb {:+.0f}",
				a_seq, grounded ? "on ground" : "airborne", Fmt(direction.dir), direction.source,
				distance, travel, blocked, pulls, heightSource,
				maxSlope, snapshot.groundSlopeDegrees, snapshot.walked, snapshot.climb);
			snapshot.headline = std::format(
				"blink #{} {}  travel {:.0f}/{:.0f}  blocked {}  pulls {}  slope {:.0f}deg  climbed {:+.0f}",
				a_seq, grounded ? "ground" : "air", travel, distance, blocked, pulls,
				snapshot.groundSlopeDegrees, snapshot.climb);
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
