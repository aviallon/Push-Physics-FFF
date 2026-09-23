#pragma once

#include <RE/H/hkpCharacterProxyListener.h>

#include <atomic>
#include <cstdint>

// PushAside's character-proxy listener: the mechanism is listener-attach, not a
// code hook. The engine calls these virtuals on the player's own
// bhkCharProxyController; the listener is appended to that proxy's listeners
// array, so `a_proxy` in every callback is the player's proxy by construction
// and `owner_` is the orphan discriminator (design.md 3.2).
//
// THREADING (in-game evidence: callbacks run on Havok thread 564, the tick and
// attach on main thread 356):
//   * owner_        main thread writes (AttachTo/Detach), callbacks read. It is
//                   std::atomic with acquire/release so the orphan check cannot
//                   see a torn pointer or a value published before the listener
//                   was appended.
//   * counters      std::atomic<uint64_t>, incremented from the callback only.
//   * the listener's own invariants (Invariant in the .cpp) are touched only by
//                   the callback thread; they are callback-confined.
// Everything else the callback reaches (Config, ProxyRegistry, PushRegistry,
// StaggerQueue, Health, PushModel's published globals) is documented at its
// definition: read-only, seqlock, spinlock, SPSC or atomic respectively.

namespace RE
{
	class bhkCharProxyController;
}

namespace pa
{
	class PushListener final : public RE::hkpCharacterProxyListener
	{
	public:
		~PushListener() override = default;

		// hkpCharacterProxyListener overrides.
		void ProcessConstraintsCallback(const RE::hkpCharacterProxy* a_proxy, const RE::hkArray<RE::hkpRootCdPoint>& a_manifold, RE::hkpSimplexSolverInput& a_input) override;
		void ContactPointAddedCallback(const RE::hkpCharacterProxy* a_proxy, const RE::hkpRootCdPoint& a_point) override;
		void ContactPointRemovedCallback(const RE::hkpCharacterProxy* a_proxy, const RE::hkpRootCdPoint& a_point) override;

		// Slot 4 - Path 1: the player's capsule contacts another character's proxy.
		void CharacterInteractionCallback(RE::hkpCharacterProxy* a_proxy, RE::hkpCharacterProxy* a_otherProxy, const RE::hkContactPoint& a_contact) override;

		// Slot 5 - Path 2: the player's capsule contacts a rigid body.
		void ObjectInteractionCallback(RE::hkpCharacterProxy* a_proxy, const RE::hkpCharacterObjectInteractionEvent& a_input, RE::hkpCharacterObjectInteractionResult& a_output) override;

		// Main thread only, idempotent. `a_controller` must be the already-verified
		// controller from PlayerController()/AsProxyController(); the proxy is read
		// back from it. Returns true once `owner_` is that controller's proxy.
		[[nodiscard]] bool AttachTo(RE::bhkCharProxyController* a_controller);
		void              Detach();  // main thread only
		[[nodiscard]] RE::hkpCharacterProxy* Owner() const { return owner_.load(std::memory_order_acquire); }

		[[nodiscard]] std::uint64_t CharacterCalls() const { return characterCalls_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t ObjectCalls() const { return objectCalls_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t ConstraintCalls() const { return constraintCalls_.load(std::memory_order_relaxed); }

	private:
		// Written by the main thread, read by the physics callback (see THREADING).
		std::atomic<RE::hkpCharacterProxy*> owner_{ nullptr };
		std::atomic<std::uint64_t> characterCalls_{ 0 };
		std::atomic<std::uint64_t> objectCalls_{ 0 };
		std::atomic<std::uint64_t> constraintCalls_{ 0 };
	};

	// Gate 2/17 evidence: how many calls arrived for a proxy that was no longer
	// the player's.
	[[nodiscard]] std::uint64_t OrphanCallCount();
}
