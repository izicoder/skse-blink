#include "PCH.h"
#include "UI.h"
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
		// Menu ranges. Distance matches Blink.cpp's kMaxDistanceCap and the clamp in
		// Config::Validate, so the slider can never ask for more than the blink will
		// actually travel.
		constexpr float kMinDistance = 100.0f;
		constexpr float kMaxDistance = 2020.0f;
		constexpr float kMaxStaminaCost = 100.0f;
		constexpr float kMinTapWindowMs = 20.0f;
		constexpr float kMaxTapWindowMs = 1000.0f;

		// Sliders fire every frame while dragging, so writes are debounced; anything
		// discrete (checkbox, dropdown, rebind, reset) saves immediately, and a
		// pending write is flushed when the menu closes.
		constexpr float kSaveDebounce = 0.5f;

		// Reset wipes every setting, so the second click has to land inside this window.
		constexpr float kResetConfirmSeconds = 4.0f;

		constexpr float kHintIndent = 14.0f;

		constexpr ImGui::ImVec4 kHintColor{ 0.62f, 0.62f, 0.62f, 1.0f };
		constexpr ImGui::ImVec4 kValueColor{ 0.80f, 0.90f, 1.00f, 1.0f };
		constexpr ImGui::ImVec4 kWarnColor{ 1.00f, 0.80f, 0.30f, 1.0f };

		std::atomic<bool>         capturing{ false };
		std::atomic<int>          renderFrames{ 0 };
		std::atomic<bool>         pendingSave{ false };
		std::atomic<std::int64_t> lastChangeTicks{ 0 };
		std::atomic<std::int64_t> resetArmedTicks{ 0 };

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

		// Dim, wrapped body text. Anything the user does not have to read to use the
		// menu (explanations, caveats) is written this way so the controls stand out.
		void Paragraph(const char* a_text)
		{
			ImGui::PushStyleColor(ImGui::ImGuiCol_Text, kHintColor);
			ImGui::TextWrapped("%s", a_text);
			ImGui::PopStyleColor();
		}

		void Hint(const char* a_text)
		{
			ImGui::Indent(kHintIndent);
			Paragraph(a_text);
			ImGui::Unindent(kHintIndent);
		}

		// Config stores the hotkey the way SKSE::InputMap numbers keys (a keyboard
		// scancode, or 256+N for mouse button N), so the engine's own lookup can name
		// the keyboard half and only the mouse half needs a table here.
		std::string HotkeyLabel(std::uint32_t a_code)
		{
			const std::uint32_t raw = a_code & 0xFF;
			if ((a_code & 0x100) != 0) {
				switch (raw) {
				case 0: return "Left Mouse";
				case 1: return "Right Mouse";
				case 2: return "Middle Mouse";
				default: return std::format("Mouse {}", raw + 1);
				}
			}
			if (auto name = SKSE::InputMap::GetKeyboardKeyName(a_code); !name.empty()) {
				return name;
			}
			return std::format("Key 0x{:02X}", raw);
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

		void RenderHotkey(std::uint32_t& a_hotkey)
		{
			if (capturing.load()) {
				ImGui::PushStyleColor(ImGui::ImGuiCol_Text, kWarnColor);
				ImGui::TextWrapped("Listening... press the key or mouse button you want to use.");
				ImGui::PopStyleColor();
				SKSEMenuFramework::SetHotkeyEnabled(false);
				return;
			}

			const auto label = HotkeyLabel(a_hotkey);
			ImGui::Text("Hotkey:");
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGui::ImGuiCol_Text, kValueColor);
			ImGui::Text("%s", label.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if (ImGui::Button("Rebind")) {
				BDI("ui: Rebind pressed, waiting for input");
				capturing.store(true);
			}

			Hint("Tap to blink: press and release the hotkey. Holding it down does nothing, so sprint or jump keys are safe.");
		}

		void RenderEffectShader(float a_itemWidth, Settings& a_settings)
		{
			if (!Effects::Available()) {
				ImGui::PushStyleColor(ImGui::ImGuiCol_Text, kWarnColor);
				ImGui::TextWrapped("Effect shaders are unavailable on this runtime, so blinks are invisible.");
				ImGui::PopStyleColor();
				return;
			}

			EnsureEffectLabels();
			int current = CurrentEffectIndex();
			ImGui::SetNextItemWidth(a_itemWidth);
			if (ImGui::Combo("Effect shader", &current, labelPtrs.data(), static_cast<int>(labelPtrs.size()))) {
				a_settings.effectShader = (current <= 0) ? std::string{ "None" } : Effects::Entries()[static_cast<std::size_t>(current) - 1].Key();
				BDI("ui: effect shader changed -> '{}'", a_settings.effectShader);
				Effects::RefreshSelection();
				SaveNow("effect shader changed");
			}

			const auto selection = Effects::DescribeSelection();
			if (selection == "none") {
				Hint("No effect plays right now. Pick a shader above, or leave it on None for a silent blink.");
			} else {
				Hint(std::format("Playing {} when you blink.", selection).c_str());
			}
		}

		void RenderReset(Settings& a_settings)
		{
			const auto armed = resetArmedTicks.load(std::memory_order_acquire);
			const bool confirming = armed != 0 && SecondsSince(armed) < kResetConfirmSeconds;

			if (ImGui::Button(confirming ? "Confirm reset" : "Reset defaults")) {
				if (confirming) {
					resetArmedTicks.store(0, std::memory_order_release);
					BDI("ui: Reset defaults confirmed (was: {})", Config::Describe());

					// Reset everything the menu owns, not just the config block: the
					// hotkey capture and the resolved effect shader are separate state
					// and would otherwise keep the old selection alive.
					a_settings = Settings{};
					if (capturing.exchange(false)) {
						SKSEMenuFramework::SetHotkeyEnabled(true);
						BD("ui: Reset defaults cancelled the active hotkey capture");
					}
					Effects::RefreshSelection();
					SaveNow("reset defaults");

					BDI("ui: Reset defaults done (now: {}), effect selection is now '{}'",
						Config::Describe(), Effects::DescribeSelection());
				} else {
					resetArmedTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
					BDI("ui: Reset defaults armed, click again within {:.0f}s to confirm", kResetConfirmSeconds);
				}
			}

			// Re-read the state: a confirmed reset in the branch above just cleared it,
			// and the Cancel button must not linger for the frame that happens on.
			const auto stillArmed = resetArmedTicks.load(std::memory_order_acquire);
			if (stillArmed != 0 && SecondsSince(stillArmed) < kResetConfirmSeconds) {
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					resetArmedTicks.store(0, std::memory_order_release);
					BD("ui: Reset defaults cancelled");
				}
			}

			Hint("Puts every setting above back to its default.");
		}

		void __stdcall Render()
		{
			const int frame = renderFrames.fetch_add(1) + 1;
			auto& settings = Config::Get();

			if (frame <= 5) {
				BD("ui: Render() frame {} (capturing={}) values: {}", frame, capturing.load(), Config::Describe());
			}

			// One width for every control, so the labels line up down the page.
			const float itemWidth = (std::max)(200.0f, ImGui::GetContentRegionAvail().x * 0.5f);

			Paragraph("Blink dashes you a short way in the direction you are holding (WASD or the movement stick).");
			Paragraph("Hold a direction, then tap the hotkey. Standing still does nothing.");

			ImGui::SeparatorText("Activation");
			RenderHotkey(settings.hotkey);

			ImGui::SeparatorText("Blink");
			ImGui::SetNextItemWidth(itemWidth);
			if (ImGui::SliderFloat("Blink distance (units)", &settings.distance, kMinDistance, kMaxDistance, "%.0f")) {
				BD("ui: distance -> {:.0f}", settings.distance);
				MarkChanged("distance");
			}
			Hint("How far one blink travels. Walls shrink it before you reach the target.");

			ImGui::SetNextItemWidth(itemWidth);
			if (ImGui::SliderFloat("Stamina cost", &settings.staminaCost, 0.0f, kMaxStaminaCost, "%.0f")) {
				BD("ui: staminaCost -> {:.0f}", settings.staminaCost);
				MarkChanged("staminaCost");
			}
			Hint("Stamina drained per blink. 0 blinks for free.");

			ImGui::SetNextItemWidth(itemWidth);
			float tapWindowMs = settings.retriggerDelay * 1000.0f;
			if (ImGui::SliderFloat("Tap window (ms)", &tapWindowMs, kMinTapWindowMs, kMaxTapWindowMs, "%.0f")) {
				settings.retriggerDelay = tapWindowMs / 1000.0f;
				BD("ui: retriggerDelay -> {:.0f}ms ({:.3f}s)", tapWindowMs, settings.retriggerDelay);
				MarkChanged("retriggerDelay");
			}
			Hint("The hotkey has to be pressed and released inside this window to count as a tap.");

			ImGui::SeparatorText("Effect");
			Paragraph("A vanilla effect shader played on you when you blink. No ESP, no new records.");
			RenderEffectShader(itemWidth, settings);

			ImGui::SeparatorText("Reset");
			RenderReset(settings);

			ImGui::Spacing();
			Paragraph("Every change saves itself; there is nothing to apply.");

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
