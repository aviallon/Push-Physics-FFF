#include "PCH.h"

#include "WorldContactListener.h"

#include "BumpSlot.h"
#include "Config.h"
#include "FrameClock.h"
#include "HkMath.h"
#include "Hooks/HavokUtil.h"
#include "ProxyAccess.h"
#include "ProxyRegistry.h"
#include "PushModel.h"

#include <RE/A/Actor.h>
#include <RE/B/BSAtomic.h>
#include <RE/B/bhkWorld.h>
#include <RE/H/hkContactPoint.h>
#include <RE/H/hkpCharacterProxy.h>
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
		std::atomic<std::uint32_t>  g_playerGroup{ 0 };
		std::atomic<bool>           g_groupWarned{ false };
		std::atomic<std::uintptr_t> g_lastIdentityLog{ 0 };
		// Two atomics rather than one hash: a hashed pair could collide with the
		// initialised "nothing yet" value and silently swallow that bump forever.
		std::atomic<std::uintptr_t> g_lastBumpBody{ 0 };
		std::atomic<std::uintptr_t> g_lastBumpChar{ 0 };
		// The bump probe has its own cap and no window: it runs on the main thread
		// and is edge-triggered, so it cannot flood, but it also must not be
		// unbounded (alternating bumper pairs would otherwise log every tick).
		std::atomic<std::uint64_t> g_bumpLines{ 0 };
		constexpr std::uint64_t    kBumpLogMax = 20;

		// --- bump-record detection -------------------------------------------
		// Forward declaration: SafeName is defined below with the other diagnostics,
		// but the bump publishing path (which runs before it in this namespace) needs
		// it. Physics-thread code must not call it - only main-thread callers do.
		[[nodiscard]] const char* SafeName(RE::TESObjectREFR* a_refr);

		// Single-slot handoff: the main thread resolves the engine's bump record to
		// a character proxy and Publishes it here; the physics thread Take()s it
		// (exchange-clear) inside ProcessConstraintsCallback. Latest wins, nulls are
		// consumed by TakeForApply and never applied.
		SingleSlot<RE::hkpCharacterProxy> g_bumpTarget;

		// Distinct bump-detected targets and the cumulative pushes actually applied
		// through this path. The distinct set is written only on the main thread, so
		// it needs no lock; the counters are atomics because the push counter is
		// incremented from the physics thread.
		constexpr std::size_t              kBumpTargetSlots = 128;
		RE::hkpCharacterProxy*             g_bumpTargetsSeen[kBumpTargetSlots]{};
		std::size_t                        g_bumpTargetsSeenCount = 0;
		std::atomic<std::uint64_t>         g_bumpTargetCount{ 0 };
		std::atomic<std::uint64_t>         g_bumpPushCount{ 0 };

		// The first push actually applied through the bump path is latched by the
		// physics thread so the main thread can name its actor (names must not be
		// resolved off-thread). Release/acquire ordering publishes target/dv before
		// the latch flag is observed.
		std::atomic<bool>                  g_bumpPushLatchSet{ false };
		std::atomic<bool>                  g_bumpPushLatched{ false };
		std::atomic<RE::hkpCharacterProxy*> g_bumpPushTarget{ nullptr };
		std::atomic<float>                 g_bumpPushDv{ 0.0f };

		// Bounded logging for detection: a session emits at most kBumpDetectLogMax
		// lines across "new target" and "first push applied". Written only by the
		// main thread (ProbePlayerBumpRecord / ReportBumpDetection).
		constexpr std::uint64_t kBumpDetectLogMax = 10;
		constexpr std::uint64_t kBumpTargetLogMax = kBumpDetectLogMax - 1;
		std::uint64_t           g_bumpDetectLines = 0;
		// Proximity guard, see WithinBumpRange(). Withheld duplicates are collapsed so
		// a sticky record cannot spend the bounded log on one repeated target.
		RE::hkpCharacterProxy* g_bumpWithheldTarget = nullptr;

		[[nodiscard]] bool ClaimBumpDetectLine(std::uint64_t a_cap)
		{
			if (g_bumpDetectLines >= a_cap) {
				return false;
			}
			++g_bumpDetectLines;
			return true;
		}

		// Main thread only. Resolve the engine's bumped character rigid body to the
		// other character's proxy: userData -> TESObjectREFR -> Actor -> verified		// controller -> proxy. Every step is required, so a rock, a null userData or
		// the player's own body yields no target. Publish(nullptr) when there is none,
		// so a stale target from a previous frame is not re-applied.
		// The engine can leave the last bump in its record (observed: the same
		// bumpedCharCollisionObject across ~20 s). Publishing it every frame means a
		// stale target could be pushed across the room, because the model's gates only
		// test direction and closing speed, never distance. So the publish is gated on
		// proximity.
		//
		// 200 world units ~ 2.9 m (1 unit ~ 1.4 cm) between capsule CENTRES: comfortably
		// beyond capsule-contact distance for humanoids (radius ~35 units each) and
		// medium creatures, tight enough that a truly stale target cannot be reached.
		// Deliberately generous rather than exact - a false withhold is a missed push,
		// which is far better than a shove at range. Dragons stay out of scope anyway.
		constexpr float kBumpMaxRangeUnits = 200.0f;
		std::atomic<std::uint64_t> g_bumpWithheldLines{ 0 };
		constexpr std::uint64_t    kBumpWithheldLogMax = 4;

		// Main thread. True when the two character capsules are close enough for the
		// contact to be real. Missing data (no player proxy, no phantom) is NOT treated
		// as "in range": refusing to publish is the safe direction.
		[[nodiscard]] bool WithinBumpRange(const RE::hkpCharacterProxy* a_target)
		{
			auto* player = PlayerProxy();
			if (!player || !a_target || !player->shapePhantom || !a_target->shapePhantom) {
				return false;
			}
			const math::Vec3 p = ToVec3(player->shapePhantom->motionState.transform.translation);
			const math::Vec3 t = ToVec3(a_target->shapePhantom->motionState.transform.translation);
			const float      dx = p.x - t.x;
			const float      dy = p.y - t.y;
			const float      dz = p.z - t.z;
			return (dx * dx + dy * dy + dz * dz) <= (kBumpMaxRangeUnits * kBumpMaxRangeUnits);
		}

		// Last bump record we logged, so each change of the engine's record produces one
		// line whether or not it resolved. Without this, "record set but unresolved" is
		// completely silent - the same class of blind spot the guardrails exist for.
		RE::hkpRigidBody* g_lastLoggedCharBody = nullptr;
		// The controller-class probe has its OWN budget: the record-change line above
		// flaps (set/cleared while touching) and would exhaust a shared one before the
		// class answer, which is the line that decides the push mechanism, ever prints.
		std::uintptr_t          g_lastClassVptr = 1;
		std::atomic<std::uint64_t> g_classLines{ 0 };
		constexpr std::uint64_t    kClassLogMax = 3;

		void PublishBumpTargetFrom(RE::hkpRigidBody* a_charBody)
		{
			auto* refr = a_charBody ? a_charBody->GetUserData() : nullptr;
			auto* actor = refr ? refr->As<RE::Actor>() : nullptr;
			RE::bhkCharacterController* ctrl = nullptr;
			RE::hkpCharacterProxy*      target = nullptr;
			if (actor && actor != RE::PlayerCharacter::GetSingleton()) {
				ctrl = actor->GetCharController();
				if (auto* pc = AsProxyController(ctrl)) {
					target = pc->GetCharacterProxy();
				}
			}

			// A non-null controller that is NOT a proxy controller is the one case where
			// the push mechanism has to differ, so name the class from the vptr instead of
			// leaving "proxy=0" ambiguous between "no controller" and "another subclass".
			if (actor && !target) {
				const auto vptr = ctrl ? *reinterpret_cast<const std::uintptr_t*>(ctrl) : 0;
				if (vptr != g_lastClassVptr && g_classLines.load(std::memory_order_relaxed) < kClassLogMax) {
					g_lastClassVptr = vptr;
					g_classLines.fetch_add(1, std::memory_order_relaxed);
					static REL::Relocation<std::uintptr_t> kProxyVt{ RE::VTABLE_bhkCharProxyController[1] };
					static REL::Relocation<std::uintptr_t> kRigidVt{ RE::VTABLE_bhkCharRigidBodyController[1] };
					logger::info("bump target controller: ctrl=0x{:X} vptr=0x{:X} proxyVtable=0x{:X} "
								 "rigidBodyVtable=0x{:X} rigidBody=0x{:X} isProxyController={}",
						reinterpret_cast<std::uintptr_t>(ctrl), vptr,
						kProxyVt.address(), kRigidVt.address(),
						reinterpret_cast<std::uintptr_t>(ctrl ? ctrl->GetRigidBody() : nullptr),
						ctrl && vptr == kProxyVt.address());
				}
			}

			// One line per CHANGE of the engine's record, naming which step of the
			// resolution chain produced what. A null charBody means the engine reports no
			// bump at all (log once for the transition); a non-null charBody with
			// resolved=false says the field is set but the chain broke here.
			if (g_lastLoggedCharBody != a_charBody) {
				g_lastLoggedCharBody = a_charBody;
				if (ClaimBumpDetectLine(kBumpDetectLogMax)) {
					logger::info("bump record: charBody=0x{:X} refr=0x{:08X} actor=0x{:X} ctrl=0x{:X} proxy=0x{:X} "
								 "resolved={} inRange={}",
						reinterpret_cast<std::uintptr_t>(a_charBody),
						refr ? refr->GetFormID() : 0u,
						reinterpret_cast<std::uintptr_t>(actor),
						reinterpret_cast<std::uintptr_t>(ctrl),
						reinterpret_cast<std::uintptr_t>(target),
						target != nullptr,
						target && WithinBumpRange(target));
				}
			}

			// Withheld targets are neither published nor counted as detections, so the
			// counters stay honest. A withheld line is also the measurement of whether the
			// engine's bump record is sticky, which is formally unknown.
			if (target && !WithinBumpRange(target)) {
				g_bumpTarget.Publish(nullptr);
				if (g_bumpWithheldTarget != target &&
					g_bumpWithheldLines.load(std::memory_order_relaxed) < kBumpWithheldLogMax) {
					g_bumpWithheldTarget = target;
					g_bumpWithheldLines.fetch_add(1, std::memory_order_relaxed);
					logger::info("bump detection: withheld stale target actor=0x{:08X} beyond {:.0f} units "
								 "(the engine's bump record outlived the contact)",
						reinterpret_cast<std::uintptr_t>(actor), kBumpMaxRangeUnits);
				}
				return;
			}

			g_bumpTarget.Publish(target);
			if (!target) {
				return;
			}

			for (std::size_t i = 0; i < g_bumpTargetsSeenCount; ++i) {
				if (g_bumpTargetsSeen[i] == target) {
					return;  // already counted and logged
				}
			}
			if (g_bumpTargetsSeenCount < kBumpTargetSlots) {
				g_bumpTargetsSeen[g_bumpTargetsSeenCount++] = target;
			}
			g_bumpTargetCount.fetch_add(1, std::memory_order_relaxed);

			// Unconditional (not gated on bDebugLog): detection evidence is the point,
			// and this line is capped. Names are resolved here, on the main thread.
			if (ClaimBumpDetectLine(kBumpTargetLogMax)) {
				logger::info("bump detection: new target proxy=0x{:X} actor=0x{:08X} '{}' bumpTargets={} pairs={}",
					reinterpret_cast<std::uintptr_t>(target), actor->GetFormID(), SafeName(actor),
					BumpTargetCount(), PushModel::PairCount());
			}
		}

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
		// The registry scans are O(capacity) and run for EVERY new collision pair in
		// the world, on the physics thread, so they are gated on debugLog: with the
		// probe off this costs one predictable branch.
		if (Config::Get().debugLog) {
			for (std::size_t i = 0; i < 2; ++i) {
				bool isPlayer = false;
				if (BodyIsRegisteredPhantom(a_event.bodies[i], isPlayer)) {
					phantomHit = true;
					phantomPlayer = phantomPlayer || isPlayer;
				}
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

		// Superset tag only, NOT a player signal: it also fires for every NPC-vs-world
		// pair, and for player-vs-null since the player is itself an Actor. Recorded
		// so the log shows how large that bucket is, never to conclude anything.
		auto*      identified = refrA ? refrA : refrB;
		const bool oneNull = (refrA == nullptr) != (refrB == nullptr);
		const bool nullVsActor = oneNull && identified && identified->As<RE::Actor>();
		if (nullVsActor) {
			nullVsActor_.fetch_add(1, std::memory_order_relaxed);
		}

		// Form IDs only, never names: this runs on the physics thread, and
		// GetDisplayFullName() is an engine call that walks the game world. Names are
		// printed by ProbePlayerBumpRecord() on the main thread instead.
		if ((groupHit || phantomHit || nullVsActor) &&
			TryClaimLogSlot(collisionLogged_, collisionLastMs_)) {
			logger::info("world contact event: groupHit={} phantomHit={} phantomPlayer={} nullVsActor={} "
						 "filterA=0x{:08X} refrA=0x{:08X} filterB=0x{:08X} refrB=0x{:08X} playerGroup=0x{:04X}",
				groupHit, phantomHit, phantomPlayer, nullVsActor,
				filterA, refrA ? refrA->GetFormID() : 0u,
				filterB, refrB ? refrB->GetFormID() : 0u,
				static_cast<std::uint16_t>(playerGroup));
		}
	}

	bool WorldContactListener::TryClaimLogSlot(std::atomic<std::uint64_t>& a_logged,
		std::atomic<std::uint64_t>& a_lastMs)
	{
		// Bounded: at most kMaxLogs lines per stream, one per kLogWindowMs. Everything
		// above the gate is a couple of atomic loads, so the hot path costs nothing
		// once the cap is reached.
		if (a_logged.load(std::memory_order_relaxed) >= kMaxLogs) {
			return false;
		}
		const auto now = FrameClock::NowMs();
		const auto last = a_lastMs.load(std::memory_order_relaxed);
		if (last != 0 && now - last < kLogWindowMs) {
			return false;
		}
		a_lastMs.store(now, std::memory_order_relaxed);
		a_logged.fetch_add(1, std::memory_order_relaxed);
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
		if (!TryClaimLogSlot(logged_, lastLogMs_)) {
			return;
		}

		const auto*     cp = a_event.contactPoint;
		const math::Vec3 pos = cp ? ToVec3(cp->position) : math::Vec3{};
		const math::Vec3 nrm = cp ? ToVec3(cp->separatingNormal) : math::Vec3{};

		// Bounded exception to "physics-thread code touches no game world": two
		// GetDisplayFullName() calls behind a budget capped at kMaxLogs lines per
		// session, because "which actor" is the entire point of this one line. Every
		// other diagnostic on this thread prints form IDs only.
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
					 "groupProbeViable={} engineMapResolves={} (engineMapRef=0x{:X})",
			reinterpret_cast<std::uintptr_t>(collidable), filter.filter, group,
			static_cast<std::int32_t>(collidable->GetCollisionLayer()),
			group != 0,
			mapped != nullptr,
			reinterpret_cast<std::uintptr_t>(mapped));
	}

	void ProbePlayerBumpRecord()
	{
		// The deferred, name-bearing detection report runs on every active tick, even
		// when the probe's own fields are unchanged (its log is edge-triggered).
		ReportBumpDetection();

		auto* ctrl = PlayerController();
		if (!ctrl) {
			// No verified controller: nothing is bumping, so do not leave a stale
			// target behind for the physics thread to apply.
			g_bumpTarget.Publish(nullptr);
			return;
		}

		auto* body = ctrl->bumpedBody.get();
		auto* charBody = ctrl->bumpedCharCollisionObject.get();

		// Detection is level-triggered on the engine's CURRENT record, not on the
		// edge below: the physics thread must keep seeing the target while the bump
		// persists. The model's real-closing-speed gate and per-target cooldown bound
		// what that can do, and a null record clears the slot so a stale target is
		// never re-applied. Resolution and the first-sight log happen on this, the
		// main thread; nothing here is done from the callback.
		if (Config::Get().useBumpDetection) {
			PublishBumpTargetFrom(charBody);
		} else {
			g_bumpTarget.Publish(nullptr);
		}

		// Edge-triggered on the pair of pointers AND capped: the engine leaves the
		// last bump in place, so a level-triggered log would repeat one bump forever,
		// while alternating bumper pairs would log every tick without the cap. Two
		// separate atomics rather than one hash, so a hash collision cannot be
		// mistaken for the "nothing yet" state. This is a 60 Hz sample of fields the
		// physics thread writes, so a bump set and cleared within one step is missed;
		// that is inherent to sampling and not a correctness claim.
		const auto bodyKey = reinterpret_cast<std::uintptr_t>(body);
		const auto charKey = reinterpret_cast<std::uintptr_t>(charBody);
		if (g_lastBumpBody.load(std::memory_order_relaxed) == bodyKey &&
			g_lastBumpChar.load(std::memory_order_relaxed) == charKey) {
			return;
		}
		g_lastBumpBody.store(bodyKey, std::memory_order_relaxed);
		g_lastBumpChar.store(charKey, std::memory_order_relaxed);
		if (!body && !charBody) {
			return;  // cleared: not a bump
		}
		if (g_bumpLines.load(std::memory_order_relaxed) >= kBumpLogMax) {
			return;
		}
		g_bumpLines.fetch_add(1, std::memory_order_relaxed);

		// These two dereferences race the physics thread's writes to non-atomic
		// hkRefPtr fields. Reading .get() is refcount-safe, and the hkRefPtr holds a
		// strong reference so the body is not freed by refcount while held - but a
		// simultaneous release could still drop the last reference. This is an
		// accepted, narrow assumption, not a proof: names are printed here (main
		// thread) rather than in the callback for exactly that reason.
		auto* bodyRefr = body ? body->GetUserData() : nullptr;
		auto* charRefr = charBody ? charBody->GetUserData() : nullptr;
		logger::info("player bump record: bumpedBody=0x{:X} (refr=0x{:08X} '{}') "
					 "bumpedCharCollisionObject=0x{:X} (refr=0x{:08X} '{}') bumpedForce={:.2f}",
			reinterpret_cast<std::uintptr_t>(body), bodyRefr ? bodyRefr->GetFormID() : 0u, SafeName(bodyRefr),
			reinterpret_cast<std::uintptr_t>(charBody), charRefr ? charRefr->GetFormID() : 0u, SafeName(charRefr),
			ctrl->bumpedForce);
	}

	RE::hkpCharacterProxy* TakePendingBumpTarget()
	{
		// exchange(nullptr): a target is consumed at most once, and a null in the slot
		// is consumed and dropped rather than handed out as something to apply.
		RE::hkpCharacterProxy* target = nullptr;
		if (!TakeForApply(g_bumpTarget, target)) {
			return nullptr;
		}
		return target;
	}

	BumpRecordView ReadBumpRecord()
	{
		BumpRecordView view{};
		auto*           ctrl = PlayerController();
		if (!ctrl) {
			return view;
		}
		view.force = ctrl->bumpedForce;
		view.charBody = ctrl->bumpedCharCollisionObject.get();
		view.refr = view.charBody ? view.charBody->GetUserData() : nullptr;
		view.actor = view.refr ? view.refr->As<RE::Actor>() : nullptr;
		if (view.actor && view.actor != RE::PlayerCharacter::GetSingleton()) {
			view.ctrl = view.actor->GetCharController();
			if (auto* pc = AsProxyController(view.ctrl)) {
				view.target = pc->GetCharacterProxy();
			}
		}
		return view;
	}

	void NoteBumpPushApplied(RE::hkpCharacterProxy* a_target, float a_dv)
	{
		if (!a_target) {
			return;
		}
		g_bumpPushCount.fetch_add(1, std::memory_order_relaxed);

		// Latch the first application for the main-thread name log. The physics
		// thread is the only writer, so compare_exchange cannot lose a race; the
		// release store publishes target/dv before the acquire load sees the flag.
		bool expected = false;
		if (g_bumpPushLatchSet.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			g_bumpPushTarget.store(a_target, std::memory_order_relaxed);
			g_bumpPushDv.store(a_dv, std::memory_order_relaxed);
			g_bumpPushLatched.store(true, std::memory_order_release);
		}
	}

	std::uint64_t BumpTargetCount()
	{
		return g_bumpTargetCount.load(std::memory_order_relaxed);
	}

	std::uint64_t BumpPushAppliedCount()
	{
		return g_bumpPushCount.load(std::memory_order_relaxed);
	}

	void ReportBumpDetection()
	{
		// One-shot: only the FIRST applied bump push is named, and only from the main
		// thread, where GetDisplayFullName()/the registry Actor* are safe. The latch
		// stays set (the counter keeps rising), so this is a cheap atomic load per
		// tick after the single line is emitted.
		if (!g_bumpPushLatched.load(std::memory_order_acquire)) {
			return;
		}
		if (!ClaimBumpDetectLine(kBumpDetectLogMax)) {
			return;
		}
		auto* target = g_bumpPushTarget.load(std::memory_order_relaxed);
		if (!target) {
			return;
		}
		const float dv = g_bumpPushDv.load(std::memory_order_relaxed);

		ProxyEntry info{};
		const bool known = ProxyRegistry::Get().Lookup(target, info);
		auto*      actor = known ? info.actor : nullptr;
		logger::info("bump detection: push applied proxy=0x{:X} actor=0x{:08X} '{}' dv={:.2f} bumpPushes={} pairs={}",
			reinterpret_cast<std::uintptr_t>(target), actor ? actor->GetFormID() : 0u, SafeName(actor),
			dv, BumpPushAppliedCount(), PushModel::PairCount());
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
		g_lastBumpBody.store(0, std::memory_order_relaxed);
		g_lastBumpChar.store(0, std::memory_order_relaxed);

		// Bump detection spans a dead generation: clear the pending target (a proxy
		// pointer from before the load must never be applied), the distinct-target
		// set (so new targets are re-reported) and the first-application latch (so the
		// new generation's first push can still be named). The line caps are NOT reset:
		// the session's log volume stays bounded across loads.
		g_bumpTarget.Clear();
		for (std::size_t i = 0; i < g_bumpTargetsSeenCount; ++i) {
			g_bumpTargetsSeen[i] = nullptr;
		}
		g_bumpTargetsSeenCount = 0;
		g_bumpPushLatchSet.store(false, std::memory_order_relaxed);
		g_bumpPushLatched.store(false, std::memory_order_release);
		g_bumpPushTarget.store(nullptr, std::memory_order_relaxed);
		g_bumpPushDv.store(0.0f, std::memory_order_relaxed);
		// Re-arm the zero-group warning too: a transient zero observed mid-load must
		// not consume the one warning that matters for the new generation.
		g_groupWarned.store(false, std::memory_order_relaxed);
	}
}
