#pragma once

#include "PCH.h"

namespace BlinkDash
{
	// Thread safe entry point. Call from any thread (the input sink runs on a
	// BSJobs thread): it only sets a flag and queues a game thread task.
	void RequestBlink(const char* a_source);

	// True while a blink is pending or executing.
	bool IsBlinkQueued();

	// Executes one blink, start to finish. Game thread only.
	bool DoBlink(const char* a_source);
}
