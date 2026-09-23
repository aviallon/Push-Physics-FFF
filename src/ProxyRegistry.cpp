#include "PCH.h"

#include "ProxyRegistry.h"

#include "Config.h"
#include "CommandChannel.h"
#include "FrameClock.h"
#include "ProxyAccess.h"
#include "PushListener.h"
#include "PushManager.h"
#include "PushModel.h"
#include "TraceChannel.h"
#include "WorldContactListener.h"

#include <RE/A/Actor.h>
#include <RE/A/ActorState.h>
#include <RE/M/Main.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESRace.h>
#include <RE/U/UI.h>
#include <RE/D/DialogueMenu.h>
#include <RE/H/hkpShapePhantom.h>

#include <cmath>

namespace pa
{
	namespace
	{
		std::uint64_t g_lastRebuildMs = 0;
		std::uint64_t g_lastPumpMs = 0;
		std::uint64_t g_lastStatsMs = 0;

		// Frames to let the world settle after a save/load message before the
		// registry touches it. kPostLoadGame/kNewGame are delivered while
		// ProcessLists and the character controllers are still being rebuilt;
		// iterating them then is what let a half-constructed actor reach the
		// (null) IsInBleedout relocation. ~0.5 s at 60 fps, plus the gameActive
		// gate below, is a cheap insurance against acting too early.
		constexpr std::uint32_t kRebuildSettleFrames = 30;

		// RE::Main::GetSingleton() dereferences its REL::Relocation<Main**> without
		// checking it. Resolve and check the relocation ourselves, so an unresolved
		// id can never turn the per-frame tick into a read at address 0. This is the
		// per-frame path, where a silent unresolved relocation is not acceptable.
		[[nodiscard]] bool GameActive()
		{
			static REL::Relocation<RE::Main**> singleton{ REL::RelocationID(516943, 403449) };
			if (singleton.address() == 0) {
				return false;
			}
			auto* main = *singleton;
			return main != nullptr && main->GetRuntimeData().gameActive;
		}

		[[nodiscard]] std::uint32_t ControllerFlags(RE::bhkCharProxyController* a_ctrl)
		{
			return a_ctrl ? a_ctrl->flags.underlying() : 0u;
		}

		// A cheap sanity check before any field read. It cannot prove an Actor*
		// is genuinely an actor, but it rejects null and misaligned pointers,
		// and - crucially - the reads that follow are all INLINE field reads, so
		// there is no relocation-backed call a wild pointer could be routed to.
		[[nodiscard]] bool PlausibleActor(const RE::Actor* a_actor)
		{
			const auto value = reinterpret_cast<std::uintptr_t>(a_actor);
			return value >= 0x10000 && (value & 0x7) == 0;
		}

		// TESRace::data.baseMass is a RELATIVE multiplier (~1.0 for humanoids), NOT
		// kilograms, so it must be scaled by the default humanoid mass before it can
		// stand in for a mass. Main-thread only (reached from FillEntry).
		void NoteHeavyRaceOnce(const RE::TESRace* a_race, float a_scaledMass)
		{
			if (!a_race || !(a_race->data.baseMass > 1.5f)) {
				return;  // ordinary humanoid-scale race: nothing to report
			}
			constexpr std::size_t kHeavyRaceSlots = 32;
			static std::uint32_t  seen[kHeavyRaceSlots]{};
			static std::size_t    seenCount = 0;
			const auto            formID = a_race->formID;
			for (std::size_t i = 0; i < seenCount; ++i) {
				if (seen[i] == formID) {
					return;
				}
			}
			if (seenCount < kHeavyRaceSlots) {
				seen[seenCount++] = formID;
			}
			// One line per heavy race ever registered. Reports the raw relative
			// multiplier so a giant/dragon can be identified without building any
			// escalation on top of it.
			logger::info("target with heavy race registered: race=0x{:08X} baseMass={:.2f} "
						 "(relative multiplier, not kg; fDefaultCharacterMass={:.0f}) -> mass={:.1f}",
				formID, a_race->data.baseMass, Config::Get().defaultCharacterMass, a_scaledMass);
		}

