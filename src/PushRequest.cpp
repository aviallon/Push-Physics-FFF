#include "PCH.h"

#include "PushRequest.h"

#include "Config.h"
#include "FrameClock.h"
#include "GameState.h"
#include "HkMath.h"
#include "PhysicsMath.h"
#include "SimGuard.h"

#include <RE/A/Actor.h>
#include <RE/B/bhkCharacterController.h>
#include <RE/H/hkpCharacterProxy.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/T/TESForm.h>

#include <atomic>
#include <cmath>

namespace pa
{
	namespace
	{
		// Two seqlock slots: the request slot holds the parsed `push` until the
		// main-thread apply consumes it, and the result slot carries the outcome
		// back to the command channel. Both ends now run on the main thread; the
		// slots are kept because `pending` is what makes consumption at-most-once
		// explicit (and because a future off-thread caller stays correct).
		// Each is a plain value plus an even/odd sequence counter; the producer
		// writes only while the sequence is odd and the consumer copies only while
		// it is even.
		struct RequestSlot
		{
			std::atomic<std::uint32_t> seq{ 0 };
			std::atomic<bool>          pending{ false };
			PushRequest                value{};
		};

		struct ResultSlot
		{
			std::atomic<std::uint32_t> seq{ 0 };
			std::atomic<bool>          pending{ false };
			PushResult                 value{};
		};

		RequestSlot g_request;
		ResultSlot  g_result;

		PushStatus g_lastPush;  // main thread only

		// ---------------------------------------------------------------- state push
		//
		// The engine's own character push. `bhkCharacterController` carries three
		// fields the on-ground/swimming character-state update consumes:
		//   outVelocity     +0x90  (the per-frame velocity/displacement accumulator)
		//   initialVelocity +0xA0  (the push velocity)
		//   velocityTime    +0x220 (the push duration, seconds)
		//
		// `bhkCharacterController::sub_78282` (AE id 78282, RVA 0x1065F50 on
		// 1.7.104) writes initialVelocity = dirScaled*(1/70)/m and velocityTime = m,
		// guarded by a "keep the stronger push" norm compare. The game's own
		// `pushactoraway` reaches it through
		// papyrus::ObjectReference::PushActorAway -> AIProcess::KnockExplosion
		// (AE 39895, RVA 0x7237C0), which passes dirScaled = unitVector*6*M and
		// m = 0.0125*M. Its sibling (RVA 0x1066020) then folds
		// initialVelocity*velocityTime into outVelocity once per state update.
		//
		// This is the mechanism a velocity write cannot reach: SetLinearVelocityImpl
		// writes the rigid body's motion velocity (+0x230), which the character
		// update recomputes every step, whereas these fields are ADDITIVE inputs the
		// update reads after it has recomputed the AI velocity. We write the two
		// fields directly rather than calling sub_78282 so the re-application below
		// is unconditional (the engine function keeps the stronger push and would
		// ignore a repeat). Both the formula and the constant are read off
		// SkyrimSE.exe 1.7.104
		// (sha256 846efccf0c1374d71f892907f46549560f2fcb0a75cb87a3eed438baa0f1402f).
		constexpr float kEngineStateInvScale = 0.0142875f;  // engine const 0x1417FDE5C

		// The engine's KnockExplosion mapping from a single magnitude M.
		constexpr float kStateDirPerMagnitude = 6.0f;      // 600 * (M/100)
		constexpr float kStateTimePerMagnitude = 0.0125f;  // 1.25 * (M/100)

		struct ActiveStatePush
		{
			bool                       active = false;
			RE::bhkCharacterController* ctrl = nullptr;
			float                      dir[3]{};  // unit horizontal, player -> target
			float                      dv = 0.0f;
			std::uint64_t              endMs = 0;
			std::uint32_t              formId = 0;
		};

		ActiveStatePush g_statePush;  // main thread only

		// Apply (or re-apply) the engine character-state push for the active
		// request. Writes `initialVelocity` (+0xA0) and `velocityTime` (+0x220).
		void WriteStatePush(RE::bhkCharacterController* a_ctrl, const float a_dir[3], float a_dv)
		{
			if (!a_ctrl) {
				return;
			}
			const float M = std::max(1.0f, std::fabs(a_dv));
			const float m = kStateTimePerMagnitude * M;
			const float scale = kStateDirPerMagnitude * M * (kEngineStateInvScale / m);
			a_ctrl->initialVelocity = Hk(a_dir[0] * scale, a_dir[1] * scale, a_dir[2] * scale);
			a_ctrl->velocityTime = m;
		}

		// Main thread. Publish a result through the seqlock slot. Kept in one
		// place so the refused path and the applied path cannot drift.
		void PublishResult(const PushResult& a_res)
		{
			const auto seq = g_result.seq.load(std::memory_order_relaxed);
			g_result.seq.store(seq + 1, std::memory_order_release);
			g_result.value = a_res;
			g_result.seq.store(seq + 2, std::memory_order_release);
			g_result.pending.store(true, std::memory_order_release);
		}

