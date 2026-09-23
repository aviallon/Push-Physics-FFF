#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace pa
{
	// The plugin's honest verdict, mirroring HeapSentinel: `DEGRADED` unless the
	// one thing Path 1 depends on was observed to work. Silence must never look
	// like success (design.md gate 20).
	class Health
	{
	public:
		static Health& Get();

		void Off(const char* a_reason);
		void Degrade(const char* a_reason);
		void Clear();

		[[nodiscard]] bool IsOff() const { return off_.load(std::memory_order_relaxed); }
		[[nodiscard]] bool IsDegraded() const { return degraded_.load(std::memory_order_relaxed); }

		[[nodiscard]] std::string Line() const;

	private:
		std::atomic<bool> off_{ false };
		std::atomic<bool> degraded_{ false };
		mutable std::mutex mutex_;
		std::string        reason_;
	};
}
