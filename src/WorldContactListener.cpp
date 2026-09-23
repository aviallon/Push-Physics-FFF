#include "PCH.h"

#include "WorldContactListener.h"

#include "FrameClock.h"
#include "HkMath.h"
#include "Hooks/HavokUtil.h"

#include <RE/A/Actor.h>
#include <RE/B/BSAtomic.h>
#include <RE/B/bhkWorld.h>
#include <RE/H/hkContactPoint.h>
#include <RE/H/hkpCollisionEvent.h>
#include <RE/H/hkpContactPointEvent.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

namespace pa
{
	namespace
	{
		WorldContactListener g_worldContactListener;

		// The world our listener was last registered on (main thread only).
		std::atomic<RE::hkpWorld*> g_lastWorld{ nullptr };

		constexpr std::uint64_t kLogWindowMs = 5000;
		constexpr std::uint64_t kMaxLogs = 20;

		// A silent early return here is indistinguishable from "this channel simply
		// has no contacts", which is how a mis-typed RE signature survived a whole
		// test run. Every gate therefore names itself once; the tick stays quiet after
		// that, so this cannot become a log flood.
		void NoteGateOnce(std::atomic<bool>& a_flag, const char* a_gate)
		{
			bool expected = false;
			if (a_flag.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
				logger::warn("world contact listener: not registered yet - {}", a_gate);
			}
		}

		std::atomic<bool> g_gateNoPlayer{ false };
		std::atomic<bool> g_gateNoCell{ false };
		std::atomic<bool> g_gateNoWorld{ false };
		std::atomic<bool> g_gateNoHkWorld{ false };
		std::atomic<bool> g_gateUtil{ false };
		std::atomic<bool> g_gateAdd{ false };
		std::atomic<bool> g_gateVerify{ false };

		[[nodiscard]] RE::TESObjectREFR* BodyRef(RE::hkpRigidBody* a_body)
		{
			return a_body ? a_body->GetUserData() : nullptr;
		}

		[[nodiscard]] const char* SafeName(RE::TESObjectREFR* a_refr)
		{
			if (!a_refr) {
				return "";
			}
			const char* name = a_refr->GetDisplayFullName();
			return name ? name : "";
		}
	}

	WorldContactListener& GetWorldContactListener()
	{
		return g_worldContactListener;
	}