		[[nodiscard]] bool VectorChanged(const float a_from[3], const float a_to[3])
		{
			const float dx = a_to[0] - a_from[0];
			const float dy = a_to[1] - a_from[1];
			const float dz = a_to[2] - a_from[2];
			return (dx * dx + dy * dy + dz * dz) > (1.0e-3f * 1.0e-3f);
		}
	}

	void PublishPushRequest(const PushRequest& a_request)
	{
		const auto seq = g_request.seq.load(std::memory_order_relaxed);
		g_request.seq.store(seq + 1, std::memory_order_release);  // odd: writing
		g_request.value = a_request;
		g_request.seq.store(seq + 2, std::memory_order_release);  // even: stable
		g_request.pending.store(true, std::memory_order_release);
	}

	bool PushRequestPending()
	{
		return g_request.pending.load(std::memory_order_acquire);
	}

	void ApplyPendingPushRequest()
	{
		if (!g_request.pending.exchange(false, std::memory_order_acq_rel)) {
			return;  // nothing queued
		}

		PushRequest req{};
		for (;;) {
			const auto s1 = g_request.seq.load(std::memory_order_acquire);
			if (s1 & 1u) {
				continue;  // a publish is in flight
			}
			req = g_request.value;
			const auto s2 = g_request.seq.load(std::memory_order_acquire);
			if (s1 == s2) {
				break;
			}
		}

		PushResult res{};
		res.targetFormId = req.targetFormId;
		res.target = reinterpret_cast<std::uintptr_t>(req.target);
		res.ctrl = reinterpret_cast<std::uintptr_t>(req.ctrl);
		res.mode = req.mode;
		res.dv = req.dv;
		res.frame = FrameClock::CurrentFrame();

		// Validate before touching any Havok state: a stalled simulation, a null
		// controller or a non-finite payload must be dropped, not applied. The
		// `pending` exchange above already guarantees at-most-once consumption.
		const auto refusal = EvaluatePushApply(SimulationStalled(), req.ctrl != nullptr,
			req.dir[0], req.dir[1], req.dir[2], req.dv);
		if (refusal != PushApplyRefusal::kNone) {
			res.refusal = static_cast<int>(refusal);
			PublishResult(res);
			return;
		}

		const RE::hkVector4 delta = Hk(req.dir[0] * req.dv, req.dir[1] * req.dv, req.dir[2] * req.dv);

		if ((req.mode == PushMode::kCtrl || req.mode == PushMode::kBoth) && req.ctrl) {
			RE::hkVector4 before;
			req.ctrl->GetLinearVelocityImpl(before);
			req.ctrl->SetLinearVelocityImpl(before + delta);
			RE::hkVector4 after;
			req.ctrl->GetLinearVelocityImpl(after);
			const auto b = ToVec3(before);
			const auto a = ToVec3(after);
			res.ctrlFrom[0] = b.x;
			res.ctrlFrom[1] = b.y;
			res.ctrlFrom[2] = b.z;
			res.ctrlTo[0] = a.x;
			res.ctrlTo[1] = a.y;
			res.ctrlTo[2] = a.z;
			res.ctrlApplied = true;
			res.mechanism |= 1;
		}

		if ((req.mode == PushMode::kRb || req.mode == PushMode::kBoth) && req.ctrl) {
			auto* rb = req.ctrl->GetRigidBody();
			res.rb = reinterpret_cast<std::uintptr_t>(rb);
			if (rb) {
				const auto before = ToVec3(rb->motion.linearVelocity);
				rb->motion.linearVelocity = rb->motion.linearVelocity + delta;
				const auto after = ToVec3(rb->motion.linearVelocity);
				res.rbFrom[0] = before.x;
				res.rbFrom[1] = before.y;
				res.rbFrom[2] = before.z;
				res.rbTo[0] = after.x;
				res.rbTo[1] = after.y;
				res.rbTo[2] = after.z;
				res.rbApplied = true;
				res.mechanism |= 2;
			}
		}

		// Engine character-state push (mode `state`). The engine's own
		// pushactoraway route, applied here and then re-applied every frame for
		// maxPushDurationMs so the character update cannot erase it before the
		// step integrates it. At most one state push is active at a time; a new
		// `push ... state` replaces it (latest wins, like the request slot).
		if ((static_cast<int>(req.mode) & static_cast<int>(PushMode::kState)) && req.ctrl) {
			const auto outBefore = ToVec3(req.ctrl->outVelocity);
			const auto initBefore = ToVec3(req.ctrl->initialVelocity);
			res.stateVTimeFrom = req.ctrl->velocityTime;

			g_statePush.active = true;
			g_statePush.ctrl = req.ctrl;
			g_statePush.dir[0] = req.dir[0];
			g_statePush.dir[1] = req.dir[1];
			g_statePush.dir[2] = req.dir[2];
			g_statePush.dv = req.dv;
			g_statePush.formId = req.targetFormId;
			g_statePush.endMs = FrameClock::NowMs() +
				static_cast<std::uint64_t>(std::max(0.0f, Config::Get().maxPushDurationMs));
			WriteStatePush(req.ctrl, req.dir, req.dv);

			const auto initAfter = ToVec3(req.ctrl->initialVelocity);
			const auto outAfter = ToVec3(req.ctrl->outVelocity);
			res.stateFrom[0] = initBefore.x;
			res.stateFrom[1] = initBefore.y;
			res.stateFrom[2] = initBefore.z;
			res.stateTo[0] = initAfter.x;
			res.stateTo[1] = initAfter.y;
			res.stateTo[2] = initAfter.z;
			res.stateVTimeTo = req.ctrl->velocityTime;
			res.outFrom[0] = outBefore.x;
			res.outFrom[1] = outBefore.y;
			res.outFrom[2] = outBefore.z;
			res.outTo[0] = outAfter.x;
			res.outTo[1] = outAfter.y;
			res.outTo[2] = outAfter.z;
			res.stateApplied = true;
			res.mechanism |= 4;
		}

		res.changed =
			(res.ctrlApplied && VectorChanged(res.ctrlFrom, res.ctrlTo)) ||
			(res.rbApplied && VectorChanged(res.rbFrom, res.rbTo)) ||
			(res.stateApplied && VectorChanged(res.stateFrom, res.stateTo));

		PublishResult(res);
	}

