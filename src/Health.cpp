#include "PCH.h"

#include "Health.h"

namespace pa
{
	Health& Health::Get()
	{
		static Health health;
		return health;
	}

	void Health::Off(const char* a_reason)
	{
		off_.store(true, std::memory_order_relaxed);
		std::scoped_lock lock(mutex_);
		reason_ = a_reason ? a_reason : "";
	}

	void Health::Degrade(const char* a_reason)
	{
		degraded_.store(true, std::memory_order_relaxed);
		std::scoped_lock lock(mutex_);
		if (reason_.empty()) {
			reason_ = a_reason ? a_reason : "";
		}
	}

	void Health::Clear()
	{
		off_.store(false, std::memory_order_relaxed);
		degraded_.store(false, std::memory_order_relaxed);
		std::scoped_lock lock(mutex_);
		reason_.clear();
	}

	std::string Health::Line() const
	{
		std::scoped_lock lock(mutex_);
		if (off_.load(std::memory_order_relaxed)) {
			return "OFF (" + reason_ + ")";
		}
		if (degraded_.load(std::memory_order_relaxed)) {
			return "DEGRADED (" + reason_ + ")";
		}
		return "OK";
	}
}
