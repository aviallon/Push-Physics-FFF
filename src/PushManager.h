#pragma once

namespace RE
{
	class hkpCharacterProxy;
}

namespace pa
{
	class PushListener;

	// Attach the process-wide PushListener to the player's character proxy.
	//
	// Resolves the chain:
	//   PlayerCharacter::GetSingleton()
	//     -> GetActorRuntimeData().currentProcess
	//     -> AIProcess::GetCharController()
	//     -> vtable identity check (bhkCharProxyController, not a bare cast)
	//     -> bhkCharProxyController::GetCharacterProxy()
	//     -> hkpCharacterProxy::listeners.push_back(listener)
	//
	// Every step is null-checked and the whole call is guarded; a failure is
	// logged and returns false, never throws. Safe to call more than once: the
	// second call is a no-op while the listener is already on the current proxy,
	// and a re-attach after the controller was rebuilt is handled by the
	// main-thread tick.
	[[nodiscard]] bool InstallPushListener();

	// Drop ownership without touching the retired proxy's listeners array; the
	// orphan check then ignores any late calls (design.md 3.5).
	void DetachPushListener();

	// Re-attach when the player's controller was rebuilt, and keep the player's
	// proxy tuned. Runs on the main thread.
	void PushManagerMainThreadTick();

	// The listener owned by this plugin for the process lifetime.
	[[nodiscard]] PushListener* GetPushListener();

	// Escalations (design.md 3.8). v1 declares no hook targets, so this refuses
	// rather than patching anything.
	bool InstallEscalationHooks();
	void RemoveEscalationHooks();
}
