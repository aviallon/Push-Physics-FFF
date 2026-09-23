#pragma once

#include <cstdint>

namespace pa
{
	// Monotonic clock for the push model. NowMs() is process-lifetime
	// milliseconds; CurrentFrame() is the once-per-frame key used by the
	// appliedFrame guard. Frame identity is the millisecond at the model's
	// resolution (Skyrim's Havok step is ~16 ms), which is enough to stop a
	// multi-point manifold from applying the same buffer several times while
	// still renewing the push on the next step.
	class FrameClock
	{
	public:
		[[nodiscard]] static std::uint64_t NowMs();
		[[nodiscard]] static std::uint32_t CurrentFrame();
	};
}
