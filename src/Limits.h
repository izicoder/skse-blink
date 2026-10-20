#pragma once

namespace BlinkDash::Limits
{
	// Every bound that Config::Validate, the menu sliders and the blink itself have to agree on.
	// They used to live in three files (Config.cpp, UI.cpp, Blink.cpp): a slider that could ask
	// for a value the blink then silently clamped, or a config that accepted what the menu could
	// not show, was one edit away. One definition means they cannot disagree.
	constexpr int kMinDistance = 100;
	constexpr int kMaxDistance = 1000;   // also the hard cap applied at blink time
	constexpr int kMaxCost = 100;
	constexpr int kMinTapWindowMs = 20;  // below this a human cannot tap it anyway
	constexpr int kMaxTapWindowMs = 1000;
	constexpr int kMinCooldownMs = 0;
	constexpr int kMaxCooldownMs = 2000;
	constexpr int kMinSlopeDegrees = 20;  // below this even stairs would be treated as walls
	constexpr int kMaxSlopeDegrees = 80;
}
