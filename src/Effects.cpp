#include "PCH.h"
#include "Effects.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash::Effects
{
	namespace
	{
		constexpr float kBlinkDuration = 0.35f;
		constexpr const char* kNameFileName = "BlinkDash.ShaderNames.json";

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

		std::vector<Entry>                          s_entries;
		std::unordered_map<std::string, std::string> s_names;
		const Entry*                                s_selected{ nullptr };
		bool                                        s_available{ false };

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

		bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs)
		{
			if (a_lhs.size() != a_rhs.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_lhs.size(); ++i) {
				const char l = (a_lhs[i] >= 'A' && a_lhs[i] <= 'Z') ? static_cast<char>(a_lhs[i] - 'A' + 'a') : a_lhs[i];
				const char r = (a_rhs[i] >= 'A' && a_rhs[i] <= 'Z') ? static_cast<char>(a_rhs[i] - 'A' + 'a') : a_rhs[i];
				if (l != r) {
					return false;
				}
			}
			return true;
		}

		std::string SourceName(RE::TESForm* a_form)
		{
			if (auto* file = a_form ? a_form->GetFile(0) : nullptr) {
				return std::string{ file->GetFilename() };
			}
			return "<unknown>";
		}

		// Light plugins (ESL / ESP with the light flag) use a 12 bit form id, normal
		// ones 24 bit. Matches the layout of the generated name file.
		std::uint32_t LocalID(RE::FormID a_runtimeFormID)
		{
			return (a_runtimeFormID >> 24) == 0xFE ? (a_runtimeFormID & 0xFFF) : (a_runtimeFormID & 0xFFFFFF);
		}

		// BlinkDash.ShaderNames.json sits next to BlinkDash.json, i.e. inside the mod
		// folder under MO2. Purely optional: without it every shader keeps its
		// plugin:formid label.
		void LoadNameFile()
		{
			s_names.clear();

			const auto path = Config::Path().parent_path() / kNameFileName;
			std::error_code ec;
			if (!std::filesystem::exists(path, ec)) {
				BDW("effects: no shader name file at {}", path.string());
				return;
			}

			try {
				std::ifstream file{ path, std::ios::binary };
				if (!file) {
					BDE("effects: could not open the shader name file");
					return;
				}

				auto json = nlohmann::json::parse(file);
				if (!json.is_object()) {
					BDE("effects: shader name file root is {}, expected an object", json.type_name());
					return;
				}

				for (const auto& [key, value] : json.items()) {
					if (value.is_string()) {
						s_names.emplace(key, value.get<std::string>());
					} else {
						BDW("effects: name entry '{}' is {}, expected a string, skipping", key, value.type_name());
					}
				}
				BDI("effects: {} shader names loaded from {}", s_names.size(), path.string());
			} catch (const std::exception& e) {
				BDE("effects: shader name file threw: {}", e.what());
			}
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
				// legacy: earlier versions stored the runtime form id, and users may
				// hand write one
				if (a_key.size() == 10 && EqualsIgnoreCase(a_key, std::format("0x{:08X}", entry.runtimeFormID))) {
					return &entry;
				}
			}
			return nullptr;
		}

		const Entry* FirstMatching(std::initializer_list<std::string_view> a_needles)
		{
			for (const auto needle : a_needles) {
				for (const auto& entry : s_entries) {
					const std::string candidate = ToLower(entry.name);
					if (!candidate.empty() && candidate.find(needle) != std::string::npos) {
						BD("effects: auto-pick candidate '{}' matched '{}'", needle, entry.Label());
						return &entry;
					}
				}
			}
			return nullptr;
		}

		void MarkDuplicates()
		{
			for (auto& entry : s_entries) {
				entry.disambiguate = false;
			}
			for (std::size_t i = 0; i < s_entries.size(); ++i) {
				for (std::size_t j = i + 1; j < s_entries.size(); ++j) {
					if (s_entries[i].Label() == s_entries[j].Label()) {
						s_entries[i].disambiguate = true;
						s_entries[j].disambiguate = true;
					}
				}
			}
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
		return std::format("{} [{}] (runtime form 0x{:08X})", s_selected->Label(), s_selected->Key(), s_selected->runtimeFormID);
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

		// Unknown key: probe the usual suspects so a first run is not silent. The
		// menu overrides it.
		s_selected = FirstMatching({ "invis", "blink", "teleport", "summon", "ghost" });
		if (s_selected) {
			BDI("effects: '{}' is not a known shader, auto-picked {} - change it in the Skyrim Menu Framework -> Blink Dash",
				want, DescribeSelection());
			return;
		}

		BDW("effects: shader '{}' was not found, using none", want);
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

		LoadNameFile();

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
		std::size_t named = 0;

		for (auto* shader : forms) {
			if (!shader) {
				continue;
			}
			Entry entry;
			entry.shader = shader;
			entry.runtimeFormID = shader->GetFormID();
			entry.plugin = SourceName(shader);
			entry.localID = LocalID(entry.runtimeFormID);

			if (const auto it = s_names.find(entry.Key()); it != s_names.end()) {
				entry.name = it->second;
				++named;
			}
			s_entries.push_back(std::move(entry));
		}

		std::ranges::sort(s_entries, [](const Entry& a_lhs, const Entry& a_rhs) {
			const auto lhs = ToLower(a_lhs.Label());
			const auto rhs = ToLower(a_rhs.Label());
			if (lhs != rhs) {
				return lhs < rhs;
			}
			return a_lhs.localID < a_rhs.localID;
		});

		MarkDuplicates();

		for (std::size_t i = 0; i < s_entries.size(); ++i) {
			const auto& entry = s_entries[i];
			BD("effects:   #{} '{}' 0x{:08X} ({}{})",
				i, entry.Label(), entry.runtimeFormID, entry.plugin,
				entry.name.empty() ? ", no name in the json" : "");
		}

		BDI("effects: {} shaders collected, {} named, {} json entries unused",
			s_entries.size(), named, s_names.size() >= named ? s_names.size() - named : 0);

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

		BD("effects: calling ApplyEffectShader(player=0x{:X}, key='{}', form=0x{:08X}, duration {:.2f}) [{}]",
			reinterpret_cast<std::uintptr_t>(player), s_selected->Key(), s_selected->runtimeFormID, kBlinkDuration, a_reason);
		auto* effect = fn(player, s_selected->shader, kBlinkDuration, nullptr, false, false, nullptr, false);
		BD("effects: ApplyEffectShader returned 0x{:X} [{}]", reinterpret_cast<std::uintptr_t>(effect), a_reason);
	}
}
