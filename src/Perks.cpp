#include "PCH.h"
#include "Perks.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash::Perks
{
	namespace
	{
		constexpr const char* kNoneKey = "None";

		std::vector<Entry>       s_entries;
		const Entry*             s_selected{ nullptr };
		std::atomic<std::size_t> s_selectedIndex{ (std::numeric_limits<std::size_t>::max)() };

		const Entry* Selected()
		{
			// The config string is the truth and the index is derived state. If the two ever
			// disagree (a settings reset that replaced every field, a hand-edited file), resolve
			// again instead of gating the blink on a perk that is no longer configured.
			const auto& want = Config::Get().requiredPerk;
			const bool none = want.empty() || want == kNoneKey;

			const auto index = s_selectedIndex.load(std::memory_order_acquire);
			if (none || index >= s_entries.size()) {
				return nullptr;
			}
			if (s_entries[index].Key() != want) {
				BDW("perks: selection was stale ('{}' selected, '{}' configured), re-resolving",
					s_entries[index].Key(), want);
				RefreshSelection();
				const auto refreshed = s_selectedIndex.load(std::memory_order_acquire);
				return refreshed >= s_entries.size() ? nullptr : &s_entries[refreshed];
			}
			return &s_entries[index];
		}

		std::size_t IndexOf(const Entry* a_entry)
		{
			if (!a_entry) {
				return (std::numeric_limits<std::size_t>::max)();
			}
			return static_cast<std::size_t>(a_entry - s_entries.data());
		}

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

		std::string SourceName(RE::TESForm* a_form)
		{
			if (auto* file = a_form ? a_form->GetFile(0) : nullptr) {
				return std::string{ file->GetFilename() };
			}
			return "<unknown>";
		}

		// Light plugins (ESL / ESP with the light flag) use a 12 bit form id, normal
		// ones 24 bit. Same layout as the effect shader keys.
		std::uint32_t LocalID(RE::FormID a_runtimeFormID)
		{
			return (a_runtimeFormID >> 24) == 0xFE ? (a_runtimeFormID & 0xFFF) : (a_runtimeFormID & 0xFFFFFF);
		}

		// The perk's in-game name. TESFullName lives on BGSPerk itself; fall back to
		// the editor id when the record has no name, so the dropdown never shows blanks.
		std::string DisplayName(RE::BGSPerk* a_perk)
		{
			if (!a_perk) {
				return {};
			}
			const RE::TESFullName* fullName = a_perk;
			// GetFullName() hands back a raw const char*, so the empty check has to be on the
			// pointer (a missing TESFullName would otherwise be dereferenced).
			if (const char* name = fullName ? fullName->GetFullName() : nullptr; name && *name) {
				return std::string{ name };
			}
			return std::string{ a_perk->GetFormEditorID() };
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

		Entry* FindByPerk(RE::BGSPerk* a_perk)
		{
			for (auto& entry : s_entries) {
				if (entry.perk == a_perk) {
					return &entry;
				}
			}
			return nullptr;
		}

		// Multi-rank perks are separate BGSPerk records chained through nextPerk, all
		// sharing a single name. Without numbering, every rank collides with its siblings
		// and the dropdown has to fall back to form ids for them.
		void AssignRanks()
		{
			const auto chainLimit = s_entries.size() + 1;  // guards against a looping chain

			for (auto& entry : s_entries) {
				entry.rank = 1;
				entry.rankCount = 1;
			}

			for (auto& head : s_entries) {
				// A chain head is a perk that no other perk's nextPerk points at.
				const bool isHead = std::ranges::none_of(s_entries, [&](const Entry& a_other) {
					return a_other.perk && a_other.perk->nextPerk == head.perk;
				});
				if (!isHead) {
					continue;
				}

				std::uint32_t count = 0;
				for (auto* perk = head.perk; perk && count < chainLimit; perk = perk->nextPerk) {
					++count;
				}
				if (count < 2) {
					continue;  // single-rank perk: rank 1 is not shown
				}

				std::uint32_t rank = 1;
				for (auto* perk = head.perk; perk && rank <= count; perk = perk->nextPerk) {
					if (auto* entry = FindByPerk(perk)) {
						entry->rank = rank;
						entry->rankCount = count;
					}
					++rank;
				}
			}
		}

		bool EqualsIgnoreCasePlugin(std::string_view a_lhs, std::string_view a_rhs)
		{
			return ToLower(a_lhs) == ToLower(a_rhs);
		}

		void MarkDuplicates()
		{
			for (auto& entry : s_entries) {
				entry.suffix = Entry::Suffix::kNone;
			}

			// Group by the un-suffixed label: entries that would read the same get a
			// plugin name, or a form id when even the plugin is the same. Same scheme as
			// the effect shader dropdown, so the two lists behave identically.
			for (std::size_t i = 0; i < s_entries.size(); ++i) {
				for (std::size_t j = i + 1; j < s_entries.size(); ++j) {
					if (ToLower(s_entries[i].BaseLabel()) != ToLower(s_entries[j].BaseLabel())) {
						continue;
					}
					const auto kind = EqualsIgnoreCasePlugin(s_entries[i].plugin, s_entries[j].plugin) ?
						Entry::Suffix::kFormID :
						Entry::Suffix::kPlugin;
					if (s_entries[i].suffix == Entry::Suffix::kNone) {
						s_entries[i].suffix = kind;
					}
					if (s_entries[j].suffix == Entry::Suffix::kNone) {
						s_entries[j].suffix = kind;
					}
				}
			}
		}

	}

	void Init()
	{
		BDI("perks: Init() begin");

		auto* handler = RE::TESDataHandler::GetSingleton();
		BD("perks: TESDataHandler = 0x{:X}", reinterpret_cast<std::uintptr_t>(handler));
		if (!handler) {
			BDE("perks: no TESDataHandler, cannot enumerate perks");
			return;
		}

		const auto& forms = handler->GetFormArray<RE::BGSPerk>();
		BDI("perks: {} BGSPerk forms in the load order", forms.size());

		s_entries.clear();
		s_entries.reserve(forms.size());

		for (auto* perk : forms) {
			if (!perk) {
				continue;
			}
			Entry entry;
			entry.perk = perk;
			entry.runtimeFormID = perk->GetFormID();
			entry.plugin = SourceName(perk);
			entry.localID = LocalID(entry.runtimeFormID);
			entry.name = DisplayName(perk);
			s_entries.push_back(std::move(entry));
		}

		AssignRanks();

		std::ranges::sort(s_entries, [](const Entry& a_lhs, const Entry& a_rhs) {
			// Named perks first, then the ones without a name (they keep their
			// plugin:form id label).
			if (a_lhs.name.empty() != a_rhs.name.empty()) {
				return !a_lhs.name.empty();
			}
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
			BD("perks:   #{} '{}' 0x{:08X} ({})", i, entry.Label(), entry.runtimeFormID, entry.plugin);
		}

		BDI("perks: {} perks collected", s_entries.size());

		RefreshSelection();
		BDI("perks: Init() end, requirement = {}", DescribeSelection());
	}

	void RefreshSelection()
	{
		const auto& want = Config::Get().requiredPerk;

		if (want.empty() || want == kNoneKey) {
			s_selectedIndex.store((std::numeric_limits<std::size_t>::max)(), std::memory_order_release);
			BDI("perks: requirement is 'none'");
			return;
		}
		if (const auto* found = FindByKey(want)) {
			s_selectedIndex.store(IndexOf(found), std::memory_order_release);
			BDI("perks: requirement = {}", DescribeSelection());
			return;
		}

		// Unknown key (plugin uninstalled, hand-typed): keep the string as written but
		// treat it as "no requirement", and say so. The menu lets the user fix it.
		s_selectedIndex.store((std::numeric_limits<std::size_t>::max)(), std::memory_order_release);
		BDW("perks: required perk '{}' was not found among {} known perks, requirement is ignored until it is re-picked",
			want, s_entries.size());
	}

	const std::vector<Entry>& Entries()
	{
		return s_entries;
	}

	std::string DescribeSelection()
	{
		const auto* selected = Selected();
		if (!selected) {
			return "none";
		}
		return std::format("{} [{}] (runtime form 0x{:08X})", selected->Label(), selected->Key(), selected->runtimeFormID);
	}

	bool RequirementSatisfied()
	{
		return Selected() == nullptr;
	}

	bool PlayerHasRequired(RE::PlayerCharacter* a_player, const char* a_source)
	{
		const auto* selected = Selected();
		if (!selected || !selected->perk) {
			BD("  perk gate: no perk required, allowing ({})", a_source);
			return true;
		}
		if (!a_player) {
			BD("  perk gate: no player, cannot check the perk, rejecting ({})", a_source);
			return false;
		}

		const bool has = a_player->HasPerk(selected->perk);
		BD("  perk gate: requires '{}' ({}) -> player {} ({})",
			selected->Label(), selected->Key(), has ? "has it" : "does NOT have it", a_source);
		return has;
	}
}
