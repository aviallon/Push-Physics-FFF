#pragma once

#include "PhysicsMath.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace RE
{
	class hkpCharacterProxy;
}

namespace pa
{
	// One pending push per target proxy. The buffer state is the pure
	// math::BufferState, so the cooldown / take-max / once-per-frame / expiry
	// rules are the tested ones, not a second copy.
	struct PushEntry
	{
		RE::hkpCharacterProxy* proxy = nullptr;
		bool                   active = false;
		math::BufferState      state{};
	};

	// Fixed-capacity open-addressed table, written from the physics callback and
	// swept from the main thread. A tiny spinlock keeps the two from tearing a
	// slot; critical sections are a handful of float ops.
	class PushRegistry
	{
	public:
		static PushRegistry& Get();

		void Init(std::uint32_t a_capacity);
		void Clear();
		void Sweep(std::uint64_t a_nowMs, float a_damping, float a_deltaSec);
		void Scale(RE::hkpCharacterProxy* a_proxy, float a_factor);

		// Find or create the entry for a_proxy, then run fn(PushEntry&) under the
		// lock. Returns false when the table is full (the push is dropped, never
		// applied half-way).
		template <class F>
		bool WithEntry(RE::hkpCharacterProxy* a_proxy, F&& a_fn);

		[[nodiscard]] std::size_t Size() const;

	private:
		PushRegistry() = default;

		[[nodiscard]] std::size_t Hash(RE::hkpCharacterProxy* a_proxy) const
		{
			return (reinterpret_cast<std::uintptr_t>(a_proxy) >> 4) % (capacity_ ? capacity_ : 1);
		}

		class SpinLock
		{
		public:
			void lock() { while (flag_.test_and_set(std::memory_order_acquire)) {} }
			void unlock() { flag_.clear(std::memory_order_release); }

		private:
			std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
		};

		mutable SpinLock                 lock_;
		std::vector<PushEntry>           slots_;
		std::uint32_t                    capacity_ = 0;
		std::atomic<std::size_t>         size_{ 0 };
	};

	template <class F>
	bool PushRegistry::WithEntry(RE::hkpCharacterProxy* a_proxy, F&& a_fn)
	{
		if (!a_proxy || capacity_ == 0) {
			return false;
		}
		std::scoped_lock guard(lock_);

		std::size_t index = Hash(a_proxy);
		for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
			auto& slot = slots_[index];
			if (slot.active) {
				if (slot.proxy == a_proxy) {
					a_fn(slot);
					return true;
				}
			} else {
				slot.proxy = a_proxy;
				slot.active = true;
				slot.state = math::BufferState{};
				size_.fetch_add(1, std::memory_order_relaxed);
				a_fn(slot);
				return true;
			}
			index = (index + 1) % capacity_;
		}
		return false;  // full
	}
}
