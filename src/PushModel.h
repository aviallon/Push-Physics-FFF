#pragma once

#include <RE/H/hkArray.h>

#include <thread>

namespace RE
{
	class hkpCharacterProxy;
	class hkpCharacterObjectInteractionEvent;
	class hkpCharacterObjectInteractionResult;
	class hkContactPoint;
	struct hkpRootCdPoint;
}

namespace pa
{
	// The push model of design.md sec 4: Path 1 (character vs character) and
	// Path 2 (character vs rigid body), plus the main-thread half (stagger drain,
	// buffer sweep, calibration log). All the maths lives in PhysicsMath and is
	// unit-tested off-game; this class is the thin Havok/game adapter.
	class PushModel
	{
	public:
		// Path 1. Called from CharacterInteractionCallback and from ScanManifold.
		static void OnCharacterContact(RE::hkpCharacterProxy* a_self,
			RE::hkpCharacterProxy* a_other,
			const RE::hkContactPoint* a_contact);

		// Path 2. Called from ObjectInteractionCallback.
		static void OnObjectContact(RE::hkpCharacterProxy* a_self,
			const RE::hkpCharacterObjectInteractionEvent* a_input,
			RE::hkpCharacterObjectInteractionResult* a_output);

		// Primary Path-1 detection (design.md 3.7), driven by the slot-1 override:
		// a scan of the manifold inside ProcessConstraintsCallback. Cheap and
		// allocation-free; distinct targets feed the same per-target path.
		static void ScanManifold(RE::hkpCharacterProxy* a_self,
			const RE::hkArray<RE::hkpRootCdPoint>& a_manifold);

		// Distinct target proxies the manifold scan has detected this session.
		// CharacterInteractionCallback never fires on the player's proxy (in-game
		// evidence, 2026-09), so this is the counter that proves Path 1 detection.
		[[nodiscard]] static std::uint64_t PairCount();

		// Approach A, applied to the player's proxy on load / config change.
		static void ApplyProxyTuning(RE::hkpCharacterProxy* a_playerProxy);

		static void TickMainThread(float a_deltaSec);

		static void LogCalibrationOnce();

		// Thread identity instrumentation (U2): the main thread records itself at
		// load, the first callback records the callback thread.
		static void SetMainThreadId(std::thread::id a_id);
		static void NoteCallbackThread();
		static std::uint32_t MainThreadId();
		static std::uint32_t CallbackThreadId();
	};
}
