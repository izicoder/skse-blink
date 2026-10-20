#pragma once

#include "PCH.h"

// A single blink's geometry, kept so the menu can draw what the last blink actually
// probed. Written once per blink, read once per frame while the debug view is open.
namespace BlinkDash::Debug
{
	struct Segment
	{
		RE::NiPoint3 from{};
		RE::NiPoint3 to{};      // hit point when hit, requested end when clear
		bool         hit{ false };
		bool         walked{ false };  // hit a surface gentle enough to blink along, so it did not shorten the travel
		float        slopeDegrees{ 0.0f };  // how far the hit surface is from flat, only meaningful when hit
		float        fraction{ 1.0f };
		const char*  label{ "ray" };
	};

	struct Snapshot
	{
		static constexpr std::size_t kMaxSegments = 40;

		Segment      segments[kMaxSegments]{};
		std::size_t  count{ 0 };

		RE::NiPoint3 feet{};
		RE::NiPoint3 direction{};
		RE::NiPoint3 landing{};
		RE::NiPoint3 directionSourcePoint{};  // where the direction came from, for the marks

		bool         blocked{ false };
		bool         grounded{ false };
		bool         groundHit{ false };
		bool         shortened{ false };
		bool         slopeFollowed{ false };  // the landing followed a slope instead of a level floor
		float        requested{ 0.0f };
		float        travel{ 0.0f };
		float        climb{ 0.0f };           // landing height minus feet height
		float        groundSlopeDegrees{ 0.0f };
		int          pulls{ 0 };
		int          walked{ 0 };             // how many probes hit a surface the blink walked along
		std::string  headline;  // one line, for the in-game overlay
		std::string  summary;   // three lines, for the menu
	};

	void Record(Snapshot a_snapshot);

	// The most recent blink, or nullptr when none has been recorded. Handed out by shared
	// pointer so the render thread never copies 40 segments and two strings while the game
	// thread is overwriting them.
	[[nodiscard]] std::shared_ptr<const Snapshot> Latest();

	void Clear();
}
