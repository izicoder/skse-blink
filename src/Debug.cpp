#include "PCH.h"
#include "Debug.h"
#include "Log.h"

namespace BlinkDash::Debug
{
	namespace
	{
		// One blink at a time: the write happens on the game thread, the read on the
		// render thread, and both are rare, so a plain mutex is plenty. The snapshot itself
		// is immutable once published, so the reader keeps its reference outside the lock.
		std::mutex                        s_lock;
		std::shared_ptr<const Snapshot>   s_latest;
	}

	void Record(Snapshot a_snapshot)
	{
		auto published = std::make_shared<const Snapshot>(std::move(a_snapshot));
		std::scoped_lock lock{ s_lock };
		s_latest = std::move(published);
	}

	std::shared_ptr<const Snapshot> Latest()
	{
		std::scoped_lock lock{ s_lock };
		return s_latest;
	}

	void Clear()
	{
		std::scoped_lock lock{ s_lock };
		s_latest.reset();
	}
}
