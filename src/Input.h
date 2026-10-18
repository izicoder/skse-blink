#pragma once

#include "PCH.h"
#include "Blink.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash::Input
{
	namespace Detail
	{
		inline std::atomic<int>& RawLogBudget()
		{
			static std::atomic<int> budget{ 300 };
			return budget;
		}

		inline std::string DescribeCode(std::uint32_t a_code)
		{
			if ((a_code & 0x100) != 0) {
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

				auto* button = event->AsButtonEvent();
				if (!button) {
					continue;
				}

				const auto device = button->device.get();
				std::uint32_t code = button->idCode;
				if (device == RE::INPUT_DEVICE::kMouse) {
					code |= 0x100;
				} else if (device != RE::INPUT_DEVICE::kKeyboard) {
					continue;
				}

				if (button->IsDown()) {
					++downCount;
					if (const int left = Detail::RawLogBudget().fetch_sub(1); left > 0) {
						BD("input: down event code=0x{:X} ({}) device={} value={:.3f} held={:.3f}",
							code, Detail::DescribeCode(code), static_cast<int>(device), button->value, button->heldDownSecs);
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
						BDI("input: hotkey DOWN ({}), release within {:.0f}ms to blink",
							Detail::DescribeCode(code), settings.retriggerDelay * 1000.0f);
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

					if (heldTime <= settings.retriggerDelay) {
						tapCount.fetch_add(1, std::memory_order_relaxed);
						BDI("input: hotkey TAP (held {:.0f}ms <= {:.0f}ms window) -> RequestBlink",
							heldTime * 1000.0f, settings.retriggerDelay * 1000.0f);
						RequestBlink("hotkey-tap");
					} else {
						longHoldCount.fetch_add(1, std::memory_order_relaxed);
						BDI("input: hotkey held {:.0f}ms > {:.0f}ms window, NOT blinking (sprint/jump style hold)",
							heldTime * 1000.0f, settings.retriggerDelay * 1000.0f);
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
	}
}
