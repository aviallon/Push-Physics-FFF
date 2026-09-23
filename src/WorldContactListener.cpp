#include "PCH.h"

#include "WorldContactListener.h"

#include "FrameClock.h"
#include "HkMath.h"
#include "Hooks/HavokUtil.h"
#include "ProxyAccess.h"
#include "ProxyRegistry.h"

#include <RE/A/Actor.h>
#include <RE/B/BSAtomic.h>
#include <RE/B/bhkWorld.h>
#include <RE/H/hkContactPoint.h>
#include <RE/H/hkpCollidable.h>
#include <RE/H/hkpCollisionEvent.h>
#include <RE/H/hkpContactPointEvent.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/H/hkpShapePhantom.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESHavokUtilities.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>

namespace pa
{
	namespace
	{
		WorldContactListener g_worldContactListener;

		// The world our listener was last registered on (main thread only).
		std::atomic<RE::hkpWorld*> g_lastWorld{ nullptr };

		// The player's collision group (CFilter bits 16-31), recorded by
		// EnsurePlayerBodyIdentity(). Identity by group survives a null userData,
		// which identity by refr does not. 0 means "not recorded", and a zero group
		// is warned about once because it makes the group probe vacuous.
		std::atomic<std::uint32_t> g_playerGroup{ 0 };
		std::atomic<bool>          g_groupWarned{ false };
		std::atomic<std::uintptr_t> g_lastIdentityLog{ 0 };
		std::atomic<std::uintptr_t> g_lastBumpPair{ 0 };

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

		[[nodiscard]] std::uint32_t FilterInfoOf(RE::hkpRigidBody* a_body)
		{
			auto* col = a_body ? a_body->GetCollidable() : nullptr;
			return col ? col->broadPhaseHandle.collisionFilterInfo.filter : 0u;
		}

		// The positive control. Resolves a contact body's collidable against the
		// registry, which stores every registered character's PHANTOM collidable
		// (ProxyRegistry FillEntry). Returns true when the body is a known character
		// phantom, and reports whether that character is the player. This is what
		// makes a zero meaningful: if no body ever resolves, character phantoms do
		// not reach this channel at all and the player's absence is not evidence.
		[[nodiscard]] bool BodyIsRegisteredPhantom(RE::hkpRigidBody* a_body, bool& a_isPlayer)
		{
			a_isPlayer = false;
			auto* col = a_body ? a_body->GetCollidable() : nullptr;
			if (!col) {
				return false;
			}
			RE::hkpCharacterProxy* proxy = nullptr;
			if (!ProxyRegistry::Get().ProxyForCollidable(col, proxy)) {
				return false;
			}
			ProxyEntry entry{};
			if (proxy && ProxyRegistry::Get().Lookup(proxy, entry)) {
				// flags only: the registry contract forbids dereferencing the Actor*
				// from off-thread (design.md 3.4).
				a_isPlayer = (entry.flags & kProxyPlayer) != 0;
				return true;
			}
			// A collidable hit without a usable entry still proves a phantom is here.
			return true;
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

		const auto playerGroup = g_playerGroup.load(std::memory_order_relaxed);

		// Null userData => the body is not in the engine's collidable->refr map
		// (static world, or a body the map does not cover). It does NOT mean "not an
		// actor": that is why identity is also checked by collision group and,
		// decisively, by resolving the collidable against the registry.
		auto*      refrA = BodyRef(a_event.bodies[0]);
		auto*      refrB = BodyRef(a_event.bodies[1]);
		const bool playerA = refrA && refrA->IsPlayerRef();
		const bool playerB = refrB && refrB->IsPlayerRef();

		const std::uint32_t filterA = FilterInfoOf(a_event.bodies[0]);
		const std::uint32_t filterB = FilterInfoOf(a_event.bodies[1]);
		const bool          groupHit = playerGroup != 0 &&
			((filterA >> 16) == playerGroup || (filterB >> 16) == playerGroup);

		bool phantomHit = false;
		bool phantomPlayer = false;
		for (std::size_t i = 0; i < 2; ++i) {
			bool isPlayer = false;
			if (BodyIsRegisteredPhantom(a_event.bodies[i], isPlayer)) {
				phantomHit = true;
				phantomPlayer = phantomPlayer || isPlayer;
			}
		}
		if (phantomHit) {
			bodyPhantom_.fetch_add(1, std::memory_order_relaxed);
		}
		if (phantomPlayer) {
			bodyPhantomPlayer_.fetch_add(1, std::memory_order_relaxed);
		}

		if (groupHit) {
			pcGroup_.fetch_add(1, std::memory_order_relaxed);
		}

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

		// Superset tag only, NOT a player signal: every NPC-vs-world pair lands here
		// too (the swallowing bucket is `other_`). It is recorded so the log shows how
		// large that bucket is, never to conclude anything about the player.
		auto*      identified = refrA ? refrA : refrB;
		const bool oneNull = (refrA == nullptr) != (refrB == nullptr);
		const bool nullVsActor = oneNull && identified && identified->As<RE::Actor>();
		if (nullVsActor) {
			nullVsActor_.fetch_add(1, std::memory_order_relaxed);
		}

		if ((groupHit || phantomHit || nullVsActor) && TryClaimLogSlot()) {
			logger::info("world contact event: groupHit={} phantomHit={} phantomPlayer={} nullVsActor={} "
						 "filterA=0x{:08X} refrA=0x{:08X} '{}' filterB=0x{:08X} refrB=0x{:08X} '{}' "
						 "playerGroup=0x{:04X}",
				groupHit, phantomHit, phantomPlayer, nullVsActor,
				filterA, refrA ? refrA->GetFormID() : 0u, SafeName(refrA),
				filterB, refrB ? refrB->GetFormID() : 0u, SafeName(refrB),
				static_cast<std::uint16_t>(playerGroup));
		}
	}