	void WorldContactListener::CollisionAddedCallback(const RE::hkpCollisionEvent& a_event)
	{
		collisionAdded_.fetch_add(1, std::memory_order_relaxed);

		// Null userData => the body is not a placed reference (static world,
		// projectile with no REFR); that is the "other" bucket.
		auto*      refrA = BodyRef(a_event.bodies[0]);
		auto*      refrB = BodyRef(a_event.bodies[1]);
		const bool playerA = refrA && refrA->IsPlayerRef();
		const bool playerB = refrB && refrB->IsPlayerRef();

		if (playerA || playerB) {
			auto* other = playerA ? refrB : refrA;
			if (other && other->As<RE::Actor>()) {
				pcActor_.fetch_add(1, std::memory_order_relaxed);
			} else if (other) {
				pcObject_.fetch_add(1, std::memory_order_relaxed);
			} else {
				other_.fetch_add(1, std::memory_order_relaxed);
			}
		} else if (refrA && refrB && refrA->As<RE::Actor>() && refrB->As<RE::Actor>()) {
			actorActor_.fetch_add(1, std::memory_order_relaxed);
		} else {
			other_.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void WorldContactListener::ContactPointCallback(const RE::hkpContactPointEvent& a_event)
	{
		// Bounded diagnostic only: at most kMaxLogs lines, one per kLogWindowMs.
		// Everything above this gate is a couple of atomic loads, so the hot path
		// costs nothing once the cap is reached.
		if (logged_.load(std::memory_order_relaxed) >= kMaxLogs) {
			return;
		}
		const auto now = FrameClock::NowMs();
		const auto last = lastLogMs_.load(std::memory_order_relaxed);
		if (last != 0 && now - last < kLogWindowMs) {
			return;
		}

		auto*      refrA = BodyRef(a_event.bodies[0]);
		auto*      refrB = BodyRef(a_event.bodies[1]);
		const bool playerA = refrA && refrA->IsPlayerRef();
		const bool playerB = refrB && refrB->IsPlayerRef();
		if (!playerA && !playerB) {
			return;
		}
		auto* playerRefr = playerA ? refrA : refrB;
		auto* otherRefr = playerA ? refrB : refrA;
		if (!otherRefr || !otherRefr->As<RE::Actor>()) {
			return;
		}

		lastLogMs_.store(now, std::memory_order_relaxed);
		logged_.fetch_add(1, std::memory_order_relaxed);

		const auto*     cp = a_event.contactPoint;
		const math::Vec3 pos = cp ? ToVec3(cp->position) : math::Vec3{};
		const math::Vec3 nrm = cp ? ToVec3(cp->separatingNormal) : math::Vec3{};

		logger::info("world contact: player<->actor player=0x{:08X} '{}' actor=0x{:08X} '{}' "
					 "point=({:.1f},{:.1f},{:.1f}) normal=({:.2f},{:.2f},{:.2f}) logged={}/{}",
			playerRefr->GetFormID(), SafeName(playerRefr),
			otherRefr->GetFormID(), SafeName(otherRefr),
			pos.x, pos.y, pos.z, nrm.x, nrm.y, nrm.z,
			logged_.load(std::memory_order_relaxed), kMaxLogs);
	}

	WorldContactListener::Stats WorldContactListener::Snapshot() const
	{
		return {
			collisionAdded_.load(std::memory_order_relaxed),
			pcActor_.load(std::memory_order_relaxed),
			pcObject_.load(std::memory_order_relaxed),
			actorActor_.load(std::memory_order_relaxed),
			other_.load(std::memory_order_relaxed),
			logged_.load(std::memory_order_relaxed)
		};
	}

	void EnsureWorldContactRegistration()
	{
		havok::LogResolvedAddressesOnce();

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			NoteGateOnce(g_gateNoPlayer, "no PlayerCharacter singleton");
			return;
		}
		auto* cell = player->GetParentCell();
		if (!cell) {
			NoteGateOnce(g_gateNoCell, "player has no parent cell");
			return;
		}
		// NiPointer holds the reference the engine handed us for this tick;
		// registering the listener does not extend the world's life.
		RE::NiPointer<RE::bhkWorld> world(cell->GetbhkWorld());
		if (!world) {
			NoteGateOnce(g_gateNoWorld, "cell has no bhkWorld");
			return;
		}
		auto* hkWorld = world->GetWorld2();
		if (!hkWorld) {
			NoteGateOnce(g_gateNoHkWorld, "bhkWorld has no ahkpWorld");
			return;
		}
		if (g_lastWorld.load(std::memory_order_acquire) == hkWorld) {
			return;  // already handled this world
		}

		std::int32_t listenerCount = 0;
		{
			// The world lock is what Precision takes around listener mutation.
			RE::BSWriteLockGuard lock(world->worldLock);
			if (!havok::HasContactListener(hkWorld, &g_worldContactListener)) {
				if (!havok::EnsureContactCallbackUtil(hkWorld)) {
					NoteGateOnce(g_gateUtil, "collision-callback util could not be ensured");
					return;  // retry next frame, null log is one-shot
				}
				if (!havok::TryAddContactListener(hkWorld, &g_worldContactListener)) {
					NoteGateOnce(g_gateAdd, "hkpWorld_addContactListener call failed");
					return;
				}
				// Assert the effect, not the call: an add that did nothing (wrong id,
				// wrong world) would otherwise masquerade as "no contacts exist".
				if (!havok::HasContactListener(hkWorld, &g_worldContactListener)) {
					NoteGateOnce(g_gateVerify, "listener absent from contactListeners after add");
					return;
				}
			}
			listenerCount = hkWorld->contactListeners.size();
		}

		g_lastWorld.store(hkWorld, std::memory_order_release);
		logger::info("world contact listener registered on hkpWorld 0x{:X} (cell 0x{:08X}, listeners now {})",
			reinterpret_cast<std::uintptr_t>(hkWorld), cell->GetFormID(), listenerCount);
	}

	void ResetWorldContactRegistration()
	{
		// The listener itself stays valid across a load; only the "which world did
		// we last touch" cache is cleared, so the next tick re-scans and
		// re-registers on the reloaded world.
		g_lastWorld.store(nullptr, std::memory_order_release);
	}
}
