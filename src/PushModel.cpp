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

		const float m_s = cfg.playerMass;  // self is the player's proxy by construction
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

		// Do not launch dragons/mammoths by walking into them.
		const float bodyMass = 1.0f / a_input->objectMassInv;
		if (bodyMass > cfg.heavyMassRatio * cfg.playerMass) {
			return;
		}

		if (cfg.objectCustomImpulse) {
			const float m_char = cfg.playerMass;
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
		if (cfg.characterStrength >= 0.0f) {
			a_playerProxy->characterStrength = cfg.characterStrength;
		}
		if (cfg.characterMass >= 0.0f) {
			a_playerProxy->characterMass = cfg.characterMass;
		}
		logger::info("proxy tuning: characterStrength {:.1f} -> {:.1f}, characterMass {:.2f} -> {:.2f}",
			beforeStrength, a_playerProxy->characterStrength, beforeMass, a_playerProxy->characterMass);
	}

	void PushModel::TickMainThread(float a_deltaSec)
	{
		const auto& cfg = Config::Get();

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
