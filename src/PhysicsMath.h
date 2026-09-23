#pragma once

// Pure push-model maths: no CommonLibSSE, no Havok, no Windows. Everything here
// is plain C++ over a tiny Vec3, so it is compiled and asserted on Linux by
// tests/ (and on Windows by the same tests). PushModel.cpp is the thin adapter
// that converts hkVector4 <-> Vec3 and calls into this module.
//
// The split is deliberate: a guard that has never been observed refusing
// anything is not a guard, so the gate decisions and the buffer maths are tested
// against synthetic inputs rather than only read.

#include <cmath>
#include <cstdint>

namespace pa::math
{
	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	[[nodiscard]] constexpr Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	[[nodiscard]] constexpr Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	[[nodiscard]] constexpr Vec3 Scale(const Vec3& v, float s) { return { v.x * s, v.y * s, v.z * s }; }
	[[nodiscard]] constexpr Vec3 Negate(const Vec3& v) { return { -v.x, -v.y, -v.z }; }
	[[nodiscard]] constexpr float Dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	[[nodiscard]] constexpr float SqrLength3(const Vec3& v) { return Dot3(v, v); }
	[[nodiscard]] inline float Length3(const Vec3& v) { return SqrLength3(v) > 0.0f ? std::sqrt(SqrLength3(v)) : 0.0f; }

	// Skyrim is Z-up: "horizontal" drops Z.
	[[nodiscard]] constexpr Vec3 Horizontal(const Vec3& v) { return { v.x, v.y, 0.0f }; }
	[[nodiscard]] Vec3 HorizontalNormalize(const Vec3& v);
	[[nodiscard]] Vec3 ClampLength(const Vec3& v, float maxLen);

	// Model step 1: d = horizontalNormalize(p_o - p_s), falling back to the
	// contact normal when the two capsule centres coincide. Returns false when
	// both candidates are degenerate (nothing to push along).
	[[nodiscard]] bool ComputePushDirection(const Vec3& p_s, const Vec3& p_o, const Vec3& n_c, Vec3& d_out);

	// Model step 2: dot(d, v_s - v_o) - minRelSpeed. <= 0 means "refuse".
	[[nodiscard]] float ClosingSpeed(const Vec3& d, const Vec3& v_s, const Vec3& v_o, float minRelSpeed);

	// Model step 3: clamp(2*m_s/(m_s+m_o), 0, cap).
	[[nodiscard]] float MassRatio(float m_s, float m_o, float cap);

	struct HeavyGateResult
	{
		bool  refuse = false;
		float scale = 1.0f;
	};

	// Model step 4: heavy targets need a real closing speed and are then scaled.
	[[nodiscard]] HeavyGateResult HeavyGate(float m_o, float m_s, float v_close, float heavyMassRatio, float heavyMinSpeed, float heavyScale);

	// Model step 4: dv = min(dv, maxDeltaV).
	[[nodiscard]] float CapDeltaV(float dv, float maxDeltaV);

	// Model step 5: combat/out-of-combat, swimming, airborne, unknown-target.
	[[nodiscard]] float ApplyStateScales(float dv, bool inCombat, bool swimming, bool airborne, bool known,
		float combatScale, float outOfCombatScale, float swimScale, float airborneScale, float unknownTargetScale);

	// Model step 8: staggerDeltaV * (m_o/m_s)^exponent.
	[[nodiscard]] float StaggerThreshold(float staggerDeltaV, float m_o, float m_s, float staggerMassExponent);

	// Model step 8: is a stagger warranted for this dv / state / cooldown?
	[[nodiscard]] bool StaggerWanted(float dv, float threshold, bool inCombat, bool staggerOutOfCombat,
		std::uint64_t nowMs, std::uint64_t lastStaggerMs, float staggerCooldownMs);

	// --- per-target buffer (model steps 6/7, gates 6/7/8/9) ------------------

	struct BufferParams
	{
		float         cooldownMs = 200.0f;
		float         maxPushDurationMs = 600.0f;
		float         maxInjectedSpeed = 350.0f;
		float         staggerCooldownMs = 1500.0f;
	};

	struct BufferState
	{
		Vec3          velocity{};
		float         magnitude = 0.0f;
		std::uint64_t lastPushMs = 0;
		std::uint64_t lastStaggerMs = 0;
		std::uint64_t expiresMs = 0;
		std::uint32_t appliedFrame = 0;
	};

	struct BufferUpdate
	{
		Vec3 velocity{};      // the buffer value AFTER the update (already capped)
		bool renewed = false; // cooldown had elapsed -> a genuinely new push
		bool applyNow = false;// the once-per-frame guard let this contact apply
		bool stagger = false; // a stagger should be queued
	};

	// Steps 6/7/8. `dv` has already been through heavy gate, caps and state scales.
	// take-max, never accumulate; applied at most once per (frame). `staggerAllowed`
	// is (inCombat || staggerOutOfCombat), `staggerThreshold` comes from
	// StaggerThreshold().
	[[nodiscard]] BufferUpdate UpdateBuffer(BufferState& a_state, const Vec3& d, float dv,
		std::uint64_t nowMs, std::uint32_t frame, const BufferParams& params,
		bool staggerAllowed, float staggerThreshold);

