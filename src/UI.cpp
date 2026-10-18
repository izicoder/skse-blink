#include "PCH.h"
#include "UI.h"
#include "Blink.h"
#include "Config.h"
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

		std::atomic<bool> capturing{ false };
		std::atomic<int>  renderFrames{ 0 };

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
			}
			if (ImGui::SliderFloat("Stamina cost", &settings.staminaCost, 0.0f, 100.0f, "%.0f")) {
				BD("ui: staminaCost -> {:.0f}", settings.staminaCost);
			}

			ImGui::Spacing();
			ImGui::TextWrapped("Retrigger delay: the hotkey must be pressed AND released inside this window. Holding it longer does nothing, so it is safe to put on sprint or jump.");
			float retriggerMs = settings.retriggerDelay * 1000.0f;
			if (ImGui::SliderFloat("Retrigger delay (ms)", &retriggerMs, kMinRetriggerMs, kMaxRetriggerMs, "%.0f")) {
				settings.retriggerDelay = retriggerMs / 1000.0f;
				BD("ui: retriggerDelay -> {:.0f}ms ({:.3f}s)", retriggerMs, settings.retriggerDelay);
			}

			ImGui::Spacing();
			if (ImGui::Button("Save")) {
				BDI("ui: Save pressed, current values: {}", Config::Describe());
				Config::Save();
			}
			ImGui::SameLine();
			if (ImGui::Button("Test blink")) {
				BDI("ui: Test blink pressed, queueing a blink request (still needs a held direction)");
				RequestBlink("menu-test-button");
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset defaults")) {
				BDI("ui: Reset defaults pressed (was: {})", Config::Describe());
				settings = Settings{};
				BDI("ui: Reset defaults done (now: {})", Config::Describe());
			}
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
				Config::Save();
				BD("ui: hotkey capture finished, framework hotkey re-enabled");
				return true;  // swallow
			}
			return false;
		}

		std::unique_ptr<SKSEMenuFramework::Model::InputEvent> inputEvent;
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

		BDI("ui: Register() end");
	}
}
