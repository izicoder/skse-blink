#include "PCH.h"
#include "Blink.h"
#include "Config.h"
#include "Log.h"
#include "SKSEMenuFramework.h"

namespace BlinkDash
{
	namespace
	{
		// Internal tuning that is not exposed in the menu.
		constexpr float kChestHeight = 64.0f;      // ray origin above the feet
		constexpr float kMaxDistanceCap = 2020.0f;
		constexpr float kClearance = 40.0f;        // pull back from a wall we hit
		constexpr float kMinTravel = 30.0f;        // shorter than this is treated as "wedged into a wall"
		constexpr float kInputDeadzone = 0.15f;    // movement input below this counts as "not holding a direction"
		constexpr float kVelocityThreshold = 25.0f;  // units/second below this counts as standing still

		// Dash gate: while true, new requests are ignored.
		std::atomic<bool> blinkGate{ false };
		std::atomic<bool> blinkQueued{ false };
		std::atomic<std::uint64_t> requestSeq{ 0 };
		std::atomic<std::uint64_t> ignoreCount{ 0 };

		std::string Fmt(const RE::NiPoint3& a_point)
		{
			return std::format("({:.1f}, {:.1f}, {:.1f})", a_point.x, a_point.y, a_point.z);
		}

		std::string FmtHotkey(std::uint32_t a_code)
		{
			if ((a_code & 0x100) != 0) {
				switch (a_code & 0xFF) {
				case 0: return "mouse left (0x100)";
				case 1: return "mouse right (0x101)";
				case 2: return "mouse middle (0x102)";
				case 3: return "mouse 4 (0x103)";
				case 4: return "mouse 5 (0x104)";
				default: return std::format("mouse button {} (0x{:X})", a_code & 0xFF, a_code);
				}
			}
			return std::format("keyboard scancode 0x{:02X}", a_code & 0xFF);
		}

		float Now()
		{
			static const auto start = std::chrono::steady_clock::now();
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
		}

		struct DirectionResult
		{
			RE::NiPoint3 dir{};
			const char*  source{ "unresolved" };
			bool         ok{ false };
		};

		// Blink direction = the direction the movement input (WASD or stick) is
		// pointing, in world space. No key codes are involved: the engine's own
		// PlayerControls::data.moveInputVec is what WASD / the stick produce, and
		// the camera yaw is the frame that input is expressed in.
		DirectionResult ResolveDirection(RE::PlayerCharacter* a_player)
		{
			DirectionResult result;

			auto* controls = RE::PlayerControls::GetSingleton();
			BD("  direction: PlayerControls=0x{:X}", reinterpret_cast<std::uintptr_t>(controls));

			float inputX = 0.0f;
			float inputY = 0.0f;
			if (controls) {
				inputX = controls->data.moveInputVec.x;
				inputY = controls->data.moveInputVec.y;
			}
			const float inputMagnitude = std::sqrt(inputX * inputX + inputY * inputY);
			BD("  direction: moveInputVec = (x {:.3f}, y {:.3f}) magnitude {:.3f}",
				inputX, inputY, inputMagnitude);

			if (inputMagnitude < kInputDeadzone) {
				result.source = "no movement input (not holding a direction)";
				BD("  direction: REJECTED, {}", result.source);
				return result;
			}

			float cameraYaw = 0.0f;
			bool haveCamera = false;
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera) {
				cameraYaw = camera->GetRuntimeData2().yaw;
				haveCamera = true;
			}
			BD("  direction: PlayerCamera=0x{:X} cameraYaw={:.4f} rad ({:.1f} deg)",
				reinterpret_cast<std::uintptr_t>(camera), cameraYaw, cameraYaw * 57.29578f);

			// The horizontal velocity is what the engine is actually doing with
			// that input, so it wins whenever the character is really moving.
			RE::NiPoint3 velocity{};
			a_player->GetLinearVelocity(velocity);
			velocity.z = 0.0f;
			const float speed = velocity.Length();
			BD("  direction: horizontal velocity = {} speed = {:.1f}", Fmt(velocity), speed);

			if (speed > kVelocityThreshold) {
				result.dir = velocity;
				result.source = "horizontal velocity";
			} else {
				const float yaw = haveCamera ? cameraYaw : a_player->data.angle.z;
				const RE::NiPoint3 forward{ std::sin(yaw), std::cos(yaw), 0.0f };
				const RE::NiPoint3 right{ std::cos(yaw), -std::sin(yaw), 0.0f };
				result.dir = forward * inputY + right * inputX;
				result.source = haveCamera ? "camera yaw + movement input" : "player facing + movement input";
				BD("  direction: standing still, using yaw={:.4f} forward={} right={}", yaw, Fmt(forward), Fmt(right));
			}

