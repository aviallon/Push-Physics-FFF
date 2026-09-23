#include "PCH.h"

#include "ProxyRegistry.h"

#include "Config.h"
#include "FrameClock.h"
#include "ProxyAccess.h"
#include "PushManager.h"
#include "PushModel.h"

#include <RE/A/Actor.h>
#include <RE/A/ActorState.h>
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

		[[nodiscard]] std::uint32_t ControllerFlags(RE::bhkCharProxyController* a_ctrl)
		{
			return a_ctrl ? a_ctrl->flags.underlying() : 0u;
		}

		[[nodiscard]] float MassForProxy(RE::hkpCharacterProxy* a_proxy, RE::Actor* a_actor, bool a_isPlayer)
		{
			const auto& cfg = Config::Get();
			if (a_isPlayer) {
				return cfg.playerMass;
			}

			float mass = a_proxy ? a_proxy->characterMass : 0.0f;
			if (!(mass > 0.0f) && a_actor) {
				if (auto* race = a_actor->GetRace()) {
					const float scale = a_actor->GetScale();
					mass = race->data.baseMass * scale * scale * scale;
				}
			}
			if (!(mass > 0.0f)) {
				mass = cfg.defaultCharacterMass;
			}
			return mass;
		}

		[[nodiscard]] std::uint32_t StateFlags(RE::Actor* a_actor, RE::bhkCharProxyController* a_ctrl, bool a_isPlayer)
		{
			std::uint32_t flags = kProxyKnown;
			if (a_isPlayer) {
				flags |= kProxyPlayer;
			}
			if (a_actor) {
				if (a_actor->IsInRagdollState()) {
					flags |= kProxyRagdoll;
				}
				if (a_actor->IsDead()) {
					flags |= kProxyDead;
				}
				if (a_actor->IsInCombat()) {
					flags |= kProxyInCombat;
				}
				if (a_actor->IsInKillMove()) {
					flags |= kProxyKillMove;
				}
				if (a_actor->IsInBleedout()) {
					flags |= kProxyBleedout;
				}
				if (a_actor->IsStaggering()) {
					flags |= kProxyStaggering;
				}
				if (auto* state = a_actor->AsActorState()) {
					if (state->IsSwimming()) {
						flags |= kProxySwimming;
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
		logger::info("proxy registry invalidated");
	}

	void ProxyRegistry::RebuildLocked()
	{
		std::uint32_t count = 0;
		const auto    playerProxy = PlayerProxy();

		if (playerProxy) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* pc = AsProxyController(player ? player->GetCharController() : nullptr);
			if (count < capacity_) {
				FillEntry(entries_[count++], player, pc, true);
			}
		}

		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			lists->ForAllActors([&](RE::Actor* a_actor) -> RE::BSContainer::ForEachResult {
				if (a_actor == RE::PlayerCharacter::GetSingleton()) {
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

	void ProxyRegistry::StartMainThreadPump()
	{
		auto task = std::make_shared<std::function<void()>>();
		*task = [task]() {
			if (!SKSE::GetTaskInterface()) {
				return;
			}

			const auto now = FrameClock::NowMs();
			const auto dtMs = g_lastPumpMs == 0 ? 0 : now - g_lastPumpMs;
			g_lastPumpMs = now;

			auto& registry = ProxyRegistry::Get();
			const auto refreshMs = static_cast<std::uint64_t>(
				std::max(0.0f, Config::Get().registryRefreshSec) * 1000.0f);
			if (g_lastRebuildMs == 0 || now - g_lastRebuildMs >= refreshMs) {
				g_lastRebuildMs = now;
				registry.RebuildNow();
			}

			if (auto* ui = RE::UI::GetSingleton()) {
				registry.SetDialogueOpen(ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME));
			}

			// Re-attach if the player's controller was rebuilt; drain deferred
			// staggers; sweep/debug-damp the push buffer.
			PushManagerMainThreadTick();
			PushModel::TickMainThread(static_cast<float>(dtMs) / 1000.0f);

			SKSE::GetTaskInterface()->AddTask(*task);
		};

		SKSE::GetTaskInterface()->AddTask(*task);
		logger::info("main-thread pump started");
	}
}
