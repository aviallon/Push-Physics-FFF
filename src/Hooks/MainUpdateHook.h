#pragma once

#include <cstdint>

namespace pa
{
	// Install the MinHook function-entry detour on RE::Main::Update that drives
	// the main-thread tick.
	//
	// The target is resolved through the Address Library and verified against the
	// committed hook table embedded in this DLL (identity + AE id + prologue
	// hash) BEFORE anything is patched. On any mismatch the detour is refused and
	// the plugin is marked DEGRADED; an unverifiable address is never hooked.
	//
	// Must be called once, on the main thread, after SKSE::Init. Returns true
	// when the detour is installed and enabled.
	bool InstallMainUpdateHook();

	// Remove the detour and uninitialize MinHook. The plugin never unloads, so
	// this exists for completeness and a clean shutdown path only.
	void RemoveMainUpdateHook();
}
