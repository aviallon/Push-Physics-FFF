#include "PhysicsMath.h"

#include <algorithm>
#include <cmath>

namespace pa::math
{
	namespace
	{
		constexpr float kDegenerateSqr = 1e-8f;
		constexpr float kHorizontalEps = 1e-4f;
	}

	Vec3 HorizontalNormalize(const Vec3& a_v)
	{
		const auto  h = Horizontal(a_v);
		const float len = Length3(h);
		if (len <= kHorizontalEps) {
			return {};
		}
		return Scale(h, 1.0f / len);
	}

	Vec3 ClampLength(const Vec3& a_v, float a_maxLen)
	{
		const float len = Length3(a_v);
		if (a_maxLen > 0.0f && len > a_maxLen) {
			return Scale(a_v, a_maxLen / len);
		}
		return a_v;
	}

	bool ComputePushDirection(const Vec3& a_p_s, const Vec3& a_p_o, const Vec3& a_n_c, Vec3& a_dOut)
	{
		Vec3 d = HorizontalNormalize(Sub(a_p_o, a_p_s));
		if (SqrLength3(d) < kDegenerateSqr) {
			// Capsule centres coincide: fall back to the contact normal, oriented
			// from pusher to other so a normal that points the wrong way cannot
			// push the target into the player.
			d = HorizontalNormalize(a_n_c);
			if (Dot3(d, Sub(Horizontal(a_p_o), Horizontal(a_p_s))) < 0.0f) {
				d = Negate(d);
			}
		}
		if (SqrLength3(d) < kDegenerateSqr) {
			return false;
		}
		a_dOut = d;
		return true;
	}

	float ClosingSpeed(const Vec3& a_d, const Vec3& a_v_s, const Vec3& a_v_o, float a_minRelSpeed)
	{
		const float closing = std::max(0.0f, Dot3(a_d, Sub(a_v_s, a_v_o)));
		return closing - a_minRelSpeed;
	}

	float MassRatio(float a_m_s, float a_m_o, float a_cap)
	{
		const float total = a_m_s + a_m_o;
		if (total <= 0.0f) {
			return 0.0f;
		}
		const float mu = 2.0f * a_m_s / total;
		return std::clamp(mu, 0.0f, a_cap);
	}

	HeavyGateResult HeavyGate(float a_m_o, float a_m_s, float a_vClose, float a_heavyMassRatio, float a_heavyMinSpeed, float a_heavyScale)
	{
		HeavyGateResult result;
		if (a_m_o > a_heavyMassRatio * a_m_s) {
			if (a_vClose < a_heavyMinSpeed) {
				result.refuse = true;
			} else {
				result.scale = a_heavyScale;
			}
		}
		return result;
	}

	float CapDeltaV(float a_dv, float a_maxDeltaV)
	{
		return std::min(a_dv, a_maxDeltaV);
	}

	float ApplyStateScales(float a_dv, bool a_inCombat, bool a_swimming, bool a_airborne, bool a_known,
		float a_combatScale, float a_outOfCombatScale, float a_swimScale, float a_airborneScale, float a_unknownTargetScale)
	{
		float dv = a_dv;
		dv *= a_inCombat ? a_combatScale : a_outOfCombatScale;
		dv *= a_swimming ? a_swimScale : 1.0f;
		dv *= a_airborne ? a_airborneScale : 1.0f;
		if (!a_known) {
			dv *= a_unknownTargetScale;
		}
		return dv;
	}

	float StaggerThreshold(float a_staggerDeltaV, float a_m_o, float a_m_s, float a_staggerMassExponent)
	{
		if (a_m_s <= 0.0f) {
			return a_staggerDeltaV;
		}
		return a_staggerDeltaV * std::pow(a_m_o / a_m_s, a_staggerMassExponent);
	}

	bool StaggerWanted(float a_dv, float a_threshold, bool a_inCombat, bool a_staggerOutOfCombat,
		std::uint64_t a_nowMs, std::uint64_t a_lastStaggerMs, float a_staggerCooldownMs)
	{
		if (a_dv < a_threshold) {
			return false;
		}
		if (!a_inCombat && !a_staggerOutOfCombat) {
			return false;
		}
		if (a_nowMs - a_lastStaggerMs < static_cast<std::uint64_t>(a_staggerCooldownMs)) {
			return false;
		}
		return true;
	}

