#include "PCH.h"
#include "UI.h"
#include "Blink.h"
#include "Config.h"
#include "Effects.h"
#include "Log.h"
#include "SKSEMenuFramework.h"

// The vendored SKSEMenuFramework.h exposes the ImGui API in namespace ImGuiMCP;
// alias it so call sites read like normal ImGui code.
namespace ImGui = ImGuiMCP;

namespace BlinkDash::UI
{
	namespace
	{
		constexpr float kMinDistance = 100.0f;
		constexpr float kMaxDistance = 3000.0f;
		constexpr float kMinRetriggerMs = 20.0f;
		constexpr float kMaxRetriggerMs = 1000.0f;

		// Sliders fire every frame while dragging, so writes are debounced; anything
		// discrete (checkbox, dropdown, rebind, reset) saves immediately, and a
		// pending write is flushed when the menu closes.
		constexpr float kSaveDebounce = 0.5f;

		std::atomic<bool>         capturing{ false };
		std::atomic<int>          renderFrames{ 0 };
		std::atomic<bool>         pendingSave{ false };
		std::atomic<std::int64_t> lastChangeTicks{ 0 };

		float SecondsSince(std::int64_t a_ticks)
		{
			const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
			return static_cast<float>(now - a_ticks) / static_cast<float>(std::chrono::steady_clock::period::den) *
				static_cast<float>(std::chrono::steady_clock::period::num);
		}

		void MarkChanged(const char* a_what)
		{
			lastChangeTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
			pendingSave.store(true, std::memory_order_release);
			BD("ui: {} changed, config will be saved", a_what);
		}

		void SaveNow(const char* a_reason)
		{
			pendingSave.store(false, std::memory_order_release);
			BDI("ui: saving config ({}) values: {}", a_reason, Config::Describe());
			Config::Save();
		}

		void FlushPendingSave(bool a_force, const char* a_reason)
		{
			if (!pendingSave.load(std::memory_order_acquire)) {
				return;
			}
			if (!a_force && SecondsSince(lastChangeTicks.load(std::memory_order_acquire)) < kSaveDebounce) {
				return;
			}
			SaveNow(a_reason);
		}

		// Stable storage for the effect shader dropdown (ImGui keeps the pointers).
		std::vector<std::string> labelStorage;
		std::vector<const char*> labelPtrs;

		void EnsureEffectLabels()
		{
			const auto& entries = Effects::Entries();
			if (labelPtrs.size() == entries.size() + 1) {
				return;
			}

			labelStorage.clear();
			labelStorage.reserve(entries.size() + 1);
			labelStorage.emplace_back("None");
			for (const auto& entry : entries) {
				labelStorage.push_back(entry.Label());
			}

			labelPtrs.clear();
			labelPtrs.reserve(labelStorage.size());
			for (auto& label : labelStorage) {
				labelPtrs.push_back(label.c_str());
			}
			BDI("ui: effect shader dropdown built with {} entries", labelPtrs.size());
		}

		int CurrentEffectIndex()
		{
			const auto& want = Config::Get().effectShader;
			if (want.empty()) {
				return 0;
			}
			const auto& entries = Effects::Entries();
			for (std::size_t i = 0; i < entries.size(); ++i) {
				if (entries[i].Key() == want) {
					return static_cast<int>(i) + 1;
				}
			}
			return 0;
		}

