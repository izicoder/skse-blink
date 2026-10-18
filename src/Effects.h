#pragma once

#include "PCH.h"

namespace BlinkDash::Effects
{
	struct Entry
	{
		RE::TESEffectShader* shader{ nullptr };
		std::string          plugin;       // file the form is defined in
		std::uint32_t        localID{ 0 };  // form id inside that file
		RE::FormID           runtimeFormID{ 0 };
		std::string          name;         // from BlinkDash.ShaderNames.json, may be empty

		bool disambiguate{ false };  // set when two entries would show the same label

		// Stable identifier: plugin file name + form id inside that plugin, so it
		// survives load order changes. This is what goes in the config.
		[[nodiscard]] std::string Key() const
		{
			return std::format("{}:{:06X}", plugin, localID);
		}

		// What the dropdown shows: the name from the json when there is one,
		// otherwise the key (i.e. an unnamed mod shader keeps its form id).
		[[nodiscard]] std::string Label() const
		{
			std::string label = name.empty() ? Key() : name;
			if (disambiguate) {
				label += std::format(" ({})", plugin);
			}
			return label;
		}
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