	BufferUpdate UpdateBuffer(BufferState& a_state, const Vec3& a_d, float a_dv,
		std::uint64_t a_nowMs, std::uint32_t a_frame, const BufferParams& a_params,
		bool a_staggerAllowed, float a_staggerThreshold)
	{
		BufferUpdate out;

		const bool cooldownElapsed = a_nowMs - a_state.lastPushMs >= static_cast<std::uint64_t>(a_params.cooldownMs);
		if (cooldownElapsed) {
			a_state.velocity = Scale(a_d, a_dv);
			a_state.lastPushMs = a_nowMs;
			out.renewed = true;
		} else {
			// take-max, never accumulate: two contacts in the same window must not
			// add up into a rocket.
			a_state.velocity = Scale(a_d, std::max(Length3(a_state.velocity), a_dv));
		}

		a_state.magnitude = a_dv;
		a_state.velocity = ClampLength(a_state.velocity, a_params.maxInjectedSpeed);
		a_state.expiresMs = a_nowMs + static_cast<std::uint64_t>(a_params.maxPushDurationMs);

		// Step 7: one manifold can hold several contact points against the same
		// proxy; without this guard the push is multiplied by the contact count.
		if (a_state.appliedFrame != a_frame) {
			a_state.appliedFrame = a_frame;
			out.applyNow = true;
		}

		out.velocity = a_state.velocity;
		if (a_staggerAllowed && a_dv >= a_staggerThreshold &&
			a_nowMs - a_state.lastStaggerMs >= static_cast<std::uint64_t>(a_params.staggerCooldownMs)) {
			a_state.lastStaggerMs = a_nowMs;
			out.stagger = true;
		}
		return out;
	}

	Vec3 DampVelocity(const Vec3& a_v, float a_damping, float a_deltaSec)
	{
		if (a_damping <= 0.0f || a_deltaSec <= 0.0f) {
			return a_v;
		}
		return Scale(a_v, std::exp(-a_damping * a_deltaSec));
	}

	bool IsExpired(const BufferState& a_state, std::uint64_t a_nowMs)
	{
		return a_nowMs >= a_state.expiresMs;
	}

	const char* GateRefusalName(GateRefusal a_refusal)
	{
		switch (a_refusal) {
		case GateRefusal::kNone: return "none";
		case GateRefusal::kMasterOff: return "disabled (bEnabled=0)";
		case GateRefusal::kNoCharacterInteraction: return "bUseCharacterInteraction=0";
		case GateRefusal::kDialogue: return "DialogueMenu is open";
		case GateRefusal::kRagdoll: return "target is ragdolled";
		case GateRefusal::kDead: return "target is dead";
		case GateRefusal::kNotPushable: return "target flags say kNotPushable";
		case GateRefusal::kNoCharacterCollisions: return "target flags say kNoCharacterCollisions";
		case GateRefusal::kKillMove: return "target is in a kill move";
		case GateRefusal::kBleedout: return "target is bleeding out";
		case GateRefusal::kDisableInCombat: return "bDisableInCombat=1 and target in combat";
		case GateRefusal::kDisableWhileSwimming: return "bDisableWhileSwimming=1 and target swimming";
		case GateRefusal::kBudgetExceeded: return "uMaxInteractionsPerFrame budget reached";
		case GateRefusal::kMinRelSpeed: return "closing speed below fMinRelSpeed";
		case GateRefusal::kDirectionDegenerate: return "no usable push direction";
		case GateRefusal::kHeavyGate: return "heavy target below fHeavyMinSpeed";
		}
		return "unknown";
	}

	GateRefusal EvaluateGates(const GateInputs& a_in)
	{
		if (!a_in.enabled) {
			return GateRefusal::kMasterOff;
		}
		if (!a_in.useCharacterInteraction) {
			return GateRefusal::kNoCharacterInteraction;
		}
		if (a_in.disableDuringDialogue && a_in.dialogueOpen) {
			return GateRefusal::kDialogue;
		}
		if (a_in.ragdoll) {
			return GateRefusal::kRagdoll;
		}
		if (a_in.dead) {
			return GateRefusal::kDead;
		}
		if (a_in.notPushable) {
			return GateRefusal::kNotPushable;
		}
		if (a_in.noCharacterCollisions) {
			return GateRefusal::kNoCharacterCollisions;
		}
		if (a_in.killMove) {
			return GateRefusal::kKillMove;
		}
		if (a_in.bleedout) {
			return GateRefusal::kBleedout;
		}
		if (a_in.disableInCombat && a_in.inCombat) {
			return GateRefusal::kDisableInCombat;
		}
		if (a_in.disableWhileSwimming && a_in.swimming) {
			return GateRefusal::kDisableWhileSwimming;
		}
		if (a_in.budgetExceeded) {
			return GateRefusal::kBudgetExceeded;
		}
		return GateRefusal::kNone;
	}
}
