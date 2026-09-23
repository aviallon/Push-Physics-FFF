#pragma once

// Havok world-side calls left out of CommonLibSSE-NG, reconstructed from the
// Address Library the same way Precision (github.com/ersh1/Precision, GPL-3.0
// WITH Modding Exception) does.
//
// These are NOT hook targets: we do not add them to Hooks/HookTargets.def or the
// committed verification table, because that table requires a prologue hash
// generated from SkyrimSE.exe (tools/gen-hooktable.py) and no game binary is
// available in CI. Instead every call is resolved through REL::Relocation and
// null-guarded, with a one-shot error log if the relocation is unresolved - the
// same discipline used for Main::GetSingleton() in ProxyRegistry.cpp.
//
//   hkpWorld_addContactListener(hkpWorld*, hkpContactListener*)
//       RELOCATION_ID(60543, 61383)  // SE A7AB80, AE A9F390
//   hkpCollisionCallbackUtil_requireCollisionCallbackUtil(hkpWorld*)
//       RELOCATION_ID(60588, 61437)  // SE A7DD00, AE AA2510
//
// The util is REQUIRED: Havok does not dispatch contact listeners without it.

namespace RE
{
	class hkpContactListener;
	class hkpWorld;
}

namespace pa::havok
{
	// Ensure the collision-callback util exists on a_world. Returns false (and
	// logs once) when the relocation is unresolved or a_world is null.
	[[nodiscard]] bool EnsureContactCallbackUtil(RE::hkpWorld* a_world);

	// Register a_listener. Returns false (and logs once) when the relocation is
	// unresolved or an argument is null.
	[[nodiscard]] bool TryAddContactListener(RE::hkpWorld* a_world, RE::hkpContactListener* a_listener);

	// True when a_listener is already in a_world->contactListeners. Pure inline
	// scan (no relocation).
	[[nodiscard]] bool HasContactListener(RE::hkpWorld* a_world, const RE::hkpContactListener* a_listener);
}