		void __stdcall Render()
		{
			const int frame = renderFrames.fetch_add(1) + 1;
			auto& settings = Config::Get();

			if (frame <= 5) {
				BD("ui: Render() frame {} (capturing={}) values: {}", frame, capturing.load(), Config::Describe());
			}

			ImGui::TextWrapped("Blink in the direction you are holding (WASD or stick). Hold a direction, tap the hotkey.");
			ImGui::Spacing();

			ImGui::Separator();
			if (capturing.load()) {
				ImGui::Text("Press any key or mouse button...");
				SKSEMenuFramework::SetHotkeyEnabled(false);
			} else {
				const bool isMouse = (settings.hotkey & 0x100) != 0;
				const std::uint32_t raw = settings.hotkey & 0xFF;
				if (isMouse && raw < 8) {
					static constexpr const char* mouseNames[8] = { "Left Mouse", "Right Mouse", "Middle Mouse", "Mouse 4", "Mouse 5", "Mouse 6", "Mouse 7", "Mouse 8" };
					ImGui::Text("Hotkey: %s", mouseNames[raw]);
				} else {
					ImGui::Text("Hotkey: 0x%02X", raw);
				}
				if (ImGui::Button("Rebind")) {
					BDI("ui: Rebind pressed, waiting for input");
					capturing.store(true);
				}
			}

			ImGui::Separator();

			if (ImGui::SliderFloat("Distance (units)", &settings.distance, kMinDistance, kMaxDistance, "%.0f")) {
				BD("ui: distance -> {:.0f}", settings.distance);
				MarkChanged("distance");
			}
			if (ImGui::SliderFloat("Stamina cost", &settings.staminaCost, 0.0f, 100.0f, "%.0f")) {
				BD("ui: staminaCost -> {:.0f}", settings.staminaCost);
				MarkChanged("staminaCost");
			}

			ImGui::Spacing();
			ImGui::TextWrapped("Retrigger delay: the hotkey must be pressed AND released inside this window. Holding it longer does nothing, so it is safe to put on sprint or jump.");
			float retriggerMs = settings.retriggerDelay * 1000.0f;
			if (ImGui::SliderFloat("Retrigger delay (ms)", &retriggerMs, kMinRetriggerMs, kMaxRetriggerMs, "%.0f")) {
				settings.retriggerDelay = retriggerMs / 1000.0f;
				BD("ui: retriggerDelay -> {:.0f}ms ({:.3f}s)", retriggerMs, settings.retriggerDelay);
				MarkChanged("retriggerDelay");
			}

			ImGui::Separator();
			ImGui::TextWrapped("Blink effect: a vanilla effect shader played on you when you blink. No ESP, no new records.");
			if (!Effects::Available()) {
				ImGui::Text("Effect shaders unavailable on this runtime.");
			} else {
				EnsureEffectLabels();
				int current = CurrentEffectIndex();
				if (ImGui::Combo("Effect shader", &current, labelPtrs.data(), static_cast<int>(labelPtrs.size()))) {
					settings.effectShader = (current <= 0) ? std::string{ "None" } : Effects::Entries()[static_cast<std::size_t>(current) - 1].Key();
					BDI("ui: effect shader changed -> '{}'", settings.effectShader);
					Effects::RefreshSelection();
					SaveNow("effect shader changed");
				}
			}

			ImGui::Spacing();
			if (ImGui::Button("Test blink")) {
				BDI("ui: Test blink pressed, queueing a blink request (still needs a held direction)");
				RequestBlink("menu-test-button");
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset defaults")) {
				BDI("ui: Reset defaults pressed (was: {})", Config::Describe());

				// Reset everything the menu owns, not just the config block: the hotkey
				// capture and the resolved effect shader are separate state and would
				// otherwise keep the old selection alive.
				settings = Settings{};
				if (capturing.exchange(false)) {
					SKSEMenuFramework::SetHotkeyEnabled(true);
					BD("ui: Reset defaults cancelled the active hotkey capture");
				}
				Effects::RefreshSelection();
				SaveNow("reset defaults");

				BDI("ui: Reset defaults done (now: {}), effect selection is now '{}'",
					Config::Describe(), Effects::DescribeSelection());
			}

			ImGui::Spacing();
			ImGui::TextWrapped("Everything saves automatically.");

			FlushPendingSave(false, "slider settled");
		}

		// While capturing, listen to raw input via the framework's input hook.
		bool __stdcall InputListener(RE::InputEvent* a_event)
		{
			if (!capturing.load() || !a_event) {
				return false;
			}

			for (auto event = a_event; event; event = event->next) {
				const auto button = event->AsButtonEvent();
				if (!button || !button->IsDown()) {
					continue;
				}

				std::uint32_t code = button->idCode;
				if (button->device.get() == RE::INPUT_DEVICE::kMouse) {
					code |= 0x100;
				} else if (button->device.get() != RE::INPUT_DEVICE::kKeyboard) {
					continue;
				}

				BDI("ui: captured new hotkey 0x{:X} (device={}, idCode={})",
					code, static_cast<int>(button->device.get()), button->idCode);
				Config::Get().hotkey = code;
				capturing.store(false);
				SKSEMenuFramework::SetHotkeyEnabled(true);
				SaveNow("hotkey rebound");
				BD("ui: hotkey capture finished, framework hotkey re-enabled");
				return true;  // swallow
			}
			return false;
		}

		std::unique_ptr<SKSEMenuFramework::Model::InputEvent> inputEvent;
		std::unique_ptr<SKSEMenuFramework::Model::Event>      menuEvent;

		// A slider that was still being dragged when the menu closes would otherwise
		// lose its debounced write.
		void __stdcall OnMenuEvent(SKSEMenuFramework::Model::EventType a_type)
		{
			if (a_type == SKSEMenuFramework::Model::kCloseMenu) {
				BD("ui: menu closed");
				FlushPendingSave(true, "menu closed");
			}
		}
	}

	void Register()
	{
		BDI("ui: Register() begin");

		const bool installed = SKSEMenuFramework::IsInstalled();
		BD("ui: SKSEMenuFramework.dll present = {}", installed);
		if (!installed) {
			BDW("ui: SKSE Menu Framework not detected, menu disabled (hotkey still works)");
			return;
		}

		BD("ui: menu framework version = {:.2f}", SKSEMenuFramework::GetMenuFrameworkVersion());

		SKSEMenuFramework::SetSection("Blink Dash");
		BD("ui: section set to 'Blink Dash'");

		SKSEMenuFramework::AddSectionItem("Settings", Render);
		BDI("ui: section item 'Settings' added");

		inputEvent = std::make_unique<SKSEMenuFramework::Model::InputEvent>(&InputListener);
		BD("ui: input event hook registered (0x{:X})", reinterpret_cast<std::uintptr_t>(inputEvent.get()));

		menuEvent = std::unique_ptr<SKSEMenuFramework::Model::Event>(SKSEMenuFramework::AddEvent(&OnMenuEvent, 0.0f));
		BD("ui: menu event hook registered (0x{:X})", reinterpret_cast<std::uintptr_t>(menuEvent.get()));

		BDI("ui: Register() end");
	}
}
