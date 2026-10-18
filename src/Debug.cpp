#include "PCH.h"
#include "Debug.h"
#include "Log.h"

namespace BlinkDash::Debug
{
	namespace
	{
		// One blink at a time: the write happens on the game thread, the read on the
		// render thread, and both are rare, so a plain mutex is plenty.
		std::mutex  s_lock;
		Snapshot    s_latest;
		bool        s_have{ false };
	}

	void Record(Snapshot a_snapshot)
	{
		std::scoped_lock lock{ s_lock };
		s_latest = std::move(a_snapshot);
		s_have = true;
	}

	bool Latest(Snapshot& a_out)
	{
		std::scoped_lock lock{ s_lock };
		if (!s_have) {
			return false;
		}
		a_out = s_latest;
		return true;
	}

	void Clear()
	{
		std::scoped_lock lock{ s_lock };
		s_have = false;
	}
}
