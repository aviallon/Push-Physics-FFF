#include "PCH.h"

#include "StaggerQueue.h"

namespace pa
{
	StaggerQueue& StaggerQueue::Get()
	{
		static StaggerQueue queue;
		return queue;
	}

	bool StaggerQueue::Push(RE::hkpCharacterProxy* a_proxy, float a_dv, const math::Vec3& a_dir)
	{
		const auto head = head_.load(std::memory_order_relaxed);
		const auto next = (head + 1) % kCapacity;
		if (next == tail_.load(std::memory_order_acquire)) {
			return false;  // full: drop rather than block the physics thread
		}
		entries_[head] = Entry{ a_proxy, a_dv, a_dir };
		head_.store(next, std::memory_order_release);
		return true;
	}

	bool StaggerQueue::Pop(Entry& a_out)
	{
		const auto tail = tail_.load(std::memory_order_relaxed);
		if (tail == head_.load(std::memory_order_acquire)) {
			return false;
		}
		a_out = entries_[tail];
		tail_.store((tail + 1) % kCapacity, std::memory_order_release);
		return true;
	}

	std::uint32_t StaggerQueue::Size() const
	{
		const auto head = head_.load(std::memory_order_acquire);
		const auto tail = tail_.load(std::memory_order_acquire);
		return (head + kCapacity - tail) % kCapacity;
	}

	void StaggerQueue::Clear()
	{
		tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
	}
}
