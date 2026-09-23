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
	// returns nullptr.
	[[nodiscard]] RE::bhkCharProxyController* AsProxyController(RE::bhkCharacterController* a_ctrl);

	// Invert proxy -> controller: proxy lives at controller + 0x350. The candidate
	// must (1) carry the listener vtable and (2) point back at exactly this proxy,
	// which makes a wild offset unrepresentable (design.md 3.3).
	[[nodiscard]] RE::bhkCharProxyController* ControllerOf(RE::hkpCharacterProxy* a_proxy);

	// The player's proxy, or nullptr when the player has none right now.
	[[nodiscard]] RE::hkpCharacterProxy* PlayerProxy();
}
