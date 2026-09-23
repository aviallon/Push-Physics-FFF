#pragma once

#include <RE/B/bhkCharProxyController.h>
#include <RE/B/bhkCharacterController.h>
#include <RE/H/hkpCharacterProxy.h>

namespace pa
{
	// The listener object is the first subobject of bhkCharProxyController, so a
	// bhkCharacterController* returned by GetCharController() points at the
	// *controller* subobject (+0x10) whose vptr is VTABLE_bhkCharProxyController[1].
	// Anything else (bhkCharRigidBodyController, mounted, detached, ragdolled)
	// returns nullptr. The controller is verified here and on the proxy side the
	// same verification is required, so an attach is based on a proven identity,
	// not on offset arithmetic.
	[[nodiscard]] RE::bhkCharProxyController* AsProxyController(RE::bhkCharacterController* a_ctrl);

	// The player's verified controller, or nullptr when the player has none right
	// now (mounted / ragdolled / mid-load). This is the authority every caller
	// must use: the hkpCharacterProxy is a *separately allocated* Havok object
	// that the controller only holds a pointer to, so the retired
	// proxy -> controller inversion (`proxy - 0x350`) is not a real relation and
	// must never be used to decide whether an attach is allowed.
	[[nodiscard]] RE::bhkCharProxyController* PlayerController();

	// The player's proxy, or nullptr when the player has none right now.
	[[nodiscard]] RE::hkpCharacterProxy* PlayerProxy();

	// Diagnostic only, one call per process: run the retired proxy -> controller
	// inversion once and, if it would have refused, log the observed qword at
	// proxy-0x350 against the expected listener vtable so the next in-game run
	// still shows the mismatch. It never gates an attach.
	void LogRetiredInversionOnce(RE::hkpCharacterProxy* a_proxy);
}
