#include "PCH.h"

#include "PushRequest.h"

#include "FrameClock.h"
#include "HkMath.h"
#include "PhysicsMath.h"
#include "SimGuard.h"

#include <RE/B/bhkCharacterController.h>
#include <RE/H/hkpCharacterProxy.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkpRigidBody.h>

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

		res.changed =
			(res.ctrlApplied && VectorChanged(res.ctrlFrom, res.ctrlTo)) ||
			(res.rbApplied && VectorChanged(res.rbFrom, res.rbTo));

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
		}
		g_lastPush.targetFormId = a_out.targetFormId;
		return true;
	}

	const PushStatus& LastPushStatus()
	{
		return g_lastPush;
	}
}
