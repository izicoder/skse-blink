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
		float        fraction{ 1.0f };
		const char*  label{ "ray" };
	};

	struct Snapshot
	{
		static constexpr std::size_t kMaxSegments = 24;

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
		float        requested{ 0.0f };
		float        travel{ 0.0f };
		int          pulls{ 0 };
		std::string  headline;  // one line, for the in-game overlay
		std::string  summary;   // two lines, for the menu
	};

	void Record(Snapshot a_snapshot);

	// False when no blink has been recorded yet.
	[[nodiscard]] bool Latest(Snapshot& a_out);

	void Clear();
}
