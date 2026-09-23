#pragma once

namespace pa
{
	// Install the MinHook function-entry detour on the function Main::Update
	// calls as its last instruction before the epilogue (AE Address Library id
	// 107306, SkyrimSE.exe 1.7.104 RVA 0x154AF70). That leaf runs exactly once
	// per frame and drives the main-thread tick.
	//
	// Main::Update's OWN entry is deliberately not hooked: HDT-SMP already
	// detours it, so its committed prologue no longer matches, and
	// CommunityShaders / SKSE patch call sites inside it. Hooking the leaf's
	// uncontested entry therefore coexists with all of them; see
	// src/Hooks/HookTargets.def for the disassembly evidence and the full
	// collision rationale.
	//
	// The target is resolved through the Address Library and verified against
	// the committed hook table embedded in this DLL (identity + AE id + prologue
	// hash) BEFORE anything is patched. On any mismatch the detour is refused and
	// the plugin is marked DEGRADED; an unverifiable address is never hooked.
	//
	// Must be called once, on the main thread, after SKSE::Init. Returns true
	// when the detour is installed and enabled.
	bool InstallFrameTickHook();

	// Remove the detour and uninitialize MinHook. The plugin never unloads, so
	// this exists for completeness and a clean shutdown path only.
	void RemoveFrameTickHook();
}