			result.dir.z = 0.0f;
			const float length = result.dir.Unitize();
			BD("  direction: candidate {} from {} (length before normalize {:.4f})",
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

			BD("  ray[{}]: calling bhkWorld::GetWorldScale()", a_label);
			const float scale = RE::bhkWorld::GetWorldScale();
			BD("  ray[{}]: world scale = {:.6f}", a_label, scale);

			RE::bhkPickData pick{};
			const RE::NiPoint3 scaledFrom = a_from * scale;
			const RE::NiPoint3 scaledTo = a_to * scale;
			pick.rayInput.from = scaledFrom;
			pick.rayInput.to = scaledTo;
			pick.rayInput.filterInfo.filter = a_filter;
			BD("  ray[{}]: from={} to={} filter=0x{:08X}", a_label, Fmt(a_from), Fmt(a_to), a_filter);
			BD("  ray[{}]: scaled from={} scaled to={}", a_label, Fmt(scaledFrom), Fmt(scaledTo));

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				BDE("  ray[{}]: no player singleton, treating the path as clear", a_label);
				result.point = a_to;
				return result;
			}

			auto* cell = player->GetParentCell();
			BD("  ray[{}]: parent cell=0x{:X}", a_label, reinterpret_cast<std::uintptr_t>(cell));

			RE::bhkWorld* world = cell ? cell->GetbhkWorld() : nullptr;
			BD("  ray[{}]: bhk world=0x{:X}", a_label, reinterpret_cast<std::uintptr_t>(world));
			if (!world) {
				BDW("  ray[{}]: no havok world for this cell, treating the path as clear", a_label);
				result.point = a_to;
				return result;
			}

			BD("  ray[{}]: calling bhkWorld::PickObject()", a_label);
			world->PickObject(pick);
			BD("  ray[{}]: PickObject returned, hasHit={} hitFraction={:.4f} pickFailed={}",
				a_label, pick.rayOutput.HasHit(), pick.rayOutput.hitFraction, pick.pickFailed);

			result.hit = pick.rayOutput.HasHit();
			result.fraction = result.hit ? pick.rayOutput.hitFraction : 1.0f;
			result.point = result.hit ? (a_from + (a_to - a_from) * pick.rayOutput.hitFraction) : a_to;
			return result;
		}

		bool CanBlinkNow(RE::PlayerCharacter* a_player, const char* a_source)
		{
			BD("blink gate checks (source={}):", a_source);

			BD("  player=0x{:X}", reinterpret_cast<std::uintptr_t>(a_player));
			if (!a_player) {
				BD("  -> REJECTED: no player singleton");
				return false;
			}

			BD("  calling player->Get3D()");
			auto* object3D = a_player->Get3D();
			BD("  player 3D=0x{:X}", reinterpret_cast<std::uintptr_t>(object3D));
			if (!object3D) {
				BD("  -> REJECTED: player has no 3D loaded");
				return false;
			}

			BD("  calling player->IsDead()");
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
			BD("  UI=0x{:X}", reinterpret_cast<std::uintptr_t>(ui));
			if (ui) {
				BD("  calling ui->IsMenuOpen(Console)");
				const bool consoleOpen = ui->IsMenuOpen(RE::Console::MENU_NAME);
				BD("  console menu open={}", consoleOpen);
				if (consoleOpen) {
					BD("  -> REJECTED: console is open");
					return false;
				}
				BD("  calling ui->GameIsPaused()");
				const bool paused = ui->GameIsPaused();
				BD("  game paused={}", paused);
				if (paused) {
					BD("  -> REJECTED: game is paused");
					return false;
				}
			}

			if (SKSEMenuFramework::IsInstalled()) {
				BD("  calling SKSEMenuFramework::IsAnyBlockingWindowOpened()");
				const bool blocking = SKSEMenuFramework::IsAnyBlockingWindowOpened();
				BD("  blocking menu window open={}", blocking);
				if (blocking) {
					BD("  -> REJECTED: a blocking menu window is open");
					return false;
				}
			} else {
				BD("  menu framework not installed, skipping the blocking window check");
			}

			BD("  -> all gate checks passed");
			return true;
		}

