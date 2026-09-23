#pragma once

namespace pa::CommandChannel
{
	// Main thread, driven from ProxyRegistry::MainThreadTick. Polls
	// <log dir>/PushAside.cmd at ~10 Hz, executes appended commands once and
	// appends responses to PushAside.out. Inert when the command file is absent.
	void Tick();

	// Main thread. True once PushAside.cmd has been observed to exist. Used by
	// the stall watchdog to keep a shipped install (no command file) silent.
	[[nodiscard]] bool Armed();
}
