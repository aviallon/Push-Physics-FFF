#pragma once

#include "PhysicsMath.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace RE
{
	class hkpCharacterProxy;
}

namespace pa
{
	// SPSC ring: the physics callback pushes a deferred stagger, the main-thread
	// pump drains it (design.md 3.4). Fixed capacity, no allocation, no locks.
	class StaggerQueue
	{
	public:
		struct Entry
		{
			RE::hkpCharacterProxy* proxy = nullptr;
			float                  dv = 0.0f;
			math::Vec3             dir{};
		};

		static StaggerQueue& Get();

		// Called from the callback thread.
		bool Push(RE::hkpCharacterProxy* a_proxy, float a_dv, const math::Vec3& a_dir);

		// Called from the main thread.
		bool Pop(Entry& a_out);

		[[nodiscard]] std::uint32_t Size() const;
		void                        Clear();

	private:
		static constexpr std::uint32_t kCapacity = 64;
		std::array<Entry, kCapacity>   entries_{};
		std::atomic<std::uint32_t>     head_{ 0 };
		std::atomic<std::uint32_t>     tail_{ 0 };
	};
}
