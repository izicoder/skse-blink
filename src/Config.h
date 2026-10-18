#pragma once

#include "PCH.h"

namespace BlinkDash
{
	struct Settings
	{
		std::uint32_t hotkey{ 0x103 };        // 0x100|N = mouse button index N (0 = LMB, 1 = RMB, 2 = MMB, 3 = MB4); otherwise a DIK scancode
		float         distance{ 300.0f };     // blink length in game units
		float         staminaCost{ 10.0f };   // stamina drained per blink, 0 = free
		float         retriggerDelay{ 0.20f };  // seconds: press AND release must be shorter than this, otherwise nothing happens
		std::string   effectShader{};         // vanilla TESEffectShader to play on blink, by editor id (or 0xFORMID); empty = none
	};

	class Config
	{
	public:
		static Settings& Get();

		// Human readable dump of every field, used by the logging.
		static std::string Describe();

		// Clamps out-of-range values and logs every correction.
		static bool Validate();

		static void Load();
		static void Save();

		static std::filesystem::path Path();
	};
}
