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
		float                       dir[3]{};  // unit horizontal, player -> target
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
		int            mechanism = 0;  // bitmask: 1 ctrl, 2 rb
		// pa::PushApplyRefusal; 0 (kNone) means the request was evaluated. Set
		// when a well-formed request was dropped without writing Havok state.
		int            refusal = 0;
		bool           ctrlApplied = false;
		bool           rbApplied = false;
		bool           changed = false;
		float          ctrlFrom[3]{};
		float          ctrlTo[3]{};
		float          rbFrom[3]{};
		float          rbTo[3]{};
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

	// Main thread. Consume a pending result (if any), fold it into
	// LastPushStatus() and hand it back for reporting.
	[[nodiscard]] bool DrainPushResult(PushResult& a_out);

	// Main thread.
	[[nodiscard]] const PushStatus& LastPushStatus();
}
