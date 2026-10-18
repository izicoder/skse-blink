#include "PCH.h"
#include "UI.h"
#include "Config.h"
#include "Debug.h"
#include "Effects.h"
#include "Input.h"
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
		constexpr float kMinCooldownMs = 0.0f;
		constexpr float kMaxCooldownMs = 2000.0f;
		constexpr float kMinSlopeDeg = 20.0f;
		constexpr float kMaxSlopeDeg = 80.0f;

		// Sliders fire every frame while dragging, so writes are debounced; anything
		// discrete (checkbox, dropdown, rebind, reset) saves immediately, and a
		// pending write is flushed when the menu closes.
		constexpr float kSaveDebounce = 0.5f;

		// Reset wipes every setting, so the second click has to land inside this window.
		constexpr float kResetConfirmSeconds = 4.0f;

		constexpr float kHintIndent = 14.0f;

		// The plan view keeps a floor under its span, so a ten unit blink does not zoom in
		// until the 24 unit body probes look like full length rays.
		constexpr float kMinDiagramSpan = 120.0f;

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
			if ((a_code & Input::State::kGamepadRawFlag) != 0) {
				return std::format("Gamepad button 0x{:X}", a_code & 0xFFF);
			}
			if ((a_code & Input::State::kGamepadFlag) != 0) {
				return SKSE::InputMap::GetGamepadButtonName(
					SKSE::InputMap::kGamepadButtonOffset_DPAD_UP + (a_code & 0x0F));
			}

			const std::uint32_t raw = a_code & 0xFF;
			if ((a_code & Input::State::kMouseFlag) != 0) {
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

		// Which family a recorded segment belongs to, so the drawing can tell them apart.
		enum class RayKind
		{
			kObstacle,  // the body height rays that decide the travel
			kFit,       // the body volume probes at the landing spot
			kGround,    // the vertical ground probes (shown as a tick, they have no XY extent)
			kCeiling    // the vertical probe that catches a low ceiling
		};

		RayKind KindOf(const char* a_label)
		{
			const std::string_view label{ a_label };
			if (label.starts_with("ground")) {
				return RayKind::kGround;
			}
			if (label.starts_with("ceiling")) {
				return RayKind::kCeiling;
			}
			if (label.starts_with("fit")) {
				return RayKind::kFit;
			}
			return RayKind::kObstacle;
		}

		// Diagram palette. Kept here so the overlay and its legend cannot drift apart.
		constexpr ImGui::ImU32 kPanelBg = IM_COL32(10, 10, 14, 215);
		constexpr ImGui::ImU32 kPanelEdge = IM_COL32(95, 95, 115, 170);
		constexpr ImGui::ImU32 kDiagramBg = IM_COL32(16, 17, 22, 225);
		constexpr ImGui::ImU32 kDiagramEdge = IM_COL32(90, 90, 110, 160);
		constexpr ImGui::ImU32 kGrid = IM_COL32(60, 62, 74, 120);
		constexpr ImGui::ImU32 kTitleText = IM_COL32(255, 255, 255, 245);
		constexpr ImGui::ImU32 kBodyText = IM_COL32(214, 216, 226, 235);
		constexpr ImGui::ImU32 kDimText = IM_COL32(150, 154, 168, 220);
		constexpr ImGui::ImU32 kPathClear = IM_COL32(90, 205, 110, 205);
		constexpr ImGui::ImU32 kPathSolid = IM_COL32(235, 85, 85, 255);
		constexpr ImGui::ImU32 kPathSlope = IM_COL32(245, 200, 90, 235);
		constexpr ImGui::ImU32 kFitHit = IM_COL32(240, 140, 60, 235);
		constexpr ImGui::ImU32 kGroundProbe = IM_COL32(110, 175, 245, 235);
		constexpr ImGui::ImU32 kCeilingProbe = IM_COL32(190, 130, 245, 235);
		constexpr ImGui::ImU32 kFeetMark = IM_COL32(255, 210, 80, 255);
		constexpr ImGui::ImU32 kLandingMark = IM_COL32(80, 190, 255, 255);
		constexpr ImGui::ImU32 kDirectionMark = IM_COL32(110, 200, 215, 200);
		constexpr ImGui::ImU32 kAxisText = IM_COL32(190, 190, 200, 215);

		// The plan view of a blink: world XY, north up, scaled to fit the box.
		void DrawBlinkDiagram(ImGui::ImDrawList* a_drawList, const Debug::Snapshot& a_snapshot, const ImGui::ImVec2 a_origin, float a_size)
		{
			if (!a_drawList || a_size < 60.0f) {
				return;
			}

			const ImGui::ImVec2 boxMax{ a_origin.x + a_size, a_origin.y + a_size };

			float minX = a_snapshot.feet.x;
			float maxX = minX;
			float minY = a_snapshot.feet.y;
			float maxY = minY;
			const auto include = [&](const RE::NiPoint3& a_point) {
				minX = (std::min)(minX, a_point.x);
				maxX = (std::max)(maxX, a_point.x);
				minY = (std::min)(minY, a_point.y);
				maxY = (std::max)(maxY, a_point.y);
			};

			include(a_snapshot.landing);
			for (std::size_t i = 0; i < a_snapshot.count; ++i) {
				include(a_snapshot.segments[i].from);
				include(a_snapshot.segments[i].to);
			}

			// A floor under the span, so a blink of ten units does not zoom to the point
			// where the short body probes look like full length rays.
			const float span = (std::max)((std::max)((std::max)(maxX - minX, maxY - minY), kMinDiagramSpan), 1.0f);
			const float pad = 12.0f;
			const float usable = a_size - pad * 2.0f;
			const auto project = [&](const RE::NiPoint3& a_point) {
				const float px = pad + (a_point.x - minX) / span * usable;
				const float py = a_size - pad - (a_point.y - minY) / span * usable;
				return ImGui::ImVec2{ a_origin.x + px, a_origin.y + py };
			};

			ImGui::ImDrawListManager::AddRectFilled(a_drawList, a_origin, boxMax, kDiagramBg, 4.0f, 0);
			ImGui::ImDrawListManager::AddRect(a_drawList, a_origin, boxMax, kDiagramEdge, 4.0f, 0, 1.0f);

			// A faint grid, so the rays can be read against something.
			for (int i = 1; i <= 3; ++i) {
				const float t = static_cast<float>(i) / 4.0f;
				const float gx = a_origin.x + pad + usable * t;
				const float gy = a_origin.y + pad + usable * t;
				ImGui::ImDrawListManager::AddLine(a_drawList, ImGui::ImVec2{ gx, a_origin.y + pad }, ImGui::ImVec2{ gx, boxMax.y - pad }, kGrid, 1.0f);
				ImGui::ImDrawListManager::AddLine(a_drawList, ImGui::ImVec2{ a_origin.x + pad, gy }, ImGui::ImVec2{ boxMax.x - pad, gy }, kGrid, 1.0f);
			}

			// Where the direction came from, drawn first so the probes stay on top.
			const auto         feetAt = project(a_snapshot.feet);
			const RE::NiPoint3 directionTip = a_snapshot.feet + a_snapshot.direction * (span * 0.32f);
			auto               tip = project(directionTip);
			tip.x = std::clamp(tip.x, a_origin.x + pad, boxMax.x - pad);
			tip.y = std::clamp(tip.y, a_origin.y + pad, boxMax.y - pad);
			ImGui::ImDrawListManager::AddLine(a_drawList, feetAt, tip, kDirectionMark, 1.5f);
			{
				const float dx = tip.x - feetAt.x;
				const float dy = tip.y - feetAt.y;
				const float length = (std::max)(std::sqrt(dx * dx + dy * dy), 1.0f);
				const float nx = dx / length;
				const float ny = dy / length;
				ImGui::ImDrawListManager::AddTriangleFilled(a_drawList,
					tip,
					ImGui::ImVec2{ tip.x - nx * 9.0f - ny * 4.5f, tip.y - ny * 9.0f + nx * 4.5f },
					ImGui::ImVec2{ tip.x - nx * 9.0f + ny * 4.5f, tip.y - ny * 9.0f - nx * 4.5f },
					kDirectionMark);
			}

			for (std::size_t i = 0; i < a_snapshot.count; ++i) {
				const auto& segment = a_snapshot.segments[i];
				const auto  at = project(segment.to);

				switch (KindOf(segment.label)) {
				case RayKind::kGround:
					// Vertical: it has no extent in the plan view, so mark the spot.
					ImGui::ImDrawListManager::AddLine(a_drawList,
						ImGui::ImVec2{ at.x, at.y - 7.0f }, ImGui::ImVec2{ at.x, at.y + 7.0f },
						segment.hit ? kGroundProbe : IM_COL32(80, 110, 140, 180), 2.0f);
					break;
				case RayKind::kCeiling:
					ImGui::ImDrawListManager::AddLine(a_drawList,
						ImGui::ImVec2{ at.x - 5.0f, at.y - 5.0f }, ImGui::ImVec2{ at.x + 5.0f, at.y + 5.0f },
						segment.hit ? kCeilingProbe : IM_COL32(90, 80, 120, 180), 2.0f);
					break;
				case RayKind::kFit:
					// Only the ones that found something are worth drawing: they are 28
					// units long against a 300 unit blink.
					if (segment.hit) {
						ImGui::ImDrawListManager::AddLine(a_drawList, project(segment.from), at, kFitHit, 2.0f);
					}
					break;
				case RayKind::kObstacle:
					ImGui::ImDrawListManager::AddLine(a_drawList, project(segment.from), at,
						segment.walked ? kPathSlope : (segment.hit ? kPathSolid : kPathClear), 2.0f);
					break;
				}
			}

			const auto feet = project(a_snapshot.feet);
			const auto landing = project(a_snapshot.landing);
			ImGui::ImDrawListManager::AddLine(a_drawList, feet, landing, IM_COL32(255, 255, 255, 170), 1.5f);

			const auto mark = [&](const ImGui::ImVec2 a_at, ImGui::ImU32 a_colour) {
				ImGui::ImDrawListManager::AddRectFilled(a_drawList,
					ImGui::ImVec2{ a_at.x - 4.0f, a_at.y - 4.0f },
					ImGui::ImVec2{ a_at.x + 4.0f, a_at.y + 4.0f },
					a_colour, 0.0f, 0);
			};
			mark(feet, kFeetMark);
			mark(landing, kLandingMark);

			// A real north arrow (the plan view is north up) and a scale bar, so the
			// picture is not just coloured lines.
			{
				const float cx = boxMax.x - 24.0f;
				const float ny = a_origin.y + 12.0f;
				ImGui::ImDrawListManager::AddLine(a_drawList, ImGui::ImVec2{ cx, ny + 22.0f }, ImGui::ImVec2{ cx, ny + 8.0f }, kAxisText, 1.5f);
				ImGui::ImDrawListManager::AddTriangleFilled(a_drawList,
					ImGui::ImVec2{ cx, ny }, ImGui::ImVec2{ cx - 5.0f, ny + 10.0f }, ImGui::ImVec2{ cx + 5.0f, ny + 10.0f }, kAxisText);
				ImGui::ImDrawListManager::AddText(a_drawList, ImGui::ImVec2{ cx - 4.5f, ny + 24.0f }, kAxisText, "N");
			}

			const float barY = boxMax.y - 9.0f;
			ImGui::ImDrawListManager::AddLine(a_drawList,
				ImGui::ImVec2{ a_origin.x + pad, barY }, ImGui::ImVec2{ boxMax.x - pad, barY }, IM_COL32(150, 150, 168, 200), 1.5f);
			ImGui::ImDrawListManager::AddLine(a_drawList,
				ImGui::ImVec2{ a_origin.x + pad, barY - 4.0f }, ImGui::ImVec2{ a_origin.x + pad, barY + 4.0f }, IM_COL32(150, 150, 168, 200), 1.5f);
			ImGui::ImDrawListManager::AddLine(a_drawList,
				ImGui::ImVec2{ boxMax.x - pad, barY - 4.0f }, ImGui::ImVec2{ boxMax.x - pad, barY + 4.0f }, IM_COL32(150, 150, 168, 200), 1.5f);
			ImGui::ImDrawListManager::AddText(a_drawList,
				ImGui::ImVec2{ a_origin.x + pad, barY - 20.0f }, kAxisText, std::format("{:.0f} units", span).c_str());
		}

		// In-game view: a labelled panel in the top left, drawn on the HUD through the
		// framework's HUD element hook, so it can be read while playing.
		void __stdcall RenderDebugOverlay()
		{
			if (!Config::Get().debugOverlay) {
				return;
			}

			// The framework hands this callback the draw list of whatever is being
			// drawn, and it paints over the game's own UI: with a menu up the panel
			// would sit on top of the cursor the player is trying to use. It is a
			// playing overlay, so it only draws when the game is actually running.
			if (SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
				return;
			}
			auto* ui = RE::UI::GetSingleton();
			if (!ui || ui->GameIsPaused()) {
				return;
			}

			static std::atomic<int> frames{ 0 };
			const int frame = frames.fetch_add(1) + 1;

			Debug::Snapshot snapshot;
			if (!Debug::Latest(snapshot)) {
				return;
			}

			auto* io = ImGui::GetIO();
			auto* drawList = ImGui::GetForegroundDrawList();
			if (!io || !drawList) {
				if (frame <= 3) {
					BDW("overlay: framework handed over no io/draw list, nothing to draw on");
				}
				return;
			}
			if (frame <= 3) {
				BD("overlay: frame {} display {:.0f}x{:.0f}, {} segments recorded",
					frame, io->DisplaySize.x, io->DisplaySize.y, snapshot.count);
			}

			// Clear of the compass, which owns the very top left of the HUD. Two columns:
			// the plan view on the left, the numbers and what hit on the right, the legend
			// spread across the bottom, so nothing has to share a line with anything else.
			const ImGui::ImVec2 panel{ 20.0f, 96.0f };
			constexpr float kPanelWidth = 620.0f;
			constexpr float kPad = 14.0f;
			constexpr float kRowHeight = 19.0f;
			constexpr float kDiagramSize = 216.0f;
			constexpr float kLegendRows = 3.0f;

			const float titleY = panel.y + 10.0f;
			const float contentY = titleY + kRowHeight + 8.0f;
			const float textX = panel.x + kPad;
			const float statsX = textX + kDiagramSize + 20.0f;

			// Everything the right column shows is built before anything is drawn, so the
			// panel can be sized to fit it instead of guessed at.
			std::vector<std::pair<ImGui::ImU32, std::string>> stats;
			stats.emplace_back(kBodyText, std::format("travel {:.0f} of {:.0f} units{}",
				snapshot.travel, snapshot.requested, snapshot.shortened ? "  (shortened)" : ""));
			stats.emplace_back(kBodyText, std::format("blocked {}   body fit pulls {}", snapshot.blocked ? "yes" : "no", snapshot.pulls));
			stats.emplace_back(kBodyText, std::format("direction ({:.2f}, {:.2f})   {}",
				snapshot.direction.x, snapshot.direction.y,
				snapshot.grounded ? (snapshot.slopeFollowed ? "on ground, followed the slope" : "on ground") : "airborne"));
			stats.emplace_back(kBodyText, std::format("climbed {:+.0f} units", snapshot.climb));
			stats.emplace_back(kBodyText, std::format("slope limit {:.0f}deg   surface {:.1f}deg",
				Config::Get().maxSlope, snapshot.groundSlopeDegrees));
			stats.emplace_back(kBodyText, std::format("walked over {} probe{}", snapshot.walked, snapshot.walked == 1 ? "" : "s"));

			std::vector<std::pair<ImGui::ImU32, std::string>> hits;
			for (std::size_t i = 0; i < snapshot.count && hits.size() < 6; ++i) {
				const auto& segment = snapshot.segments[i];
				if (!segment.hit) {
					continue;
				}
				hits.emplace_back(segment.walked ? kPathSlope : kPathSolid,
					std::format("{} hit a {:.0f}deg surface at {:.0f}%", segment.label, segment.slopeDegrees, segment.fraction * 100.0f));
			}
			if (hits.empty()) {
				hits.emplace_back(IM_COL32(120, 220, 140, 235), "nothing hit, the whole path was clear");
			}

			const float statsHeight = kRowHeight * static_cast<float>(stats.size()) + 10.0f + kRowHeight * static_cast<float>(hits.size() + 1);
			const float contentBottom = (std::max)(contentY + kDiagramSize, contentY + statsHeight) + 10.0f;
			const float panelBottom = contentBottom + kRowHeight * kLegendRows + 10.0f;

			ImGui::ImDrawListManager::AddRectFilled(drawList, panel,
				ImGui::ImVec2{ panel.x + kPanelWidth, panelBottom }, kPanelBg, 6.0f, 0);
			ImGui::ImDrawListManager::AddRect(drawList, panel,
				ImGui::ImVec2{ panel.x + kPanelWidth, panelBottom }, kPanelEdge, 6.0f, 0, 1.0f);

			ImGui::ImDrawListManager::AddText(drawList, ImGui::ImVec2{ textX, titleY }, kTitleText, snapshot.headline.c_str());
			ImGui::ImDrawListManager::AddLine(drawList,
				ImGui::ImVec2{ textX, titleY + kRowHeight }, ImGui::ImVec2{ panel.x + kPanelWidth - kPad, titleY + kRowHeight },
				IM_COL32(70, 72, 86, 200), 1.0f);

			DrawBlinkDiagram(drawList, snapshot, ImGui::ImVec2{ textX, contentY }, kDiagramSize);

			// The right column: the numbers, then what actually hit.
			float statsY = contentY;
			for (const auto& [colour, text] : stats) {
				ImGui::ImDrawListManager::AddText(drawList, ImGui::ImVec2{ statsX, statsY }, colour, text.c_str());
				statsY += kRowHeight;
			}
			statsY += 10.0f;
			ImGui::ImDrawListManager::AddText(drawList, ImGui::ImVec2{ statsX, statsY }, kAxisText, "WHAT HIT");
			statsY += kRowHeight;
			for (const auto& [colour, text] : hits) {
				ImGui::ImDrawListManager::AddRectFilled(drawList,
					ImGui::ImVec2{ statsX, statsY + 4.0f }, ImGui::ImVec2{ statsX + 8.0f, statsY + 13.0f }, colour, 1.0f, 0);
				ImGui::ImDrawListManager::AddText(drawList, ImGui::ImVec2{ statsX + 14.0f, statsY }, kBodyText, text.c_str());
				statsY += kRowHeight;
			}

			// Legend across the bottom, three entries per line. Each entry gets its colour
			// as a swatch, so the picture reads without the words.
			constexpr int kLegendCols = 3;
			const float   legendColWidth = (kPanelWidth - kPad * 2.0f) / static_cast<float>(kLegendCols);
			const auto    legend = [&](int a_column, int a_row, ImGui::ImU32 a_colour, const char* a_text) {
				const float lx = textX + legendColWidth * static_cast<float>(a_column);
				const float ly = contentBottom + kRowHeight * static_cast<float>(a_row);
				ImGui::ImDrawListManager::AddRectFilled(drawList,
					ImGui::ImVec2{ lx, ly + 4.0f }, ImGui::ImVec2{ lx + 9.0f, ly + 13.0f }, a_colour, 1.0f, 0);
				ImGui::ImDrawListManager::AddText(drawList, ImGui::ImVec2{ lx + 15.0f, ly }, kDimText, a_text);
			};
			legend(0, 0, kFeetMark, "you");
			legend(0, 1, kLandingMark, "where you land");
			legend(0, 2, kDirectionMark, "push direction");
			legend(1, 0, kPathClear, "ray, clear");
			legend(1, 1, kPathSolid, "ray, solid hit");
			legend(1, 2, kPathSlope, "hit a slope");
			legend(2, 0, kFitHit, "body probe hit");
			legend(2, 1, kGroundProbe, "ground probe");
			legend(2, 2, kCeilingProbe, "ceiling hit");
		}

		void RenderDebug(float a_itemWidth)
		{
			UNUSED(a_itemWidth);
			auto& settings = Config::Get();
			if (ImGui::Checkbox("Draw what the last blink probed", &settings.debugOverlay)) {
				BD("ui: debugOverlay -> {}", settings.debugOverlay);
				MarkChanged("debugOverlay");
			}
			Hint("Draws the last blink in game, top left of the screen, while you play: the rays, the landing spot, the ground probes and the numbers. Nothing is drawn here, and the overlay hides itself while the game is paused or a menu is open so nothing covers the cursor.");

			Debug::Snapshot snapshot;
			if (Debug::Latest(snapshot)) {
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGui::ImGuiCol_Text, kValueColor);
				ImGui::TextUnformatted("Last blink");
				ImGui::PopStyleColor();
				Paragraph(snapshot.summary.c_str());
			} else {
				Hint("No blink has been recorded yet.");
			}
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

			Paragraph("Blink dashes you a short way in the direction you are pushing (WASD or the movement stick).");
			Paragraph("Hold a direction, then tap the hotkey. Standing still it blinks where you face.");

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
			if (ImGui::SliderFloat("Max slope (deg)", &settings.maxSlope, kMinSlopeDeg, kMaxSlopeDeg, "%.0f")) {
				BD("ui: maxSlope -> {:.0f}", settings.maxSlope);
				MarkChanged("maxSlope");
			}
			Hint("Ground steeper than this counts as a wall: the blink stops at it instead of climbing it. Shallower ground is followed for the whole distance, up the hill or down it.");

			ImGui::SetNextItemWidth(itemWidth);
			if (ImGui::SliderFloat("Stamina cost", &settings.staminaCost, 0.0f, kMaxStaminaCost, "%.0f")) {
				BD("ui: staminaCost -> {:.0f}", settings.staminaCost);
				MarkChanged("staminaCost");
			}
			Hint("Stamina drained per blink, and only while you are in combat. 0 blinks for free.");

			ImGui::SetNextItemWidth(itemWidth);
			float tapWindowMs = settings.retriggerDelay * 1000.0f;
			if (ImGui::SliderFloat("Tap window (ms)", &tapWindowMs, kMinTapWindowMs, kMaxTapWindowMs, "%.0f")) {
				settings.retriggerDelay = tapWindowMs / 1000.0f;
				BD("ui: retriggerDelay -> {:.0f}ms ({:.3f}s)", tapWindowMs, settings.retriggerDelay);
				MarkChanged("retriggerDelay");
			}
			Hint("The hotkey has to be pressed and released inside this window to count as a tap.");

			ImGui::SetNextItemWidth(itemWidth);
			float cooldownMs = settings.cooldown * 1000.0f;
			if (ImGui::SliderFloat("Cooldown (ms)", &cooldownMs, kMinCooldownMs, kMaxCooldownMs, "%.0f")) {
				settings.cooldown = cooldownMs / 1000.0f;
				BD("ui: cooldown -> {:.0f}ms ({:.3f}s)", cooldownMs, settings.cooldown);
				MarkChanged("cooldown");
			}
			Hint("How long to wait after a successful blink before the next one. 0 lets every tap blink.");

			ImGui::SeparatorText("Effect");
			Paragraph("A vanilla effect shader played on you when you blink. No ESP, no new records.");
			RenderEffectShader(itemWidth, settings);

			ImGui::SeparatorText("Reset");
			RenderReset(settings);

			ImGui::SeparatorText("Debug");
			RenderDebug(itemWidth);

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

				// Any device: keyboard key, mouse button or gamepad button.
				const auto code = Input::State::Encode(button->device.get(), button->idCode);
				if (code == Input::State::kUnknownCode) {
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
		std::unique_ptr<SKSEMenuFramework::Model::HudElement> overlayElement;

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

		overlayElement = std::unique_ptr<SKSEMenuFramework::Model::HudElement>(SKSEMenuFramework::AddHudElement(&RenderDebugOverlay));
		BD("ui: debug overlay hook registered (0x{:X})", reinterpret_cast<std::uintptr_t>(overlayElement.get()));

		BDI("ui: Register() end");
	}
}