	// Gate 9: exponential damping of a buffered velocity.
	[[nodiscard]] Vec3 DampVelocity(const Vec3& v, float damping, float deltaSec);

	// Gate 7: entry lifetime.
	[[nodiscard]] bool IsExpired(const BufferState& state, std::uint64_t nowMs);

	// --- gates (design.md sec 5) --------------------------------------------

	enum class GateRefusal : std::uint8_t
	{
		kNone = 0,
		kMasterOff,              // 1
		kNoCharacterInteraction, // 1 (Path-1 switch)
		kDialogue,               // 15
		kRagdoll,                // 10
		kDead,                   // 11
		kNotPushable,            // 12
		kNoCharacterCollisions,  // 12
		kKillMove,               // 13
		kBleedout,               // 13
		kDisableInCombat,        // 19
		kDisableWhileSwimming,   // 19
		kBudgetExceeded,         // 18
		kMinRelSpeed,            // 5
		kDirectionDegenerate,    // step 1
		kHeavyGate,              // 4
	};

	[[nodiscard]] const char* GateRefusalName(GateRefusal a_refusal);

	struct GateInputs
	{
		bool enabled = true;
		bool useCharacterInteraction = true;
		bool dialogueOpen = false;
		bool disableDuringDialogue = true;
		bool ragdoll = false;
		bool dead = false;
		bool notPushable = false;
		bool noCharacterCollisions = false;
		bool killMove = false;
		bool bleedout = false;
		bool inCombat = false;
		bool swimming = false;
		bool disableInCombat = false;
		bool disableWhileSwimming = false;
		bool budgetExceeded = false;
	};

	// Boolean-state gates 1/10/11/12/13/15/18/19, evaluated before any physics.
	[[nodiscard]] GateRefusal EvaluateGates(const GateInputs& a_in);

	// --- character-strength derivation (Approach A) ---------------------------
	//
	// Havok's own CharacterInteractionDemo sets cpci.m_characterStrength = 5000
	// ("how much the character is able to push other objects around"); Havok's
	// default is HK_REAL_MAX and Skyrim never sets it (measured FLT_MAX in game),
	// so there is no game-authoritative value. 5000 is the base and the value is
	// derived from the player's race mass, level and physical skills.
	//
	// All inputs are plain floats so the derivation is unit-tested off-game.
	struct StrengthInputs
	{
		float raceBaseMass = 80.0f;       // TESRace::data.baseMass (inline field)
		float referenceMass = 80.0f;      // fStrengthReferenceMass
		float level = 1.0f;               // Actor level, >= 1
		float oneHanded = 0.0f;           // raw actor values, 0..100 (may exceed)
		float twoHanded = 0.0f;
		float block = 0.0f;
		float heavyArmor = 0.0f;
		float archery = 0.0f;
		float weightOneHanded = 0.30f;
		float weightTwoHanded = 0.25f;
		float weightBlock = 0.20f;
		float weightHeavyArmor = 0.15f;
		float weightArchery = 0.10f;
		float levelGain = 0.05f;           // fStrengthLevelGain
		float skillGain = 1.371f;          // fStrengthSkillGain
		float referenceSkill = 0.15f;      // P is normalised to 1 here
		float base = 5000.0f;             // fStrengthBase
		float minStrength = 500.0f;       // fStrengthMin
		float maxStrength = 200000.0f;    // fStrengthMax

		// Diagnostics only; DeriveCharacterStrength ignores it.
		std::uint32_t raceFormID = 0;
	};

	// Weighted mean of the physical skills, each as actorValue/100 clamped to
	// [0,1], normalised by the weight sum. Returns 0 when the weights are all 0.
	[[nodiscard]] float PhysicalSkill01(const StrengthInputs& a_in);

	// Power index P, normalised so P(L1, skill01 = referenceSkill) = 1:
	//   P = (1 + levelGain * max(0, level - 1)) * (1 + skillGain * skill01)
	//       / (1 + skillGain * referenceSkill)
	// The fitted anchors (fStrengthBase = 5000, fStrengthLevelGain = 0.05,
	// fStrengthSkillGain = 1.371, referenceSkill = 0.15) are
	// L1/0.15 -> 1, L50/0.80 -> 6, L252/1.00 -> ~26.65.
	[[nodiscard]] float StrengthPower(const StrengthInputs& a_in);

	// characterStrength = base * P * (raceBaseMass / referenceMass), clamped to
	// [minStrength, maxStrength].
	[[nodiscard]] float DeriveCharacterStrength(const StrengthInputs& a_in);

	// fPlayerMass * P^fMassPowerExponent: the mass the player *contests* with.
	// 100 at base, ~350 at L50/skill0.80, ~1000 at max.
	[[nodiscard]] float EffectivePlayerMass(float a_baseMass, float a_power, float a_exponent);

	// fCharacterStrength semantics: >= 0 is an explicit override used verbatim;
	// < 0 derives. It can never return the -1 sentinel.
	[[nodiscard]] float ResolveCharacterStrength(float a_override, const StrengthInputs& a_in);
}
