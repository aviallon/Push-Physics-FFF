#include "PCH.h"

#include "FrameClock.h"

namespace pa
{
	namespace
	{
		std::uint64_t NowMsImpl()
		{
			using clock = std::chrono::steady_clock;
			static const auto start = clock::now();
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count());
		}
	}

	std::uint64_t FrameClock::NowMs()
	{
		return NowMsImpl();
	}

	std::uint32_t FrameClock::CurrentFrame()
	{
		return static_cast<std::uint32_t>(NowMsImpl());
	}
}
