#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class Actor;
	class bhkCharProxyController;
	class hkpCharacterProxy;
	class hkpCollidable;
}

namespace pa
{
	// Per-proxy snapshot, published by the main thread and read (never mutated)
	// by the physics callback. The Actor* is stored so the main thread can use it
	// later, but the callback must never dereference it: only mass and flags are
	// read off-thread (design.md 3.4).
	enum ProxyStateFlags : std::uint32_t
	{
		kProxyKnown = 1u << 0,
		kProxyRagdoll = 1u << 1,
		kProxyDead = 1u << 2,
		kProxyInCombat = 1u << 3,
		kProxySwimming = 1u << 4,
		kProxyAirborne = 1u << 5,
		kProxyKillMove = 1u << 6,
		kProxyBleedout = 1u << 7,
		kProxyNotPushable = 1u << 8,
		kProxyNoCharacterCollisions = 1u << 9,
		kProxyStaggering = 1u << 10,
		kProxyPlayer = 1u << 11,
	};

	struct ProxyEntry
	{
		RE::hkpCharacterProxy*     proxy = nullptr;
		RE::bhkCharProxyController* controller = nullptr;
		RE::Actor*                 actor = nullptr;
		const RE::hkpCollidable*   collidable = nullptr;
		float                      mass = 0.0f;
		std::uint32_t              flags = 0;
	};

	// Main-thread map proxy -> {Actor*, mass, flags}, rebuilt on load and every
	// fRegistryRefreshSec, published behind a seqlock counter so a reader on the
	// physics thread sees a whole entry or retries.
	class ProxyRegistry
	{
	public:
		static ProxyRegistry& Get();

		void Init(std::uint32_t a_capacity);   // main thread, before any reader
		void Invalidate();                     // main thread, kPreLoadGame
		void RequestRebuild();                 // main thread, save/load message; dirty flag only
		void RebuildNow();                     // main thread
		// Main-thread tick, driven once per frame by the verified frame-tail detour
		// (src/Hooks/FrameTickHook.cpp): refresh the registry, drive
		// attach/re-attach and the model's main-thread half, emit the debug
		// heartbeat. No-op on the world until a game is loaded, RE::Main reports
		// gameActive and any pending rebuild has settled.
		void MainThreadTick();

		// Reader, any thread.
		[[nodiscard]] bool Lookup(const RE::hkpCharacterProxy* a_proxy, ProxyEntry& a_out) const;
		[[nodiscard]] bool ProxyForCollidable(const RE::hkpCollidable* a_collidable, RE::hkpCharacterProxy*& a_proxyOut) const;

		// Main thread only (the single writer): iterate the published entries. No
		// seqlock is needed because the caller is the writer. Used by the `registry`
		// command, which is allowed to resolve names.
		template <class F>
		void ForEachEntry(F&& a_fn) const
		{
			const auto size = size_.load(std::memory_order_relaxed);
			for (std::uint32_t i = 0; i < size && i < capacity_; ++i) {
				a_fn(entries_[i]);
			}
		}

		// Main-thread state used by the callback without touching the game world.
		void SetDialogueOpen(bool a_open) { dialogueOpen_.store(a_open, std::memory_order_relaxed); }
		[[nodiscard]] bool DialogueOpen() const { return dialogueOpen_.load(std::memory_order_relaxed); }

		[[nodiscard]] std::uint64_t Generation() const { return generation_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint32_t Size() const { return size_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint32_t Capacity() const { return capacity_; }

	private:
		ProxyRegistry() = default;

		void RebuildLocked();

		mutable std::atomic<std::uint32_t> seq_{ 0 };
		std::vector<ProxyEntry>            entries_;
		std::atomic<std::uint32_t>         size_{ 0 };
		std::uint32_t                      capacity_ = 0;
		std::atomic<bool>                  dialogueOpen_{ false };
		std::atomic<std::uint64_t>         generation_{ 0 };
		// Set by RequestRebuild() from the save/load message handler and consumed
		// by MainThreadTick() once the world is stable. pendingRebuildFrames_ is
		// only touched on the main thread.
		std::atomic<bool> rebuildRequested_{ false };
		std::uint32_t     pendingRebuildFrames_ = 0;
	};
}
