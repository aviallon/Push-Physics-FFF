#include "PCH.h"

#include "PushListener.h"

#include "Config.h"
#include "Health.h"
#include "ProxyAccess.h"
#include "PushModel.h"

#include <atomic>
#include <cmath>

namespace pa
{
	namespace
	{
		std::atomic<std::uint64_t> g_orphanCalls{ 0 };

		// Gate 4: the leading const hkpCharacterProxy* on the slot-1 override is
		// an unverified NG reconstruction (U3). Prove the ABI on live data before
		// trusting a byte of the manifold; N consecutive violations turn the scan
		// off for good and mark the plugin DEGRADED.
		class Invariant
		{
		public:
			bool Ok(bool a_condition)
			{
				if (a_condition) {
					consecutiveFailures_ = 0;
					return true;
				}
				++consecutiveFailures_;
				if (consecutiveFailures_ == kDisableAfter) {
					logger::error("invariant 'proxy == owner && plausible frame time' failed {} times in a row; manifold scan disabled, health DEGRADED", kDisableAfter);
					Health::Get().Degrade("processConstraintsCallback ABI invariant failed");
				}
				if (consecutiveFailures_ >= kDisableAfter) {
					disabled_ = true;
				}
				return false;
			}

			[[nodiscard]] bool Disabled() const { return disabled_; }

		private:
			static constexpr int kDisableAfter = 8;
			int                  consecutiveFailures_ = 0;
			bool                 disabled_ = false;
		};

		Invariant& GetInvariant()
		{
			static Invariant invariant;
			return invariant;
		}
	}

	std::uint64_t OrphanCallCount()
	{
		return g_orphanCalls.load(std::memory_order_relaxed);
	}

	bool PushListener::AttachTo(RE::bhkCharProxyController* a_controller)
	{
		if (!a_controller) {
			return false;
		}

		// The controller was already verified with its vtable (AsProxyController /
		// PlayerController); its proxy is the authority. The old
		// proxy -> controller inversion (proxy - 0x350) is retired: the proxy is a
		// separately allocated Havok object, so its address has no fixed relation
		// to the controller's, and the inversion refused a real player proxy every
		// frame (BUG 1).
		auto* a_proxy = a_controller->GetCharacterProxy();
		if (!a_proxy) {
			return false;
		}

		// One-shot, non-gating: keep the retired inversion's mismatch visible.
		LogRetiredInversionOnce(a_proxy);

		if (owner_ == a_proxy) {
			return true;
		}

		// Record ownership BEFORE appending, so a callback racing the append is
		// still recognised as ours.
		owner_ = a_proxy;

		auto& listeners = a_proxy->listeners;
		for (std::int32_t i = 0; i < listeners.size(); ++i) {
			if (listeners[i] == this) {
				return true;  // already present: a re-attach must not double-register
			}
		}
		listeners.push_back(this);
		logger::info("listener attached to player proxy 0x{:X} (listeners now {})",
			reinterpret_cast<std::uintptr_t>(a_proxy), listeners.size());
		return true;
	}

	void PushListener::Detach()
	{
		owner_ = nullptr;
	}

	void PushListener::ProcessConstraintsCallback(const RE::hkpCharacterProxy* a_proxy,
		const RE::hkArray<RE::hkpRootCdPoint>& a_manifold,
		RE::hkpSimplexSolverInput& a_input)
	{
		constraintCalls_.fetch_add(1, std::memory_order_relaxed);
		if (!owner_) {
			return;  // not attached yet: nothing we can vouch for
		}

		auto& invariant = GetInvariant();
		if (invariant.Disabled()) {
			return;
		}

		const bool plausible = a_proxy == owner_ &&
			std::isfinite(a_input.deltaTime) && a_input.deltaTime > 0.0f && a_input.deltaTime <= 0.2f;
		if (!invariant.Ok(plausible)) {
			return;  // never dereference what we cannot vouch for
		}

		if (Config::Get().useManifoldScan) {
			PushModel::ScanManifold(const_cast<RE::hkpCharacterProxy*>(a_proxy), a_manifold);
		}
	}

	void PushListener::ContactPointAddedCallback(const RE::hkpCharacterProxy*, const RE::hkpRootCdPoint&)
	{
		// Detection uses CharacterInteractionCallback (and, optionally, ScanManifold).
	}

	void PushListener::ContactPointRemovedCallback(const RE::hkpCharacterProxy*, const RE::hkpRootCdPoint&)
	{
		// Detection uses CharacterInteractionCallback (and, optionally, ScanManifold).
	}

	void PushListener::CharacterInteractionCallback(RE::hkpCharacterProxy* a_proxy, RE::hkpCharacterProxy* a_otherProxy, const RE::hkContactPoint& a_contact)
	{
		if (a_proxy != owner_) {
			g_orphanCalls.fetch_add(1, std::memory_order_relaxed);
			return;  // gate 2: this listener is on a proxy that is no longer the player's
		}
		if (!a_otherProxy) {
			return;
		}
		characterCalls_.fetch_add(1, std::memory_order_relaxed);
		if (!Config::Get().useCharacterInteraction) {
			return;
		}
		PushModel::OnCharacterContact(a_proxy, a_otherProxy, &a_contact);
	}

	void PushListener::ObjectInteractionCallback(RE::hkpCharacterProxy* a_proxy, const RE::hkpCharacterObjectInteractionEvent& a_input, RE::hkpCharacterObjectInteractionResult& a_output)
	{
		if (a_proxy != owner_) {
			g_orphanCalls.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		objectCalls_.fetch_add(1, std::memory_order_relaxed);
		if (!Config::Get().useObjectInteraction) {
			return;
		}
		PushModel::OnObjectContact(a_proxy, &a_input, &a_output);
	}
}