	bool DrainPushResult(PushResult& a_out)
	{
		if (!g_result.pending.exchange(false, std::memory_order_acq_rel)) {
			return false;
		}
		for (;;) {
			const auto s1 = g_result.seq.load(std::memory_order_acquire);
			if (s1 & 1u) {
				continue;
			}
			a_out = g_result.value;
			const auto s2 = g_result.seq.load(std::memory_order_acquire);
			if (s1 == s2) {
				break;
			}
		}

		if (a_out.refusal != 0) {
			// A dropped request must not masquerade as the last applied push in the
			// `status` line or the trace's push_* columns.
			return true;
		}

		g_lastPush.active = true;
		g_lastPush.requestedMode = a_out.mode;
		g_lastPush.requestedDv = a_out.dv;
		g_lastPush.mechanism = a_out.mechanism;
		g_lastPush.changed = a_out.changed;
		for (int i = 0; i < 3; ++i) {
			g_lastPush.ctrlFrom[i] = a_out.ctrlFrom[i];
			g_lastPush.ctrlTo[i] = a_out.ctrlTo[i];
			g_lastPush.rbFrom[i] = a_out.rbFrom[i];
			g_lastPush.rbTo[i] = a_out.rbTo[i];
			g_lastPush.stateFrom[i] = a_out.stateFrom[i];
			g_lastPush.stateTo[i] = a_out.stateTo[i];
			g_lastPush.outFrom[i] = a_out.outFrom[i];
			g_lastPush.outTo[i] = a_out.outTo[i];
		}
		g_lastPush.targetFormId = a_out.targetFormId;
		g_lastPush.stateApplied = a_out.stateApplied;
		g_lastPush.stateActive = g_statePush.active;
		g_lastPush.stateVTimeFrom = a_out.stateVTimeFrom;
		g_lastPush.stateVTimeTo = a_out.stateVTimeTo;
		return true;
	}

	bool StatePushActive()
	{
		return g_statePush.active;
	}

	void ReapplyActiveStatePush()
	{
		if (!g_statePush.active) {
			return;
		}
		// A save load / cell change rebuilds the character controller, so the
		// pointer captured at publish time can be freed. Re-resolve it from the
		// actor each frame and stop rather than write through a dangling pointer.
		auto* ctrl = g_statePush.ctrl;
		if (g_statePush.formId != 0) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_statePush.formId);
			ctrl = actor ? actor->GetCharController() : nullptr;
		}
		// The engine writes land on the AI-updated character state, which only
		// exists while a game is loaded. Refuse (and drop) otherwise.
		if (!IsGameActive() || !ctrl) {
			g_statePush.active = false;
			g_statePush.ctrl = nullptr;
			return;
		}
		g_statePush.ctrl = ctrl;
		if (FrameClock::NowMs() >= g_statePush.endMs) {
			// Expired: zero the fields so the character stops being pushed.
			ctrl->initialVelocity = Hk(0.0f, 0.0f, 0.0f);
			ctrl->velocityTime = 0.0f;
			g_statePush.active = false;
			g_statePush.ctrl = nullptr;
			return;
		}
		// Still inside the window: re-apply. Calling the engine function again is
		// safe and idempotent (it keeps the stronger push); it is what prevents a
		// single write from being erased before the physics step integrates it.
		WriteStatePush(ctrl, g_statePush.dir, g_statePush.dv);
	}

	const PushStatus& LastPushStatus()
	{
		return g_lastPush;
	}
}
