#pragma once

#include "PCH.h"

namespace BlinkDash::Effects
{
	struct Entry
	{
		RE::TESEffectShader* shader{ nullptr };
		std::string          editorID;
		std::string          source;
		RE::FormID           formID{ 0 };

		// Stable identifier used in the config and the menu: the editor id when the
		// form has one, otherwise the form id in hex.
		[[nodiscard]] std::string Key() const
		{
			return editorID.empty() ? std::format("0x{:08X}", formID) : editorID;
		}
	};

	// Resolves the engine call, enumerates every effect shader in the load order and
	// applies the configured selection. Game thread, once, at kDataLoaded.
	void Init();

	// Re-resolves the selection after the config changed.
	void RefreshSelection();

	[[nodiscard]] const std::vector<Entry>& Entries();
	[[nodiscard]] bool                      Available();
	[[nodiscard]] std::string               DescribeSelection();

	// Plays the selected shader on the player. Does nothing when none are selected
	// or when the engine call did not resolve.
	void PlayOnPlayer(const char* a_reason);

	// Menu hook: plays the selection for a couple of seconds.
	void Preview();
}