	bool WorldContactListener::TryClaimLogSlot()
	{
		// Bounded: at most kMaxLogs lines, one per kLogWindowMs. Everything above
		// the gate is a couple of atomic loads, so the hot path costs nothing once
		// the cap is reached. Shared by both callbacks, so the cap is global.
		if (logged_.load(std::memory_order_relaxed) >= kMaxLogs) {
			return false;
		}
		const auto now = FrameClock::NowMs();
		const auto last = lastLogMs_.load(std::memory_order_relaxed);
		if (last != 0 && now - last < kLogWindowMs) {
			return false;
		}
		lastLogMs_.store(now, std::memory_order_relaxed);
		logged_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void WorldContactListener::ContactPointCallback(const RE::hkpContactPointEvent& a_event)
	{
		// The predicate comes FIRST and only then the shared budget is claimed, or a
		// callback that fires orders of magnitude more often would starve the
		// CollisionAddedCallback diagnostic to zero lines.
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
		if (!TryClaimLogSlot()) {
			return;
		}

		lastLogMs_.store(FrameClock::NowMs(), std::memory_order_relaxed);

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
			bodyPhantom_.load(std::memory_order_relaxed),
			bodyPhantomPlayer_.load(std::memory_order_relaxed),
			pcGroup_.load(std::memory_order_relaxed),
			nullVsActor_.load(std::memory_order_relaxed),
			logged_.load(std::memory_order_relaxed)
		};
	}

