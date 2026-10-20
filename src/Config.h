#pragma once

#include "PCH.h"

namespace BlinkDash
{
	// Which actor value the blink spends. The check and the drain are identical for both;
	// only the pool being touched differs, so this stays a plain either/or.
	enum class CostResource
	{
		kStamina,
		kMagicka
	};

	struct Settings
	{
		std::uint32_t hotkey{ 0x103 };        // 0x100|N = mouse button index N (0 = LMB, 1 = RMB, 2 = MMB, 3 = MB4); otherwise a DIK scancode
		int           distance{ 200 };        // blink length in game units
		int           costAmount{ 10 };       // drained per blink from costResource, 0 = free
		CostResource  costResource{ CostResource::kStamina };  // stamina or magicka
		bool          costOutsideCombat{ false };  // charge it out of combat too; off = the cost exists in combat only
		int           retriggerMs{ 200 };     // tap window in milliseconds: press AND release inside this, otherwise nothing happens
		int           cooldownMs{ 1000 };     // milliseconds between blinks; 0 = off, every tap may blink
		int           maxSlope{ 45 };         // degrees: ground steeper than this counts as a wall, not as a slope to blink along
		std::string   effectShader{ "Skyrim.esm:000E5C" };  // effect shader to play on blink: "plugin:FORMID" from BlinkDash.ShaderNames.json, or "None"
		std::string   requiredPerk{ "None" };  // blink only works while the player has this perk: "plugin:FORMID", or "None" = always on
		bool          debugOverlay{ false };  // draw the last blink's rays and landing spot in game
		bool          logging{ true };        // write every step to BlinkDash.log
	};

	class Config
	{
	public:
		// The live settings object. The menu edits it in place from the render thread, so a
		// field-by-field read of this from the game thread is not safe for the string fields;
		// use Snapshot() for anything that reads across threads.
		static Settings& Get();

		// A copy taken under the settings lock. That is what the game thread reads: the menu
		// reassigns effectShader/requiredPerk while a blink can be running, and a plain read of
		// a std::string being reassigned is a use-after-free waiting to happen.
		static Settings Snapshot();

		// The two string settings are written through locked setters for the same reason.
		static void SetEffectShader(std::string a_value);
		static void SetRequiredPerk(std::string a_value);

		// Replaces every field with its default, under the lock.
		static void ResetToDefaults();

		// Human readable dump of every field, used by the logging.
		static std::string Describe();

		// Clamps out-of-range values and logs every correction.
		static bool Validate();

		static void Load();
		static void Save();

		static std::filesystem::path Path();
	};
}
