#pragma once

#include "PCH.h"

namespace BlinkDash::Perks
{
	struct Entry
	{
		RE::BGSPerk*  perk{ nullptr };
		std::string   plugin;       // file the form is defined in
		std::uint32_t localID{ 0 };  // form id inside that file
		RE::FormID    runtimeFormID{ 0 };
		std::string   name;         // the perk's editor/full name, may be empty

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

		// Label without any disambiguation suffix.
		[[nodiscard]] std::string BaseLabel() const
		{
			return name.empty() ? Key() : name;
		}

		// What the dropdown shows: the perk name when there is one, otherwise the key
		// (i.e. a perk with no name keeps its plugin:form id label).
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

	// Enumerates every BGSPerk in the load order (vanilla and modded alike) and
	// applies the configured selection. Game thread, at kDataLoaded.
	void Init();

	// Re-resolves the selection after the config changed.
	void RefreshSelection();

	[[nodiscard]] const std::vector<Entry>& Entries();

	// The resolved requirement, for logging and the menu readout.
	[[nodiscard]] std::string DescribeSelection();

	// True when no perk is required. A configured perk that cannot be resolved
	// (uninstalled plugin, hand-typed key) counts as "not required", so a stale
	// config never locks the blink out silently.
	[[nodiscard]] bool RequirementSatisfied();

	// The full gate: no requirement configured, or the player has the perk.
	// Called from the blink gate checks on the game thread.
	[[nodiscard]] bool PlayerHasRequired(RE::PlayerCharacter* a_player, const char* a_source);
}
