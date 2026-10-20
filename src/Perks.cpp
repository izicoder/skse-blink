#include "PCH.h"
#include "Perks.h"
#include "Config.h"
#include "Log.h"

namespace BlinkDash::Perks
{
	namespace
	{
		constexpr const char* kNoneKey = "None";

		Forms::List<Entry> s_list;
		std::vector<Entry>& s_entries = s_list.Mutable();
		std::atomic<std::size_t> s_selectedIndex{ Forms::kNoSelection };

		const Entry* Selected()
		{
			// The config string is the truth and the index is derived state. If the two ever
			// disagree (a settings reset that replaced every field, a hand-edited file), resolve
			// again instead of gating the blink on a perk that is no longer configured.
			const auto settings = Config::Snapshot();
			const auto& want = settings.requiredPerk;
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
			entry.plugin = Forms::SourceName(perk);
			entry.localID = Forms::LocalID(entry.runtimeFormID);
			entry.name = DisplayName(perk);
			s_entries.push_back(std::move(entry));
		}

		// Numbering first: the shared sort and the duplicate marking both read BaseLabel(),
		// which is where the rank is shown.
		AssignRanks();

		// Named perks first, then the ones without a name (they keep their plugin:form id
		// label); the shared helper marks duplicate labels.
		s_list.SortAndMark();

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
		const auto settings = Config::Snapshot();
		const auto& want = settings.requiredPerk;

		if (want.empty() || want == kNoneKey) {
			s_selectedIndex.store(Forms::kNoSelection, std::memory_order_release);
			BDI("perks: requirement is 'none'");
			return;
		}
		if (const auto* found = s_list.Find(want)) {
			s_selectedIndex.store(s_list.IndexOf(found), std::memory_order_release);
			BDI("perks: requirement = {}", DescribeSelection());
			return;
		}

		// Unknown key (plugin uninstalled, hand-typed): keep the string as written but
		// treat it as "no requirement", and say so. The menu lets the user fix it.
		s_selectedIndex.store(Forms::kNoSelection, std::memory_order_release);
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
