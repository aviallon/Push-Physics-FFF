#pragma once

// The `push` command's application path.
//
// Resolution (actor -> controller -> proxy, and the player -> target direction)
// happens on the MAIN thread when the command is parsed. The request is
// published into a seqlock slot and consumed by ApplyPendingPushRequest() on the
// SAME main thread, in ProxyRegistry::MainThreadTick, under the Havok world
// write lock (world->worldLock).
//
// It deliberately does NOT run in PushListener::ProcessConstraintsCallback:
// that callback runs inside the physics step, in the player character's own
// solver callback, and a push writes a velocity into a DIFFERENT character's
// controller / rigid body. Doing that re-entrantly from the solver hung two
// game sessions (the main thread ended up in a sched_yield spin, no crash log).
// The engine itself sets character velocities from the main thread, so applying
// a push there - with the same world lock Precision takes for structural world
// changes - is the engine's own usage pattern.
//
// The result of each application is published back through a second seqlock
// slot; `DrainPushResult` is the main-thread consumer (the seqlock is kept
// because it is what makes "consume exactly once" explicit).

#include "CommandParse.h"

#include <cstdint>

namespace RE
{
	class bhkCharacterController;
	class hkpCharacterProxy;
}

namespace pa
{
	struct PushRequest
	{
		RE::hkpCharacterProxy*      target = nullptr;
		RE::bhkCharacterController* ctrl = nullptr;
		float                       dir[3]{};     // unit horizontal, player -> target
		float                       origin[3]{};  // the pusher (player) position
		float                       dv = 0.0f;
		PushMode                    mode = PushMode::kBoth;
		std::uint32_t               targetFormId = 0;
	};

	struct PushResult
	{
		std::uint32_t  targetFormId = 0;
		std::uintptr_t target = 0;
		std::uintptr_t ctrl = 0;
		std::uintptr_t rb = 0;
		PushMode       mode = PushMode::kBoth;
		float          dv = 0.0f;
		int            mechanism = 0;  // bitmask: 1 ctrl, 2 rb, 4 state
		// pa::PushApplyRefusal; 0 (kNone) means the request was evaluated. Set
		// when a well-formed request was dropped without writing Havok state.
		int            refusal = 0;
		bool           ctrlApplied = false;
		bool           rbApplied = false;
		bool           stateApplied = false;
		bool           knockApplied = false;
		float          knockOrigin[3]{};
		float          knockMag = 0.0f;
		bool           stepListenApplied = false;
		std::uintptr_t stepListenBody = 0;
		std::uintptr_t stepListenPrev = 0;
		bool           changed = false;
		float          ctrlFrom[3]{};
		float          ctrlTo[3]{};
		float          rbFrom[3]{};
		float          rbTo[3]{};
		// The engine character-state push fields: bhkCharacterController's
		// initialVelocity (+0xA0) and velocityTime (+0x220), plus outVelocity
		// (+0x90), the per-frame displacement the state update integrates.
		float          stateFrom[3]{};
		float          stateVTimeFrom = 0.0f;
		float          stateTo[3]{};
		float          stateVTimeTo = 0.0f;
		float          outFrom[3]{};
		float          outTo[3]{};
		std::uint64_t  frame = 0;
	};

	// Persistent main-thread summary of the last applied push, used by the
	// trace's push_* columns. Owned by the main thread; no synchronisation.
	struct PushStatus
	{
		bool          active = false;
		PushMode      requestedMode = PushMode::kBoth;
		float         requestedDv = 0.0f;
		int           mechanism = 0;
		bool          changed = false;
		float         ctrlFrom[3]{};
		float         ctrlTo[3]{};
		float         rbFrom[3]{};
		float         rbTo[3]{};
		std::uint32_t targetFormId = 0;
		bool          stateApplied = false;
		bool          stateActive = false;  // a state push is still being re-applied
		bool          knockApplied = false;
		float         knockOrigin[3]{};
		float         knockMag = 0.0f;
		bool          stepListenApplied = false;
		std::uintptr_t stepListenBody = 0;
		std::uintptr_t stepListenPrev = 0;
		float         stateFrom[3]{};
		float         stateVTimeFrom = 0.0f;
		float         stateTo[3]{};
		float         stateVTimeTo = 0.0f;
		float         outFrom[3]{};
		float         outTo[3]{};
	};

	// Main thread. Latest wins; a request is applied at most once.
	void PublishPushRequest(const PushRequest& a_request);

	// Any thread. True while a published request has not yet been consumed (or
	// dropped) by ApplyPendingPushRequest(). Read by the stall warning.
	[[nodiscard]] bool PushRequestPending();

	// Main thread (ProxyRegistry::MainThreadTick), holding world->worldLock.
	// Consumes at most one request and writes the resulting velocities; publishes
	// a PushResult.
	void ApplyPendingPushRequest();

	// Any thread. True while a state push is still inside its re-application
	// window (maxPushDurationMs). Read by the main-thread tick to decide whether
	// it must take the world lock even with no request pending.
	[[nodiscard]] bool StatePushActive();

	// Any thread. True while the character-step listener push is inside its
	// window. Unlike the state push it is self-driving: once armed (under the
	// world lock) the listener applies the velocity from its own callback every
	// step, so the main thread does not re-apply it.
	[[nodiscard]] bool StepListenActive();

	// Main thread (ProxyRegistry::MainThreadTick), holding world->worldLock.
	// Re-applies the active state push so it is not erased by the character
	// update, and zeroes the fields once the window expires.
	void ReapplyActiveStatePush();

	// Main thread. Consume a pending result (if any), fold it into
	// LastPushStatus() and hand it back for reporting.
	[[nodiscard]] bool DrainPushResult(PushResult& a_out);

	// Main thread.
	[[nodiscard]] const PushStatus& LastPushStatus();
}
