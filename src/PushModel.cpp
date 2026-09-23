#include "PCH.h"

#include "PushModel.h"

#include "Config.h"
#include "FrameClock.h"
#include "HkMath.h"
#include "ProxyAccess.h"
#include "ProxyRegistry.h"
#include "PushRegistry.h"
#include "StaggerQueue.h"
#include "Health.h"

#include <RE/A/Actor.h>
#include <RE/T/TESRace.h>
#include <RE/H/hkContactPoint.h>
#include <RE/H/hkpCharacterProxy.h>
#include <RE/H/hkpCharacterProxyListener.h>
#include <RE/H/hkpRootCdPoint.h>
#include <RE/H/hkpShapePhantom.h>

#include <cmath>
#include <iterator>

namespace pa
{
	namespace
	{
		// Gate 18: per-frame interaction budget. The frame key is the same
		// millisecond key the buffer uses.
		std::atomic<std::uint32_t> g_budgetFrame{ 0xFFFFFFFFu };
		std::atomic<std::uint32_t> g_budgetCount{ 0 };

		std::atomic<std::uint32_t> g_mainThreadId{ 0 };
		std::atomic<std::uint32_t> g_callbackThreadId{ 0 };
		std::atomic<bool>          g_calibrationLogged{ false };
		std::atomic<bool>          g_objectNormalLogged{ false };

		// Effective player mass contested in the push model: fPlayerMass *
		// P^fMassPowerExponent. Written on the main thread, read by the physics
		// callback, so it is an atomic float. 0 means "not computed yet".
		std::atomic<float> g_effectivePlayerMass{ 0.0f };
		std::uint64_t      g_lastPowerRefreshMs = 0;  // main thread only

		std::atomic<std::uint64_t> g_debugWindowMs{ 0 };
		std::atomic<std::uint32_t> g_debugCount{ 0 };

		std::atomic<std::uint64_t> g_gateCounts[static_cast<std::size_t>(math::GateRefusal::kHeavyGate) + 2]{};

		void CountGate(math::GateRefusal a_refusal)
		{
			const auto index = static_cast<std::size_t>(a_refusal);
			if (index < std::size(g_gateCounts)) {
				g_gateCounts[index].fetch_add(1, std::memory_order_relaxed);
			}
		}

		[[nodiscard]] bool BudgetExceeded()
		{
			const auto frame = FrameClock::CurrentFrame();
			if (g_budgetFrame.load(std::memory_order_relaxed) != frame) {
				g_budgetFrame.store(frame, std::memory_order_relaxed);
				g_budgetCount.store(0, std::memory_order_relaxed);
			}
			return g_budgetCount.fetch_add(1, std::memory_order_relaxed) >= Config::Get().maxInteractionsPerFrame;
		}

		[[nodiscard]] bool DebugLineAllowed()
		{
			const auto now = FrameClock::NowMs();
			if (now - g_debugWindowMs.load(std::memory_order_relaxed) >= 1000) {
				g_debugWindowMs.store(now, std::memory_order_relaxed);
				g_debugCount.store(0, std::memory_order_relaxed);
			}
			return g_debugCount.fetch_add(1, std::memory_order_relaxed) < Config::Get().debugLogMaxPerSec;
		}

		[[nodiscard]] math::Vec3 PhantomPosition(const RE::hkpCharacterProxy* a_proxy)
		{
			if (a_proxy && a_proxy->shapePhantom) {
				return ToVec3(a_proxy->shapePhantom->motionState.transform.translation);
			}
			return {};
		}

		[[nodiscard]] float EffectivePlayerMass()
		{
			const float mass = g_effectivePlayerMass.load(std::memory_order_relaxed);
			return mass > 0.0f ? mass : Config::Get().playerMass;
		}

		// "Unknown target" diagnostics: design gate 16 pushes an unregistered
		// proxy conservatively (unknown mass, fUnknownTargetScale) and never
		// staggers. The note must be at most once per target and bounded, so it
		// cannot itself become the next physics-callback log flood. Deliberately
		// debug-only.
		constexpr std::size_t              kUnknownTargetSlots = 64;
		std::atomic<RE::hkpCharacterProxy*> g_unknownTargets[kUnknownTargetSlots]{};

