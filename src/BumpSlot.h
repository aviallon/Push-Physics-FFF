#pragma once

#include <atomic>

// Pure C++ (no RE/Havok/Windows dependency) so the single-slot handoff's
// semantics are asserted by the off-game tests as well as used in-game. The
// caller passes the live pointer type; the tests pass an int*.
namespace pa
{
	// One-slot latest-wins handoff from a producer (main thread) to a consumer
	// (physics thread). Publish() overwrites whatever was pending, Take() is a
	// single exchange(nullptr) so a value is consumed exactly once, and a null
	// publish clears the slot. A forgotten value is therefore *replaced*, not
	// queued, and can never be consumed twice.
	template <class T>
	class SingleSlot
	{
	public:
		void Publish(T* a_value) { slot_.store(a_value, std::memory_order_release); }

		// Consume the pending value. Always leaves the slot empty; returns nullptr
		// when nothing was pending, which the caller must treat as "do nothing".
		[[nodiscard]] T* Take() { return slot_.exchange(nullptr, std::memory_order_acq_rel); }

		[[nodiscard]] T* Peek() const { return slot_.load(std::memory_order_acquire); }

		void Clear() { slot_.store(nullptr, std::memory_order_release); }

	private:
		std::atomic<T*> slot_{ nullptr };
	};

	// The canonical consume gate. Returns true only when the slot held a non-null
	// value, and hands that value out. A null in the slot is consumed and dropped
	// here, so a consumer using this helper can never apply a null or a value
	// twice.
	template <class T>
	[[nodiscard]] bool TakeForApply(SingleSlot<T>& a_slot, T*& a_out)
	{
		a_out = a_slot.Take();
		return a_out != nullptr;
	}
}