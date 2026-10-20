#pragma once

#include "PCH.h"
#include "FormList.h"

namespace BlinkDash::Effects
{
	// Key()/Label()/suffix/plugin/localID/name all come from Forms::EntryBase.
	struct Entry : Forms::EntryBase
	{
		RE::TESEffectShader* shader{ nullptr };
	};

	// Resolves the engine call, loads the name file, enumerates every effect shader
	// in the load order and applies the configured selection. Game thread, at
	// kDataLoaded.
	void Init();

	// Re-resolves the selection after the config changed.
	void RefreshSelection();

	[[nodiscard]] const std::vector<Entry>& Entries();
	[[nodiscard]] bool                      Available();
	[[nodiscard]] std::string               DescribeSelection();

	// Plays the selected shader on the player. Does nothing when none are selected
	// or when the engine call did not resolve.
	void PlayOnPlayer(const char* a_reason);
}