		void NoteUnknownTargetOnce(RE::hkpCharacterProxy* a_proxy)
		{
			if (!a_proxy || !Config::Get().debugLog) {
				return;
			}
			for (const auto& slot : g_unknownTargets) {
				if (slot.load(std::memory_order_relaxed) == a_proxy) {
					return;  // already noted for this target
				}
			}
			for (auto& slot : g_unknownTargets) {
				RE::hkpCharacterProxy* expected = nullptr;
				if (slot.compare_exchange_strong(expected, a_proxy, std::memory_order_relaxed)) {
					logger::info("character interaction: unknown target 0x{:X} is not in the proxy registry; "
								 "conservative scaling (gate 16), no stagger",
						reinterpret_cast<std::uintptr_t>(a_proxy));
					return;
				}
			}
			// Table full: stop noting (bounded), never per frame.
		}

		// Fill the pure derivation inputs from the live player. Race mass is an
		// inline TESRace::data field and the five skills go through the
		// ActorValueOwner virtual, so neither path can be an unresolved
		// relocation. Actor::GetLevel() is relocation-backed: resolve it here and
		// refuse to call an unresolved (AE-id-0) id, the failure mode of the
		// 95f2485 crash, falling back to level 1 (neutral level term).
		[[nodiscard]] bool FillStrengthInputsFromPlayer(const Config& a_cfg, math::StrengthInputs& a_in)
		{
			a_in.base = a_cfg.strengthBase;
			a_in.referenceMass = a_cfg.strengthReferenceMass;
			a_in.levelGain = a_cfg.strengthLevelGain;
			a_in.skillGain = a_cfg.strengthSkillGain;
			a_in.minStrength = a_cfg.strengthMin;
			a_in.maxStrength = a_cfg.strengthMax;
			a_in.weightOneHanded = a_cfg.strengthWeightOneHanded;
			a_in.weightTwoHanded = a_cfg.strengthWeightTwoHanded;
			a_in.weightBlock = a_cfg.strengthWeightBlock;
			a_in.weightHeavyArmor = a_cfg.strengthWeightHeavyArmor;
			a_in.weightArchery = a_cfg.strengthWeightArchery;

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}

			if (auto* race = player->GetActorRuntimeData().race) {
				a_in.raceBaseMass = race->data.baseMass;
				a_in.raceFormID = race->formID;
			}

			using GetLevelFn = std::uint16_t (RE::Actor::*)(void) const;
			static REL::Relocation<GetLevelFn> getLevel{ REL::RelocationID(36344, 37334) };
			if (getLevel.address() != 0) {
				a_in.level = static_cast<float>(getLevel(player));
			}

			if (auto* valueOwner = player->AsActorValueOwner()) {
				a_in.oneHanded = valueOwner->GetActorValue(RE::ActorValue::kOneHanded);
				a_in.twoHanded = valueOwner->GetActorValue(RE::ActorValue::kTwoHanded);
				a_in.block = valueOwner->GetActorValue(RE::ActorValue::kBlock);
				a_in.heavyArmor = valueOwner->GetActorValue(RE::ActorValue::kHeavyArmor);
				a_in.archery = valueOwner->GetActorValue(RE::ActorValue::kArchery);
			}
			return true;
		}

		void LogStrengthDerivationOnce(const math::StrengthInputs& a_in, float a_strength, float a_power, float a_effectiveMass, bool a_havePlayer)
		{
			static std::atomic<bool> logged{ false };
			if (logged.exchange(true, std::memory_order_relaxed)) {
				return;
			}
			logger::info(
				"strength derivation (one-shot): race=0x{:08X} raceBaseMass={:.1f} referenceMass={:.1f} level={:.0f} "
				"skills 1H={:.0f} 2H={:.0f} Block={:.0f} HA={:.0f} Archery={:.0f} "
				"base={:.0f} levelGain={:.2f} skillGain={:.2f} -> P={:.3f} characterStrength={:.1f} "
				"effectiveMass={:.1f}{}",
				a_in.raceFormID, a_in.raceBaseMass, a_in.referenceMass, a_in.level,
				a_in.oneHanded, a_in.twoHanded, a_in.block, a_in.heavyArmor, a_in.archery,
				a_in.base, a_in.levelGain, a_in.skillGain, a_power, a_strength, a_effectiveMass,
				a_havePlayer ? "" : " (no player: defaults)");
		}

