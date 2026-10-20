#pragma once

#include "PCH.h"
#include "Blink.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash::Input
{
	// Raw device state for the current frame, in the encoding the hotkey uses:
	//   0x0000..0x00FF  keyboard scancode
	//   0x0100..0x01FF  mouse button (0x100 | button)
	//   0x1000..0x100F  gamepad button (index into the gamepad button table)
	//   0x2000 | raw    gamepad button the table does not know
	namespace State
	{
		constexpr std::uint32_t kMouseFlag = 0x0100;
		constexpr std::uint32_t kGamepadFlag = 0x1000;
		constexpr std::uint32_t kGamepadRawFlag = 0x2000;
		constexpr std::uint32_t kUnknownCode = 0xFFFFFFFF;

		constexpr std::uint32_t kCodeCount = kGamepadFlag + 16;
		constexpr std::uint32_t kWordCount = (kCodeCount + 63) / 64;

		// ButtonEvent::idCode for a gamepad button is the XInput mask (0x0001..0x8000),
		// with the two triggers and the sticks on their own small ids. Index them so the
		// encoding is compact and matches SKSE::InputMap's gamepad order.
		constexpr std::uint32_t kGamepadButtonIds[] = {
			0x0001, 0x0002, 0x0004, 0x0008,  // dpad up / down / left / right
			0x0010, 0x0020,                  // start / back
			0x0040, 0x0080,                  // left / right thumb click
			0x0100, 0x0200,                  // left / right shoulder
			0x1000, 0x2000, 0x4000, 0x8000,  // A / B / X / Y
			0x0009, 0x000A                   // left / right trigger
		};

		// -1 when the id is not a gamepad button we know.
		inline int GamepadButtonIndex(std::uint32_t a_id)
		{
			for (std::size_t i = 0; i < std::size(kGamepadButtonIds); ++i) {
				if (kGamepadButtonIds[i] == a_id) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		// Turns a raw device id into the code above. kUnknownCode when it is not one.
		inline std::uint32_t Encode(RE::INPUT_DEVICE a_device, std::uint32_t a_id)
		{
			if (a_id == RE::ControlMap::kInvalid) {
				return kUnknownCode;
			}
			switch (a_device) {
			case RE::INPUT_DEVICE::kKeyboard:
				return a_id & 0xFF;
			case RE::INPUT_DEVICE::kMouse:
				return kMouseFlag | (a_id & 0xFF);
			case RE::INPUT_DEVICE::kGamepad:
				{
					const int index = GamepadButtonIndex(a_id);
					return index >= 0 ? kGamepadFlag + static_cast<std::uint32_t>(index) : kGamepadRawFlag | a_id;
				}
			default:
				return kUnknownCode;
			}
		}

		inline std::atomic<std::uint64_t> s_held[kWordCount]{};

		inline void Set(std::uint32_t a_code, bool a_down)
		{
			const auto word = a_code / 64;
			const auto bit = std::uint64_t{ 1 } << (a_code % 64);
			if (word >= kWordCount) {
				return;
			}
			if (a_down) {
				s_held[word].fetch_or(bit, std::memory_order_relaxed);
			} else {
				s_held[word].fetch_and(~bit, std::memory_order_relaxed);
			}
		}

		[[nodiscard]] inline bool IsHeld(std::uint32_t a_code)
		{
			if (a_code == kUnknownCode) {
				return false;
			}
			const auto word = a_code / 64;
			if (word >= kWordCount) {
				return false;
			}
			return (s_held[word].load(std::memory_order_relaxed) >> (a_code % 64)) & 1;
		}

		// The left stick, straight off the thumbstick events, which is the same kind of
		// raw state as the keys above and wants no translation through the engine.
		inline std::atomic<float>        s_stickX{ 0.0f };
		inline std::atomic<float>        s_stickY{ 0.0f };
		inline std::atomic<bool>         s_stickSeen{ false };
		inline std::atomic<std::int64_t> s_stickTicks{ 0 };
		inline std::atomic<bool>         s_stickStaleLogged{ false };

		inline void SetStick(float a_x, float a_y)
		{
			s_stickX.store(a_x, std::memory_order_relaxed);
			s_stickY.store(a_y, std::memory_order_relaxed);
			s_stickTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
			s_stickSeen.store(true, std::memory_order_relaxed);
			s_stickStaleLogged.store(false, std::memory_order_relaxed);
		}

		// A stick that stopped reporting is not a stick that is still pushed: an unplugged
		// controller leaves the last vector behind forever, and the stick outranks the keyboard.
		// Events arrive continuously while a pad is connected, so two seconds of silence is not
		// a held stick.
		inline bool StickSeen()
		{
			if (!s_stickSeen.load(std::memory_order_relaxed)) {
				return false;
			}
			const auto ticks = s_stickTicks.load(std::memory_order_relaxed);
			if (ticks == 0) {
				return false;
			}
			const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
			const auto age = std::chrono::steady_clock::duration{ now - ticks };
			if (age <= std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(2))) {
				return true;
			}
			if (!s_stickStaleLogged.exchange(true, std::memory_order_relaxed)) {
				BDW("input: no thumbstick event for 2s, dropping the stick (was the controller unplugged?)");
			}
			return false;
		}

		// Drops every held button and the stick. Called when the game stops sending releases -
		// a menu opening, a save loading - because a key that was down at that moment never gets
		// its release event and would otherwise stay "held" for the rest of the session.
		inline void Clear()
		{
			for (auto& word : s_held) {
				word.store(0, std::memory_order_relaxed);
			}
			s_stickX.store(0.0f, std::memory_order_relaxed);
			s_stickY.store(0.0f, std::memory_order_relaxed);
			s_stickSeen.store(false, std::memory_order_relaxed);
			s_stickTicks.store(0, std::memory_order_relaxed);
			s_stickStaleLogged.store(false, std::memory_order_relaxed);
			BD("input: held state cleared");
		}
	}

	namespace Detail
	{
		inline std::atomic<int>& RawLogBudget()
		{
			static std::atomic<int> budget{ 300 };
			return budget;
		}

		inline std::string DescribeCode(std::uint32_t a_code)
		{
			if ((a_code & State::kGamepadRawFlag) != 0) {
				return std::format("gamepad button 0x{:X}", a_code & 0xFFF);
			}
			if ((a_code & State::kGamepadFlag) != 0) {
				return SKSE::InputMap::GetGamepadButtonName(
					SKSE::InputMap::kGamepadButtonOffset_DPAD_UP + (a_code & 0x0F));
			}
			if ((a_code & State::kMouseFlag) != 0) {
				return std::format("mouse button {}", a_code & 0xFF);
			}
			return std::format("key scancode 0x{:02X}", a_code & 0xFF);
		}

		inline float SecondsSince(std::int64_t a_ticks)
		{
			const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
			return static_cast<float>(now - a_ticks) / static_cast<float>(std::chrono::steady_clock::period::den) *
				static_cast<float>(std::chrono::steady_clock::period::num);
		}
	}

	// The hotkey is tap-to-blink: a press and a release inside the configured
	// retrigger delay triggers one blink. Holding it longer does nothing, so the
	// same key can sit on sprint/jump/etc without firing the blink.
	class EventHandler : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static EventHandler* GetSingleton()
		{
			static EventHandler singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!a_event || !*a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}

			const auto& settings = Config::Get();
			std::uint32_t eventCount = 0;
			std::uint32_t downCount = 0;

			for (auto* event = *a_event; event; event = event->next) {
				++eventCount;

				// The left stick is raw device state, the same as the keys: read it here
				// and nowhere else.
				if (auto* stick = event->AsThumbstickEvent()) {
					if (stick->idCode == RE::ThumbstickEvent::InputTypes::kLeftThumbstick) {
						State::SetStick(stick->xValue, stick->yValue);
						if (const int left = Detail::RawLogBudget().fetch_sub(1); left > 0) {
							BD("input: left stick x={:.3f} y={:.3f}", stick->xValue, stick->yValue);
						}
					}
					continue;
				}

				auto* button = event->AsButtonEvent();
				if (!button) {
					continue;
				}

				const std::uint32_t code = State::Encode(button->device.get(), button->GetIDCode());
				if (code == State::kUnknownCode) {
					continue;
				}

				// Track every button, not just the hotkey: the blink direction is built
				// from the movement inputs that are down at the moment it fires.
				State::Set(code, button->Value() > 0.0f);

				if (button->IsDown()) {
					++downCount;
					if (const int left = Detail::RawLogBudget().fetch_sub(1); left > 0) {
						BD("input: down event code=0x{:X} ({}) device={} value={:.3f} held={:.3f}",
							code, Detail::DescribeCode(code), static_cast<int>(button->device.get()), button->Value(), button->HeldDuration());
					}
				}

				if (code != settings.hotkey) {
					continue;
				}

				// ButtonEvent semantics (CommonLibSSE):
				//   IsDown() = value > 0 AND heldDownSecs == 0  -> only the FIRST press event
				//   IsHeld() = value > 0 AND heldDownSecs > 0   -> every repeat while held
				//   IsUp()   = value == 0 AND heldDownSecs > 0  -> the release
				// "not IsDown" therefore means "held repeat", not "released", so the
				// press/release decision is made on Value() alone.
				const auto  nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
				const float value = button->Value();
				const float held = button->HeldDuration();

				if (value > 0.0f) {
					if (held <= 0.0f) {
						pressTicks.store(nowTicks, std::memory_order_release);
						pressCount.fetch_add(1, std::memory_order_relaxed);
						BDI("input: hotkey DOWN ({}), release within {}ms to blink",
							Detail::DescribeCode(code), settings.retriggerMs);
					} else if (held <= kRepeatLogWindow) {
						BD("input: hotkey held repeat at {:.0f}ms (no blink while held)", held * 1000.0f);
					}
				} else {
					const auto  pressed = pressTicks.exchange(0, std::memory_order_acq_rel);
					const bool  hadPress = pressed != 0;
					const float heldTime = held > 0.0f ? held : (hadPress ? Detail::SecondsSince(pressed) : 0.0f);

					if (!hadPress && held <= 0.0f) {
						BD("input: hotkey release with neither a recorded press nor a held duration, ignoring");
						continue;
					}

					// The tap window is configured in whole milliseconds, so the measured hold is
					// rounded to the same unit before it is compared.
					const int heldMs = static_cast<int>(std::lround(heldTime * 1000.0f));
					if (heldMs <= settings.retriggerMs) {
						tapCount.fetch_add(1, std::memory_order_relaxed);
						BDI("input: hotkey TAP (held {}ms <= {}ms window) -> RequestBlink",
							heldMs, settings.retriggerMs);
						RequestBlink("hotkey-tap");
					} else {
						longHoldCount.fetch_add(1, std::memory_order_relaxed);
						BDI("input: hotkey held {}ms > {}ms window, NOT blinking (sprint/jump style hold)",
							heldMs, settings.retriggerMs);
					}
				}
			}

			if (downCount > 0) {
				BD("input: frame processed events={} downs={} (hotkey=0x{:X} presses={} taps={} longholds={})",
					eventCount, downCount, settings.hotkey,
					pressCount.load(), tapCount.load(), longHoldCount.load());
			}

			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		// Held repeats are only logged for the first moments of a hold, so a sprint
		// key does not turn the log into a firehose.
		static constexpr float kRepeatLogWindow = 0.10f;

		std::atomic<std::int64_t>  pressTicks{ 0 };
		std::atomic<std::uint64_t> pressCount{ 0 };
		std::atomic<std::uint64_t> tapCount{ 0 };
		std::atomic<std::uint64_t> longHoldCount{ 0 };
	};

	// The game keeps dispatching input while a menu is up, but a key that was already down when
	// the menu opened never gets its release: the menu eats it. Clearing on open means every
	// blink after a menu starts from "nothing held" instead of drifting in a stale direction.
	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			if (a_event->opening) {
				BD("input: menu '{}' opened, dropping the held state", a_event->menuName.c_str());
				State::Clear();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	inline void Register()
	{
		BDI("input: Register() begin");
		if (auto* manager = RE::BSInputDeviceManager::GetSingleton()) {
			BD("input: BSInputDeviceManager=0x{:X}", reinterpret_cast<std::uintptr_t>(manager));
			manager->AddEventSink(EventHandler::GetSingleton());
			BDI("input: event sink registered (tap to blink, retrigger delay applies)");
		} else {
			BDE("input: BSInputDeviceManager singleton is null, the hotkey will not work");
		}

		if (auto* ui = RE::UI::GetSingleton()) {
			if (auto* source = ui->GetEventSource<RE::MenuOpenCloseEvent>()) {
				source->AddEventSink(MenuSink::GetSingleton());
				BDI("input: menu sink registered (held state is dropped when a menu opens)");
			} else {
				BDW("input: no menu event source, held state survives menus");
			}
		} else {
			BDW("input: no UI singleton, held state survives menus");
		}
	}
}