		// Mass from the Havok proxy when the engine set one, else from the race's
		// RELATIVE base mass scaled by the default humanoid mass. There is
		// deliberately NO scale correction: the only source was
		// TESObjectREFR::GetScale(), a REL::Relocation-backed call (AE id 19664),
		// and a slightly wrong fallback mass is preferable to an unresolvable call
		// on a bad actor.
		[[nodiscard]] float MassForProxy(RE::hkpCharacterProxy* a_proxy, RE::Actor* a_actor, bool a_isPlayer)
		{
			const auto& cfg = Config::Get();
			if (a_isPlayer) {
				return cfg.playerMass;
			}

			float mass = a_proxy ? a_proxy->characterMass : 0.0f;
			if (!(mass > 0.0f) && PlausibleActor(a_actor)) {
				// ActorRuntimeData::race is a plain inline field; reading it does
				// not go through Actor::GetRace()'s GetBaseObject() path.
				if (auto* race = a_actor->GetActorRuntimeData().race) {
					mass = race->data.baseMass * cfg.defaultCharacterMass;
					NoteHeavyRaceOnce(race, mass);
				}
			}
			if (!(mass > 0.0f)) {
				mass = cfg.defaultCharacterMass;
			}
			return mass;
		}

		// Every flag below comes from an inline field read on the Actor.
		//
		// This path used to call Actor::IsInRagdollState(), IsDead(),
		// IsInCombat(), IsInBleedout(), IsStaggering() and
		// ActorState::IsSwimming(). Two of those are the bug (2026-09-23):
		//
		//   * Actor::IsInBleedout() is RELOCATION_ID(48461, 0). The AE id is 0,
		//     so on 1.7.104 CommonLibSSE-NG's resolver returns a null address and
		//     the call jumps to 0 (the crash, PushAside+0x2D2C9). 48461 is the
		//     SE-only id and is absent from the AE Address Library's naming.
		//   * Actor::IsInRagdollState() is RELOCATION_ID(36492, 37491), a
		//     relocation-backed out-of-line call that also internally calls the
		//     virtual IsDead(false).
		//
		// The inline replacements are GetLifeState() (kDead / IsBleedingOut()),
		// boolFlags (kIsInKillMove) and ActorState's IsSwimming()/IsStaggered().
		// kProxyRagdoll and kProxyInCombat are DROPPED: the only engine predicates
		// for them are the relocation/vtable-backed IsInRagdollState() and
		// IsInCombat(), and no inline equivalent is trustworthy. A gate that never
		// fires is better than a call that can jump to 0.
		[[nodiscard]] std::uint32_t StateFlags(RE::Actor* a_actor, RE::bhkCharProxyController* a_ctrl, bool a_isPlayer)
		{
			std::uint32_t flags = kProxyKnown;
			if (a_isPlayer) {
				flags |= kProxyPlayer;
			}
			if (PlausibleActor(a_actor)) {
				if (a_actor->IsInKillMove()) {
					flags |= kProxyKillMove;
				}
				if (auto* state = a_actor->AsActorState()) {
					if (state->GetLifeState() == RE::ACTOR_LIFE_STATE::kDead) {
						flags |= kProxyDead;
					}
					if (state->IsBleedingOut()) {
						flags |= kProxyBleedout;
					}
					if (state->IsSwimming()) {
						flags |= kProxySwimming;
					}
					if (state->IsStaggered()) {
						flags |= kProxyStaggering;
					}
				}
			}

			const auto cflags = ControllerFlags(a_ctrl);
			if (cflags & static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNotPushable)) {
				flags |= kProxyNotPushable;
			}
			if (cflags & static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNotPushablePermanent)) {
				flags |= kProxyNotPushable;
			}
			if (cflags & static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kNoCharacterCollisions)) {
				flags |= kProxyNoCharacterCollisions;
			}
			const auto supportMask = static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kSupport) |
				static_cast<std::uint32_t>(RE::CHARACTER_FLAGS::kHasPotentialSupportManifold);
			if ((cflags & supportMask) == 0) {
				flags |= kProxyAirborne;
			}
			return flags;
		}

		void FillEntry(ProxyEntry& a_entry, RE::Actor* a_actor, RE::bhkCharProxyController* a_ctrl, bool a_isPlayer)
		{
			auto* proxy = a_ctrl ? a_ctrl->GetCharacterProxy() : nullptr;
			a_entry.proxy = proxy;
			a_entry.controller = a_ctrl;
			a_entry.actor = a_actor;
			a_entry.collidable = (proxy && proxy->shapePhantom) ? proxy->shapePhantom->GetCollidable() : nullptr;
			a_entry.mass = MassForProxy(proxy, a_actor, a_isPlayer);
			a_entry.flags = StateFlags(a_actor, a_ctrl, a_isPlayer);
		}
	}

	ProxyRegistry& ProxyRegistry::Get()
	{
		static ProxyRegistry registry;
		return registry;
	}

	void ProxyRegistry::Init(std::uint32_t a_capacity)
	{
		capacity_ = a_capacity > 0 ? a_capacity : 512;
		entries_.assign(capacity_, ProxyEntry{});
		size_.store(0, std::memory_order_relaxed);
		seq_.store(0, std::memory_order_relaxed);
		logger::info("proxy registry: capacity {}", capacity_);
	}

	void ProxyRegistry::Invalidate()
	{
		seq_.fetch_add(1, std::memory_order_acq_rel);  // odd: writing
		size_.store(0, std::memory_order_relaxed);
		seq_.fetch_add(1, std::memory_order_acq_rel);  // even: stable
		generation_.fetch_add(1, std::memory_order_acq_rel);
		rebuildRequested_.store(false, std::memory_order_relaxed);
		pendingRebuildFrames_ = 0;
		logger::info("proxy registry invalidated");
	}

	void ProxyRegistry::RequestRebuild()
	{
		// Called from the SKSE message handler at kDataLoaded / kPostLoadGame /
		// kNewGame. It must not iterate the world: only mark it dirty and let
		// MainThreadTick do the work once PlayerCharacter exists, RE::Main says
		// the game is active, and kRebuildSettleFrames have elapsed.
		pendingRebuildFrames_ = kRebuildSettleFrames;
		rebuildRequested_.store(true, std::memory_order_release);
		logger::info("proxy registry rebuild requested; deferred {} frames past gameActive", kRebuildSettleFrames);
	}

	void ProxyRegistry::RebuildLocked()
	{
		std::uint32_t count = 0;
		const auto    playerProxy = PlayerProxy();

		if (playerProxy) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (PlausibleActor(player)) {
				auto* pc = AsProxyController(player->GetCharController());
				if (count < capacity_) {
					FillEntry(entries_[count++], player, pc, true);
				}
			}
		}

		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			lists->ForAllActors([&](RE::Actor* a_actor) -> RE::BSContainer::ForEachResult {
				if (!PlausibleActor(a_actor) || a_actor == RE::PlayerCharacter::GetSingleton()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* ctrl = a_actor->GetCharController();
				auto* pc = AsProxyController(ctrl);
				auto* proxy = pc ? pc->GetCharacterProxy() : nullptr;
				if (!proxy || proxy == playerProxy) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				if (count >= capacity_) {
					return RE::BSContainer::ForEachResult::kStop;
				}
				FillEntry(entries_[count++], a_actor, pc, false);
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		size_.store(count, std::memory_order_relaxed);
	}

	void ProxyRegistry::RebuildNow()
	{
		// Single writer (main thread): odd while writing, even when stable.
		seq_.fetch_add(1, std::memory_order_acq_rel);
		RebuildLocked();
		seq_.fetch_add(1, std::memory_order_acq_rel);
		generation_.fetch_add(1, std::memory_order_acq_rel);
	}

	bool ProxyRegistry::Lookup(const RE::hkpCharacterProxy* a_proxy, ProxyEntry& a_out) const
	{
		if (!a_proxy) {
			return false;
		}
		for (;;) {
			const auto s1 = seq_.load(std::memory_order_acquire);
			if (s1 & 1u) {
				continue;  // writer in progress
			}
			const auto size = size_.load(std::memory_order_relaxed);
			bool       found = false;
			for (std::uint32_t i = 0; i < size && i < capacity_; ++i) {
				if (entries_[i].proxy == a_proxy) {
					a_out = entries_[i];
					found = true;
					break;
				}
			}
			const auto s2 = seq_.load(std::memory_order_acquire);
			if (s1 == s2) {
				return found;
			}
		}
	}

	bool ProxyRegistry::ProxyForCollidable(const RE::hkpCollidable* a_collidable, RE::hkpCharacterProxy*& a_proxyOut) const
	{
		if (!a_collidable) {
			return false;
		}
		for (;;) {
			const auto s1 = seq_.load(std::memory_order_acquire);
			if (s1 & 1u) {
				continue;
			}
			const auto size = size_.load(std::memory_order_relaxed);
			bool       found = false;
			for (std::uint32_t i = 0; i < size && i < capacity_; ++i) {
				if (entries_[i].collidable == a_collidable) {
					a_proxyOut = entries_[i].proxy;
					found = true;
					break;
				}
			}
			const auto s2 = seq_.load(std::memory_order_acquire);
			if (s1 == s2) {
				return found;
			}
		}
	}

	void ProxyRegistry::MainThreadTick()
	{
		// Instrumentation side channel (main thread only). It runs even before a
		// game is loaded so `help`, `status` and `trace on` work at the main menu;
		// the world-reading commands guard their own nulls, and TraceChannel only
		// writes a row once a player proxy exists. Both are inert when
		// PushAside.cmd / PushAside.trace are absent or off.
		CommandChannel::Tick();
		TraceChannel::Tick();

		const auto now = FrameClock::NowMs();
		const auto dtMs = g_lastPumpMs == 0 ? 0 : now - g_lastPumpMs;
		g_lastPumpMs = now;

		// Nothing below may touch the world before a save is loaded and the game
		// reports itself active: ProcessLists exists at the main menu but has no
		// live actors, the player's proxy does not exist yet, and a deferred
		// rebuild has not settled. The tick still runs every frame (it is also
		// the heartbeat); it just does no world work until the world is stable.
		auto*      player = RE::PlayerCharacter::GetSingleton();
		const bool gameActive = player != nullptr && GameActive();

		if (gameActive) {
			auto&      registry = *this;
			bool       rebuild = false;

			// A save/load message only set the dirty flag. Wait out the settle
			// frames, then force one rebuild and clear the request. While the
			// request is pending the periodic refresh must NOT fire, or the first
			// active frame would iterate the world the delay was meant to protect.
			const bool pending = rebuildRequested_.load(std::memory_order_acquire);
			if (pending) {
				if (pendingRebuildFrames_ > 0) {
					--pendingRebuildFrames_;
				} else if (rebuildRequested_.exchange(false, std::memory_order_acq_rel)) {
					rebuild = true;
				}
			}

			const auto refreshMs = static_cast<std::uint64_t>(
				std::max(0.0f, Config::Get().registryRefreshSec) * 1000.0f);
			if (!pending && !rebuild && (g_lastRebuildMs == 0 || now - g_lastRebuildMs >= refreshMs)) {
				rebuild = true;
			}

			if (rebuild) {
				g_lastRebuildMs = now;
				registry.RebuildNow();
			}

			if (auto* ui = RE::UI::GetSingleton()) {
				registry.SetDialogueOpen(ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME));
			}

			// Re-attach if the player's controller was rebuilt; drain deferred
			// staggers; sweep/debug-damp the push buffer. PushManagerMainThreadTick
			// performs the initial attach once a player proxy exists, so the message
			// handler never has to touch the world.
			PushManagerMainThreadTick();
			PushModel::TickMainThread(static_cast<float>(dtMs) / 1000.0f);
			EnsurePlayerBodyIdentity();
			ProbePlayerBumpRecord();
			EnsureWorldContactRegistration();
		}

		// Instrumentation (design §7 step 3). Whether Havok dispatches the
		// character-interaction virtual is the one thing that cannot be verified
		// off-game, so the listener counters must be *observable* in the log, not
		// merely incremented. Gated on bDebugLog: a shipped install should not
		// write a stats line forever, and the first-run instrumentation profile
		// turns it on. This fires at the main menu too, so "0" cannot be mistaken
		// for "the hook is not running".
		if (Config::Get().debugLog) {
			const auto statsMs = static_cast<std::uint64_t>(
				std::max(1.0f, Config::Get().calibrationLogAfterSec) * 1000.0f);
			if (g_lastStatsMs == 0 || now - g_lastStatsMs >= statsMs) {
				g_lastStatsMs = now;
				auto* listener = GetPushListener();
				const auto contact = GetWorldContactListener().Snapshot();
				logger::info("listener stats: character={} object={} constraints={} orphans={} pairs={} "
							 "bumpTargets={} bumpPushes={} "
							 "contactAdded={} pcActor={} pcObject={} actorActor={} contactOther={} "
							 "bodyPhantom={} bodyPhantomPlayer={} pcGroup={} nullVsActor={}",
					listener->CharacterCalls(), listener->ObjectCalls(),
					listener->ConstraintCalls(), OrphanCallCount(), PushModel::PairCount(),
					BumpTargetCount(), BumpPushAppliedCount(),
					contact.collisionAdded, contact.pcActor, contact.pcObject, contact.actorActor, contact.other,
					contact.bodyPhantom, contact.bodyPhantomPlayer, contact.pcGroup, contact.nullVsActor);
			}
		}
	}
}