		// Main-thread: compute P and publish the mass the player contests with. The
		// physics callback only ever reads the atomic; it never touches the Actor.
		[[nodiscard]] float PublishPlayerPower(const Config& a_cfg, math::StrengthInputs& a_in, bool& a_havePlayer)
		{
			a_havePlayer = FillStrengthInputsFromPlayer(a_cfg, a_in);
			const float power = a_havePlayer ? math::StrengthPower(a_in) : 1.0f;
			g_effectivePlayerMass.store(math::EffectivePlayerMass(a_cfg.playerMass, power, a_cfg.massPowerExponent),
				std::memory_order_relaxed);
			return power;
		}
	}

	void PushModel::SetMainThreadId(std::thread::id a_id)
	{
		g_mainThreadId.store(static_cast<std::uint32_t>(std::hash<std::thread::id>{}(a_id)), std::memory_order_relaxed);
	}

	void PushModel::NoteCallbackThread()
	{
		if (g_callbackThreadId.load(std::memory_order_relaxed) == 0) {
			g_callbackThreadId.store(
				static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())),
				std::memory_order_relaxed);
		}
	}

	std::uint32_t PushModel::MainThreadId()
	{
		return g_mainThreadId.load(std::memory_order_relaxed);
	}

	std::uint32_t PushModel::CallbackThreadId()
	{
		return g_callbackThreadId.load(std::memory_order_relaxed);
	}

	void PushModel::OnCharacterContact(RE::hkpCharacterProxy* a_self,
		RE::hkpCharacterProxy* a_other,
		const RE::hkContactPoint* a_contact)
	{
		if (!a_self || !a_other || a_self == a_other) {
			return;
		}

		NoteCallbackThread();

		const auto& cfg = Config::Get();
		auto&       proxies = ProxyRegistry::Get();
		auto&       buffers = PushRegistry::Get();

		if (BudgetExceeded()) {
			CountGate(math::GateRefusal::kBudgetExceeded);
			return;
		}

		ProxyEntry     otherInfo{};
		const bool     known = proxies.Lookup(a_other, otherInfo);
		if (!known) {
			NoteUnknownTargetOnce(a_other);
		}

		math::GateInputs gates;
		gates.enabled = cfg.enabled;
		gates.useCharacterInteraction = cfg.useCharacterInteraction;
		gates.dialogueOpen = proxies.DialogueOpen();
		gates.disableDuringDialogue = cfg.disableDuringDialogue;
		gates.ragdoll = known && (otherInfo.flags & kProxyRagdoll);
		gates.dead = known && (otherInfo.flags & kProxyDead);
		gates.notPushable = known && (otherInfo.flags & kProxyNotPushable);
		gates.noCharacterCollisions = known && (otherInfo.flags & kProxyNoCharacterCollisions);
		gates.killMove = known && (otherInfo.flags & kProxyKillMove);
		gates.bleedout = known && (otherInfo.flags & kProxyBleedout);
		gates.inCombat = known && (otherInfo.flags & kProxyInCombat);
		gates.swimming = known && (otherInfo.flags & kProxySwimming);
		gates.disableInCombat = cfg.disableInCombat;
		gates.disableWhileSwimming = cfg.disableWhileSwimming;

		if (const auto refusal = math::EvaluateGates(gates); refusal != math::GateRefusal::kNone) {
			CountGate(refusal);
			return;
		}

		const math::Vec3 p_s = PhantomPosition(a_self);
		const math::Vec3 p_o = PhantomPosition(a_other);
		const math::Vec3 n_c = a_contact ? ToVec3(a_contact->separatingNormal) : math::Vec3{};

		math::Vec3 d;
		if (!math::ComputePushDirection(p_s, p_o, n_c, d)) {
			CountGate(math::GateRefusal::kDirectionDegenerate);
			return;
		}

		const float v_close = math::ClosingSpeed(d, ToVec3(a_self->velocity), ToVec3(a_other->velocity), cfg.minRelSpeed);
		if (v_close <= 0.0f) {
			CountGate(math::GateRefusal::kMinRelSpeed);
			return;
		}

		const float m_s = EffectivePlayerMass();  // self is the player's proxy by construction
		const float m_o = known ? otherInfo.mass : cfg.defaultCharacterMass;

		const float mu = math::MassRatio(m_s, m_o, cfg.massRatioMax);
		float       dv = (1.0f + cfg.restitution) * mu * v_close * cfg.pushScale;

		const auto heavy = math::HeavyGate(m_o, m_s, v_close, cfg.heavyMassRatio, cfg.heavyMinSpeed, cfg.heavyScale);
		if (heavy.refuse) {
			CountGate(math::GateRefusal::kHeavyGate);
			return;
		}
		dv *= heavy.scale;
		dv = math::CapDeltaV(dv, cfg.maxDeltaV);

		const bool inCombat = known && (otherInfo.flags & kProxyInCombat);
		const bool swimming = known && (otherInfo.flags & kProxySwimming);
		const bool airborne = known && (otherInfo.flags & kProxyAirborne);
		dv = math::ApplyStateScales(dv, inCombat, swimming, airborne, known,
			cfg.combatScale, cfg.outOfCombatScale, cfg.swimScale, cfg.airborneScale, cfg.unknownTargetScale);

		const float threshold = math::StaggerThreshold(cfg.staggerDeltaV, m_o, m_s, cfg.staggerMassExponent);
		// Gate 17: a registry miss pushes anyway (scaled) but never staggers.
		const bool  staggerAllowed = known && (inCombat || cfg.staggerOutOfCombat);

		const std::uint64_t now = FrameClock::NowMs();
		const std::uint32_t frame = FrameClock::CurrentFrame();
		const math::BufferParams params{
			cfg.pushCooldownMs,
			cfg.maxPushDurationMs,
			cfg.maxInjectedSpeed,
			cfg.staggerCooldownMs
		};

		bool applied = false;
		buffers.WithEntry(a_other, [&](PushEntry& e) {
			const auto update = math::UpdateBuffer(e.state, d, dv, now, frame, params, staggerAllowed, threshold);
			if (update.applyNow) {
				// Step 7: the push itself. A velocity is written, never a position
				// and never pushDelta, so the target's own manifold bounds it.
				a_other->velocity = a_other->velocity + ToHk(update.velocity);
				if (cfg.reactionOnPusher > 0.0f) {
					a_self->velocity = a_self->velocity - ToHk(math::Scale(update.velocity, cfg.reactionOnPusher));
				}
				applied = true;
			}
			if (update.stagger) {
				StaggerQueue::Get().Push(a_other, dv, d);
			}
		});

		if (applied && cfg.debugLog && DebugLineAllowed()) {
			logger::info("push: dv={:.2f} mu={:.3f} m_o={:.1f} known={} v_close={:.2f} (cooldown {}ms)",
				dv, mu, m_o, known, v_close, cfg.pushCooldownMs);
		}
	}

	void PushModel::OnObjectContact(RE::hkpCharacterProxy* a_self,
		const RE::hkpCharacterObjectInteractionEvent* a_input,
		RE::hkpCharacterObjectInteractionResult* a_output)
	{
		(void)a_self;
		const auto& cfg = Config::Get();
		if (!cfg.useObjectInteraction || !a_input || !a_output) {
			return;
		}
		// The engine's own impulse is the correct one; do not touch static bodies.
		if (!a_input->body || a_input->objectMassInv <= 0.0f) {
			return;
		}

		// Do not launch dragons/mammoths by walking into them. The comparison uses
		// the effective player mass (fPlayerMass * P^fMassPowerExponent), so a
		// maxed player contests dragon-scale mass instead of being curbstomped.
		const float effectiveMass = EffectivePlayerMass();
		const float bodyMass = 1.0f / a_input->objectMassInv;
		if (bodyMass > cfg.heavyMassRatio * effectiveMass) {
			return;
		}

		if (cfg.objectCustomImpulse) {
			const float m_char = effectiveMass;
			const float m_char_inv = m_char > 0.0f ? 1.0f / m_char : 0.0f;
			// Sign measured in game (design.md 4.2): v_close = -projectedVelocity.
			const float v_close = -a_input->projectedVelocity;
			const float j = (1.0f + cfg.objectRestitution) *
				std::max(0.0f, v_close - cfg.minRelSpeed) /
				(a_input->objectMassInv + m_char_inv);
			const float capped = cfg.maxObjectImpulse > 0.0f ? std::min(j, cfg.maxObjectImpulse) : j;
			// TODO(push): the normal's orientation relative to the body is unverified
			// (design.md 4.2); pin it from the logged dot() below before relying on it.
			const auto dir = math::HorizontalNormalize(ToVec3(a_input->normal));
			a_output->objectImpulse = ToHk(math::Scale(dir, capped * cfg.objectShoveScale));
			a_output->impulsePosition = a_input->position;
		} else {
			// Scale whatever objectImpulse currently holds: the engine's listener and
			// ours are peers in the same array and their order is unspecified.
			a_output->objectImpulse = ToHk(math::Scale(ToVec3(a_output->objectImpulse), cfg.objectShoveScale));
			a_output->impulsePosition = a_input->position;
		}

		if (!g_objectNormalLogged.exchange(true) && cfg.debugLog) {
			const float dot = math::Dot3(ToVec3(a_input->normal), ToVec3(a_output->objectImpulse));
			logger::info("object interaction: dot(normal, objectImpulse) = {:.3f} (pin the sign from this)", dot);
		}
	}

	void PushModel::ScanManifold(RE::hkpCharacterProxy* a_self,
		const RE::hkArray<RE::hkpRootCdPoint>& a_manifold)
	{
		if (!a_self) {
			return;
		}
		for (std::int32_t i = 0; i < a_manifold.size(); ++i) {
			const auto& point = a_manifold[i];
			RE::hkpCharacterProxy* other = nullptr;
			if (ProxyRegistry::Get().ProxyForCollidable(point.rootCollidableB, other) && other != a_self) {
				OnCharacterContact(a_self, other, &point.contact);
			} else if (ProxyRegistry::Get().ProxyForCollidable(point.rootCollidableA, other) && other != a_self) {
				OnCharacterContact(a_self, other, &point.contact);
			}
		}
	}

	void PushModel::ApplyProxyTuning(RE::hkpCharacterProxy* a_playerProxy)
	{
		if (!a_playerProxy) {
			return;
		}
		const auto& cfg = Config::Get();
		const float beforeStrength = a_playerProxy->characterStrength;
		const float beforeMass = a_playerProxy->characterMass;
		bool        changed = false;

		// Always publish P and the contested mass; the strength knob itself is the
		// derived value (fCharacterStrength < 0) or an explicit override (>= 0).
		math::StrengthInputs in;
		bool                havePlayer = false;
		const float         power = PublishPlayerPower(cfg, in, havePlayer);
		const float         effectiveMass = EffectivePlayerMass();
		const float         strength = math::ResolveCharacterStrength(cfg.characterStrength, in);
		LogStrengthDerivationOnce(in, strength, power, effectiveMass, havePlayer);

		if (a_playerProxy->characterStrength != strength) {
			a_playerProxy->characterStrength = strength;
			changed = true;
		}

		if (cfg.characterMass >= 0.0f && a_playerProxy->characterMass != cfg.characterMass) {
			a_playerProxy->characterMass = cfg.characterMass;
			changed = true;
		}

		// BUG 2: this used to log unconditionally, every frame. Log only when a
		// value actually changed or once per proxy (the first attach); a re-attach
		// to the same value writes nothing.
		static RE::hkpCharacterProxy* lastLogged = nullptr;
		if (!changed && lastLogged == a_playerProxy) {
			return;
		}
		lastLogged = a_playerProxy;
		logger::info("proxy tuning: characterStrength {:.1f} -> {:.1f}, characterMass {:.2f} -> {:.2f}",
			beforeStrength, a_playerProxy->characterStrength, beforeMass, a_playerProxy->characterMass);
	}

	void PushModel::TickMainThread(float a_deltaSec)
	{
		const auto& cfg = Config::Get();

		// Level and skills change at runtime (level-ups, skill training). Refresh the
		// published effective mass at most once per second; the physics callback
		// reads only the atomic, and this never logs.
		const auto nowMs = FrameClock::NowMs();
		if (g_lastPowerRefreshMs == 0 || nowMs - g_lastPowerRefreshMs >= 1000) {
			g_lastPowerRefreshMs = nowMs;
			math::StrengthInputs in;
			bool                havePlayer = false;
			PublishPlayerPower(cfg, in, havePlayer);
		}

		// Drain deferred staggers. The view is deliberately only read/queued here.
		StaggerQueue::Entry entry;
		while (StaggerQueue::Get().Pop(entry)) {
			ProxyEntry info{};
			ProxyRegistry::Get().Lookup(entry.proxy, info);
			if (cfg.debugLog) {
				logger::info("stagger queued: actor=0x{:X} dv={:.2f} (animation-graph call not implemented in v1)",
					reinterpret_cast<std::uintptr_t>(info.actor), entry.dv);
			}
			// TODO(push): fire the behavior-graph stagger event here once the right
			// API is confirmed (design.md 4.1 step 8 / escalation E4).
		}

		PushRegistry::Get().Sweep(FrameClock::NowMs(), cfg.pushDamping, a_deltaSec);
	}

	void PushModel::LogCalibrationOnce()
	{
		if (g_calibrationLogged.exchange(true)) {
			return;
		}
		auto* proxy = PlayerProxy();
		if (!proxy) {
			logger::info("calibration: no player proxy yet");
			return;
		}
		logger::info(
			"calibration: player proxy velocity={:.1f} u/s characterMass={:.2f} characterStrength={:.1f} "
			"listener vtable=240558 callback thread={} (main={})",
			math::Length3(ToVec3(proxy->velocity)),
			proxy->characterMass, proxy->characterStrength,
			CallbackThreadId(), MainThreadId());
	}
}
