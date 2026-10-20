#pragma once

#include "PCH.h"

namespace BlinkDash::Forms
{
	// Shared plumbing for the two load-order dropdowns (effect shaders, perks). Both enumerate
	// every form of one type, label it the same way, disambiguate duplicate names the same way
	// and resolve the same stable "plugin:FORMID" key. The two copies had drifted in small ways
	// (only one of them understood the legacy runtime-id key); one copy cannot drift.

	inline std::string ToLower(std::string_view a_text)
	{
		std::string out{ a_text };
		for (auto& c : out) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return out;
	}

	inline bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs)
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

	inline std::string SourceName(RE::TESForm* a_form)
	{
		if (auto* file = a_form ? a_form->GetFile(0) : nullptr) {
			return std::string{ file->GetFilename() };
		}
		return "<unknown>";
	}

	// Light plugins (ESL / ESP with the light flag) use a 12 bit form id, normal ones 24 bit.
	inline std::uint32_t LocalID(RE::FormID a_runtimeFormID)
	{
		return (a_runtimeFormID >> 24) == 0xFE ? (a_runtimeFormID & 0xFFF) : (a_runtimeFormID & 0xFFFFFF);
	}

	// The fields every dropdown entry has. A derived Entry adds the form pointer it enumerates.
	struct EntryBase
	{
		std::string   plugin;        // file the form is defined in
		std::uint32_t localID{ 0 };  // form id inside that file
		RE::FormID    runtimeFormID{ 0 };
		std::string   name;          // display name, may be empty
		std::uint32_t rank{ 1 };         // position in a nextPerk chain, 1 based
		std::uint32_t rankCount{ 1 };    // length of that chain, 1 for single-rank forms

		// Value written to the config. Always stable across load order changes.
		[[nodiscard]] std::string Key() const
		{
			return std::format("{}:{:06X}", plugin, localID);
		}

		enum class Suffix
		{
			kNone,     // unique label
			kPlugin,   // same name exists in another plugin
			kFormID    // same name exists in the same plugin
		};
		Suffix suffix{ Suffix::kNone };

		// Label without the disambiguation suffix. Multi-rank perks are separate records sharing
		// one name, so they are numbered the way the game shows them ("Armsman 3"); a form with
		// a single rank (every effect shader) gets the bare name.
		[[nodiscard]] std::string BaseLabel() const
		{
			if (name.empty()) {
				return Key();
			}
			return rankCount > 1 ? std::format("{} {}", name, rank) : name;
		}

		// What the dropdown shows: the name when there is one, otherwise the key, with a suffix
		// only when two entries would read the same.
		[[nodiscard]] std::string Label() const
		{
			switch (suffix) {
			case Suffix::kPlugin:
				return std::format("{} ({})", BaseLabel(), plugin);
			case Suffix::kFormID:
				return std::format("{} (0x{:06X})", BaseLabel(), localID);
			default:
				return BaseLabel();
			}
		}
	};

	// windows.h defines max() as a macro, hence the parentheses
	inline constexpr std::size_t kNoSelection = (std::numeric_limits<std::size_t>::max)();

	template <class Entry>
	class List
	{
	public:
		[[nodiscard]] std::vector<Entry>&       Mutable() { return _entries; }
		[[nodiscard]] const std::vector<Entry>& Entries() const { return _entries; }

		// Named forms first, then case-insensitively by label, then by form id inside the plugin.
		void SortAndMark()
		{
			std::ranges::sort(_entries, [](const Entry& a_lhs, const Entry& a_rhs) {
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
		}

		// Resolves a config value: the stable key, or a legacy runtime form id ("0x00012345")
		// written by an earlier build or by hand.
		[[nodiscard]] const Entry* Find(std::string_view a_key) const
		{
			if (a_key.empty()) {
				return nullptr;
			}
			for (const auto& entry : _entries) {
				if (entry.Key() == a_key) {
					return &entry;
				}
				if (a_key.size() == 10 && EqualsIgnoreCase(a_key, std::format("0x{:08X}", entry.runtimeFormID))) {
					return &entry;
				}
			}
			return nullptr;
		}

		// An index rather than a pointer so concurrent menu edits and playback never race on a
		// raw pointer into a vector that can be rebuilt.
		[[nodiscard]] std::size_t IndexOf(const Entry* a_entry) const
		{
			if (!a_entry || _entries.empty()) {
				return kNoSelection;
			}
			return static_cast<std::size_t>(a_entry - _entries.data());
		}

	private:
		void MarkDuplicates()
		{
			for (auto& entry : _entries) {
				entry.suffix = EntryBase::Suffix::kNone;
			}

			// Group by the un-suffixed label: entries that would read the same get a plugin name,
			// or a form id when even the plugin is the same.
			for (std::size_t i = 0; i < _entries.size(); ++i) {
				for (std::size_t j = i + 1; j < _entries.size(); ++j) {
					if (ToLower(_entries[i].BaseLabel()) != ToLower(_entries[j].BaseLabel())) {
						continue;
					}
					const auto kind = EqualsIgnoreCase(_entries[i].plugin, _entries[j].plugin) ?
						EntryBase::Suffix::kFormID :
						EntryBase::Suffix::kPlugin;
					if (_entries[i].suffix == EntryBase::Suffix::kNone) {
						_entries[i].suffix = kind;
					}
					if (_entries[j].suffix == EntryBase::Suffix::kNone) {
						_entries[j].suffix = kind;
					}
				}
			}
		}

		std::vector<Entry> _entries;
	};
}