	void EnsurePlayerBodyIdentity()
	{
		auto* proxy = PlayerProxy();
		if (!proxy || !proxy->shapePhantom) {
			return;
		}
		auto* collidable = proxy->shapePhantom->GetCollidable();
		if (!collidable) {
			return;
		}

		const auto& filter = collidable->broadPhaseHandle.collisionFilterInfo;
		const auto  group = filter.GetSystemGroup();
		g_playerGroup.store(group, std::memory_order_relaxed);

		if (group == 0) {
			// Do not let "no data" read as "no hits": with a zero group every
			// group comparison is false and pcGroup can never move.
			bool warned = false;
			if (g_groupWarned.compare_exchange_strong(warned, true, std::memory_order_relaxed)) {
				logger::warn("collision-group probe DISABLED: the player phantom's system group is 0, "
							 "so pcGroup/groupHit can never fire; use bodyPhantom instead");
			}
		}

		// Log on CHANGE, not on first sight: the first frame a player proxy exists can
		// be mid-load, and engineMapResolves=false is then a timing artefact rather
		// than an answer.
		const auto key = reinterpret_cast<std::uintptr_t>(collidable) ^ (static_cast<std::uintptr_t>(group) << 48);
		if (g_lastIdentityLog.load(std::memory_order_relaxed) == key) {
			return;
		}
		g_lastIdentityLog.store(key, std::memory_order_relaxed);

		// Does the ENGINE's collidable->refr map resolve the player's phantom? If it
		// does not, no player<->actor contact can ever be counted by refr, and body
		// identity has to come from the registry instead.
		auto* mapped = RE::TESHavokUtilities::FindCollidableRef(*collidable);
		logger::info("player body identity: collidable=0x{:X} filterInfo=0x{:08X} group=0x{:04X} layer={} "
					 "engineMapResolves={} (engineMapRef=0x{:X})",
			reinterpret_cast<std::uintptr_t>(collidable), filter.filter, group,
			static_cast<std::int32_t>(collidable->GetCollisionLayer()),
			mapped != nullptr,
			reinterpret_cast<std::uintptr_t>(mapped));
	}

	void ProbePlayerBumpRecord()
	{
		auto* ctrl = PlayerController();
		if (!ctrl) {
			return;
		}

		auto* body = ctrl->bumpedBody.get();
		auto* charBody = ctrl->bumpedCharCollisionObject.get();

		// The engine leaves the last bump in place, so this is edge-triggered on the
		// pair of pointers: a level-triggered log would either repeat one bump
		// forever or miss the bump entirely.
		const auto pair = reinterpret_cast<std::uintptr_t>(body) ^
			(reinterpret_cast<std::uintptr_t>(charBody) * 0x9E3779B97F4A7C15ull);
		if (g_lastBumpPair.load(std::memory_order_relaxed) == pair) {
			return;
		}
		g_lastBumpPair.store(pair, std::memory_order_relaxed);
		if (!body && !charBody) {
			return;  // cleared: not a bump
		}

		auto* bodyRefr = body ? body->GetUserData() : nullptr;
		auto* charRefr = charBody ? charBody->GetUserData() : nullptr;
		logger::info("player bump record: bumpedBody=0x{:X} (refr=0x{:08X} '{}') "
					 "bumpedCharCollisionObject=0x{:X} (refr=0x{:08X} '{}') bumpedForce={:.2f} supportBody=0x{:X}",
			reinterpret_cast<std::uintptr_t>(body), bodyRefr ? bodyRefr->GetFormID() : 0u, SafeName(bodyRefr),
			reinterpret_cast<std::uintptr_t>(charBody), charRefr ? charRefr->GetFormID() : 0u, SafeName(charRefr),
			ctrl->bumpedForce, reinterpret_cast<std::uintptr_t>(ctrl->supportBody.get()));
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

		// The player proxy is recreated on load (the log shows a new proxy address),
		// so the recorded identity and the bump edges belong to a dead generation.
		// Clearing them also guarantees the identity line is re-emitted for the new
		// generation instead of the answer being frozen from before the load.
		g_playerGroup.store(0, std::memory_order_relaxed);
		g_lastIdentityLog.store(0, std::memory_order_relaxed);
		g_lastBumpPair.store(0, std::memory_order_relaxed);
	}
}
