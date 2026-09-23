#include "PCH.h"

#include "PushRegistry.h"

namespace pa
{
	PushRegistry& PushRegistry::Get()
	{
		static PushRegistry registry;
		return registry;
	}

	void PushRegistry::Init(std::uint32_t a_capacity)
	{
		capacity_ = a_capacity > 0 ? a_capacity : 512;
		slots_.assign(capacity_, PushEntry{});
		size_.store(0, std::memory_order_relaxed);
		logger::info("push registry: capacity {}", capacity_);
	}

	void PushRegistry::Clear()
	{
		std::scoped_lock guard(lock_);
		for (auto& slot : slots_) {
			slot = PushEntry{};
		}
		size_.store(0, std::memory_order_relaxed);
	}

	void PushRegistry::Sweep(std::uint64_t a_nowMs, float a_damping, float a_deltaSec)
	{
		std::scoped_lock guard(lock_);
		std::size_t      removed = 0;
		for (auto& slot : slots_) {
			if (!slot.active) {
				continue;
			}
			if (math::IsExpired(slot.state, a_nowMs)) {
				slot = PushEntry{};
				++removed;
				continue;
			}
			slot.state.velocity = math::DampVelocity(slot.state.velocity, a_damping, a_deltaSec);
		}
		if (removed > 0) {
			size_.fetch_sub(removed, std::memory_order_relaxed);
		}
	}

	void PushRegistry::Scale(RE::hkpCharacterProxy* a_proxy, float a_factor)
	{
		if (!a_proxy || capacity_ == 0) {
			return;
		}
		std::scoped_lock guard(lock_);
		std::size_t      index = Hash(a_proxy);
		for (std::uint32_t probe = 0; probe < capacity_; ++probe) {
			auto& slot = slots_[index];
			if (!slot.active) {
				return;
			}
			if (slot.proxy == a_proxy) {
				slot.state.velocity = math::Scale(slot.state.velocity, a_factor);
				return;
			}
			index = (index + 1) % capacity_;
		}
	}

	std::size_t PushRegistry::Size() const
	{
		return size_.load(std::memory_order_relaxed);
	}
}
