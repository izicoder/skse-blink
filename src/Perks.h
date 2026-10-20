#pragma once

#include "PCH.h"
#include "FormList.h"

namespace BlinkDash::Perks
{
	// Key()/Label()/suffix/plugin/localID/name/rank all come from Forms::EntryBase;
	// Label() numbers multi-rank perks through BaseLabel().
	struct Entry : Forms::EntryBase
	{
		RE::BGSPerk* perk{ nullptr };
	};

	// Enumerates every BGSPerk in the load order (vanilla and modded alike) and
	// applies the configured selection. Game thread, at kDataLoaded.
	void Init();

	// Re-resolves the selection after the config changed.
	void RefreshSelection();

	[[nodiscard]] const std::vector<Entry>& Entries();

	// The resolved requirement, for logging and the menu readout.
	[[nodiscard]] std::string DescribeSelection();

	// The full gate: no requirement configured, or the player has the perk.
	// Called from the blink gate checks on the game thread.
	[[nodiscard]] bool PlayerHasRequired(RE::PlayerCharacter* a_player, const char* a_source);
}
