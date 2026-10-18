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

		// What the dropdown shows: the name from the json when there is one,
		// otherwise the key (i.e. an unnamed mod shader keeps its form id).
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