		bool ExecuteBlink(std::uint64_t a_seq, const char* a_source)
		{
			BD("blink[{}] ===== ExecuteBlink begin (source={}, thread={}) =====", a_seq, a_source, Log::ThreadId());

			const auto& settings = Config::Get();
			BD("blink[{}] settings: {}", a_seq, Config::Describe());

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

			if (settings.staminaCost > 0.0f) {
				BD("blink[{}] stamina check: calling player->GetActorValue(kStamina)", a_seq);
				const float stamina = player->GetActorValue(RE::ActorValue::kStamina);
				BD("blink[{}] stamina={:.1f} cost={:.1f}", a_seq, stamina, settings.staminaCost);
				if (stamina < settings.staminaCost) {
					BD("blink[{}] REJECTED: not enough stamina", a_seq);
					return false;
				}
			} else {
				BD("blink[{}] no stamina cost configured, skipping the stamina check", a_seq);
			}

			const auto direction = ResolveDirection(player);
			BD("blink[{}] direction ok={} source='{}' dir={}", a_seq, direction.ok, direction.source, Fmt(direction.dir));
			if (!direction.ok) {
				BDI("blink[{}] REJECTED: no blink direction ({})", a_seq, direction.source);
				return false;
			}

			BD("blink[{}] calling player->GetPosition()", a_seq);
			const RE::NiPoint3 feet = player->GetPosition();
			BD("blink[{}] feet position = {}", a_seq, Fmt(feet));

			const RE::NiPoint3 rayStart = feet + RE::NiPoint3{ 0.0f, 0.0f, kChestHeight };
			BD("blink[{}] ray start = {} (feet + {:.0f})", a_seq, Fmt(rayStart), kChestHeight);

			const float distance = std::clamp(settings.distance, 0.0f, kMaxDistanceCap);
			if (distance < kMinTravel) {
				BD("blink[{}] REJECTED: configured distance {:.0f} is below the minimum travel {:.0f}",
					a_seq, distance, kMinTravel);
				return false;
			}
			BD("blink[{}] distance request={:.1f} clamped={:.1f} (cap {:.1f})", a_seq, settings.distance, distance, kMaxDistanceCap);

			std::uint32_t rawFilter = 0;
			BD("blink[{}] calling player->GetCollisionFilterInfo()", a_seq);
			{
				RE::CFilter cfilter;
				player->GetCollisionFilterInfo(cfilter);
				rawFilter = cfilter.filter;
				BD("blink[{}] raw collision filter = 0x{:08X}", a_seq, rawFilter);
			}
			const auto rayFilter = BuildRayFilter(rawFilter);

			BD("blink[{}] --- forward ray ---", a_seq);
			const auto forward = Raycast("forward", rayStart, rayStart + direction.dir * distance, rayFilter);
			BD("blink[{}] forward ray: hit={} fraction={:.4f} point={}",
				a_seq, forward.hit, forward.fraction, Fmt(forward.point));

			RE::NiPoint3 landing = forward.point;
			if (forward.hit) {
				landing = landing - direction.dir * kClearance;
				BD("blink[{}] wall hit, pulled back by clearance {:.0f} -> {}", a_seq, kClearance, Fmt(landing));
			}

			const float travel = (landing - rayStart).Length();
			BD("blink[{}] travel from the ray start = {:.1f} (minimum {:.1f})", a_seq, travel, kMinTravel);
			if (travel < kMinTravel) {
				BDI("blink[{}] REJECTED: only {:.1f} units of room (wedged against geometry)", a_seq, travel);
				return false;
			}

			// Horizontal blink: keep the current height, no ground probing at all.
			landing.z = feet.z;
			BD("blink[{}] landing point (height forced to the current feet height) = {}", a_seq, Fmt(landing));

			BD("blink[{}] calling player->SetPosition({}, true)", a_seq, Fmt(landing));
			player->SetPosition(landing, true);

			const RE::NiPoint3 after = player->GetPosition();
			BD("blink[{}] position after the teleport = {} (delta {})", a_seq, Fmt(after), Fmt(after - feet));

			if (settings.staminaCost > 0.0f) {
				BD("blink[{}] calling player->DamageActorValue(kStamina, {:.1f})", a_seq, settings.staminaCost);
				player->DamageActorValue(RE::ActorValue::kStamina, settings.staminaCost);
				BD("blink[{}] stamina after = {:.1f}", a_seq, player->GetActorValue(RE::ActorValue::kStamina));
			}

			BDI("blink[{}] SUCCESS source={} direction={} travel={:.1f} wallHit={} from={} to={}",
				a_seq, a_source, direction.source, travel, forward.hit, Fmt(feet), Fmt(after));
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
			BD("request[{}]: task interface 0x{:X}, queueing GameThreadBlink",
				seq, reinterpret_cast<std::uintptr_t>(task));
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
