#include "PCH.h"
#include "Effects.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash::Effects
{
	namespace
	{
		constexpr float kBlinkDuration = 0.35f;
		constexpr float kPreviewDuration = 2.0f;

		// TESObjectREFR::ApplyEffectShader -- RELOCATION_ID(19446, 19872).
		// Resolved by hand (instead of calling the library wrapper) so the address can
		// be verified once at load: a RELOCATION_ID slot of 0 has no mapping for the
		// current runtime and would be a jump into null.
		using ApplyEffectShaderFn = decltype(&RE::TESObjectREFR::ApplyEffectShader);

		REL::Relocation<ApplyEffectShaderFn>& ApplyEffectShader()
		{
			static REL::Relocation<ApplyEffectShaderFn> fn{ RELOCATION_ID(19446, 19872) };
			return fn;
		}

		std::vector<Entry> s_entries;
		const Entry*       s_selected{ nullptr };
		bool               s_available{ false };

		std::string SafeEditorID(RE::TESForm* a_form)
		{
			// GetFormEditorID is a virtual, so it needs no address library entry.
			const char* id = a_form ? a_form->GetFormEditorID() : nullptr;
			return (id && *id) ? std::string{ id } : std::string{};
		}

		std::string SourceName(RE::TESForm* a_form)
		{
			if (auto* file = a_form ? a_form->GetFile(0) : nullptr) {
				return std::string{ file->GetFilename() };
			}
			return "<unknown>";
		}

		const Entry* FindByKey(std::string_view a_key)
		{
			if (a_key.empty()) {
				return nullptr;
			}
			for (const auto& entry : s_entries) {
				if (entry.Key() == a_key) {
					return &entry;
				}
			}
			return nullptr;
		}

		// Used only when the config has no shader chosen yet: first match wins, so a
		// fresh install has something visible without opening the menu.
		std::string ToLower(std::string_view a_text)
		{
			std::string out{ a_text };
			for (auto& c : out) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}

		const Entry* FirstMatching(std::initializer_list<std::string_view> a_needles)
		{
			for (const auto needle : a_needles) {
				for (const auto& entry : s_entries) {
					if (entry.editorID.empty()) {
						continue;
					}
					if (ToLower(entry.editorID).find(needle) != std::string::npos) {
						BD("effects: auto-pick candidate '{}' matched '{}'", needle, entry.editorID);
						return &entry;
					}
				}
			}
			return nullptr;
		}
	}

	bool Available()
	{
		return s_available;
	}

	const std::vector<Entry>& Entries()
	{
		return s_entries;
	}

	std::string DescribeSelection()
	{
		if (!s_selected) {
			return "none";
		}
		return std::format("{} (0x{:08X}, {})", s_selected->Key(), s_selected->formID, s_selected->source);
	}

	void RefreshSelection()
	{
		const auto& want = Config::Get().effectShader;
		s_selected = nullptr;

		if (want.empty() || want == "None") {
			BDI("effects: selection is 'none'");
			return;
		}
		if (!s_available) {
			BDW("effects: shader '{}' is set in the config but effects are unavailable", want);
			return;
		}
		if (const auto* found = FindByKey(want)) {
			s_selected = found;
			BDI("effects: selection = {}", DescribeSelection());
			return;
		}

		// "<auto>" (the shipped default) and anything unknown fall back to a probe of
		// the usual suspects, so a first run is not silent. The menu overrides it.
		s_selected = FirstMatching({ "invisib", "blink", "teleport", "summon", "soul" });
		if (s_selected) {
			BDI("effects: '{}' is not a known shader, auto-picked {} - change it in the Skyrim Menu Framework -> Blink Dash",
				want, DescribeSelection());
			return;
		}

		BDW("effects: shader '{}' was not found in the load order, using none", want);
		BDW("effects: {} shaders are available, pick one in the Skyrim Menu Framework -> Blink Dash", s_entries.size());
	}

	void Init()
	{
		BDI("effects: Init() begin");

		const auto address = ApplyEffectShader().address();
		BDI("effects: ApplyEffectShader (RELOCATION_ID(19446, 19872)) resolved to 0x{:X}", address);
		if (address == 0) {
			BDE("effects: ApplyEffectShader has no mapping for this runtime, effect shaders are disabled");
			s_available = false;
			return;
		}
		s_available = true;

		auto* handler = RE::TESDataHandler::GetSingleton();
		BD("effects: TESDataHandler = 0x{:X}", reinterpret_cast<std::uintptr_t>(handler));
		if (!handler) {
			BDE("effects: no TESDataHandler, cannot enumerate effect shaders");
			s_available = false;
			return;
		}

		const auto& forms = handler->GetFormArray<RE::TESEffectShader>();
		BDI("effects: {} TESEffectShader forms in the load order", forms.size());

		s_entries.clear();
		s_entries.reserve(forms.size());
		for (auto* shader : forms) {
			if (!shader) {
				continue;
			}
			Entry entry;
			entry.shader = shader;
			entry.formID = shader->GetFormID();
			entry.editorID = SafeEditorID(shader);
			entry.source = SourceName(shader);
			s_entries.push_back(std::move(entry));
		}

		std::ranges::sort(s_entries, [](const Entry& a_lhs, const Entry& a_rhs) {
			if (a_lhs.editorID.empty() != a_rhs.editorID.empty()) {
				return !a_lhs.editorID.empty();
			}
			if (a_lhs.editorID != a_rhs.editorID) {
				return a_lhs.editorID < a_rhs.editorID;
			}
			return a_lhs.formID < a_rhs.formID;
		});

		for (std::size_t i = 0; i < s_entries.size(); ++i) {
			const auto& entry = s_entries[i];
			BD("effects:   #{} '{}' 0x{:08X} ({})", i, entry.Key(), entry.formID, entry.source);
		}
		BDI("effects: {} effect shaders collected", s_entries.size());

		RefreshSelection();
		BDI("effects: Init() end, selection = {}", DescribeSelection());
	}

	void PlayOnPlayer(const char* a_reason)
	{
		if (!s_available) {
			BD("effects: unavailable, skipping playback ({})", a_reason);
			return;
		}
		if (!s_selected || !s_selected->shader) {
			BD("effects: nothing selected, skipping playback ({})", a_reason);
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			BD("effects: no player singleton, skipping playback ({})", a_reason);
			return;
		}

		const auto fn = ApplyEffectShader();
		if (fn.address() == 0) {
			BDE("effects: ApplyEffectShader address is 0, refusing to call it ({})", a_reason);
			return;
		}

		BD("effects: calling ApplyEffectShader(player=0x{:X}, '{}' 0x{:08X}, duration {:.2f}) [{}]",
			reinterpret_cast<std::uintptr_t>(player), s_selected->Key(), s_selected->formID, kBlinkDuration, a_reason);
		auto* effect = fn(player, s_selected->shader, kBlinkDuration, nullptr, false, false, nullptr, false);
		BD("effects: ApplyEffectShader returned 0x{:X} [{}]", reinterpret_cast<std::uintptr_t>(effect), a_reason);
	}

	void Preview()
	{
		BDI("effects: Preview() requested");
		if (!s_available) {
			BDW("effects: unavailable, nothing to preview");
			return;
		}
		if (!s_selected || !s_selected->shader) {
			BDW("effects: no shader selected, pick one from the dropdown first");
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			BDW("effects: no player singleton");
			return;
		}

		const auto fn = ApplyEffectShader();
		if (fn.address() == 0) {
			BDE("effects: ApplyEffectShader address is 0, refusing to call it");
			return;
		}

		BDI("effects: previewing '{}' 0x{:08X} for {:.1f}s", s_selected->Key(), s_selected->formID, kPreviewDuration);
		auto* effect = fn(player, s_selected->shader, kPreviewDuration, nullptr, false, false, nullptr, false);
		BD("effects: preview ApplyEffectShader returned 0x{:X}", reinterpret_cast<std::uintptr_t>(effect));
	}
}
