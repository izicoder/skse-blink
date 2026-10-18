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
		float         cooldown{ 0.0f };        // seconds between blinks; 0 = off, every tap may blink
		float         maxSlope{ 45.0f };       // degrees: ground steeper than this counts as a wall, not as a slope to blink along
		std::string   effectShader{ "Skyrim.esm:000E5C" };  // effect shader to play on blink: "plugin:FORMID" from BlinkDash.ShaderNames.json, or "None"
		bool          debugOverlay{ false };  // draw the last blink's rays and landing spot in the menu
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
