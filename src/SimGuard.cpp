#include "SimGuard.h"

#include <atomic>
#include <cmath>

namespace pa
{
	StallUpdate StallWatch::Sample(bool a_active, std::uint64_t a_counter, std::uint64_t a_nowMs)
	{
		StallUpdate update{};

		if (!a_active) {
			// No game running: nothing to watch, and no stall may be claimed.
			// The baseline is re-armed so the threshold is measured from the
			// moment the game becomes active again.
			state_ = StallState::kUnknown;
			lastCounter_ = a_counter;
			lastAdvanceMs_ = a_nowMs;
			return update;
		}

		if (state_ == StallState::kUnknown) {
			// First active sample: establish the baseline, never a transition.
			state_ = StallState::kHealthy;
			lastCounter_ = a_counter;
			lastAdvanceMs_ = a_nowMs;
			return update;
		}

		if (a_counter != lastCounter_) {
			lastCounter_ = a_counter;
			const auto stalledFor = state_ == StallState::kStalled ? a_nowMs - lastAdvanceMs_ : 0;
			lastAdvanceMs_ = a_nowMs;
			if (state_ == StallState::kStalled) {
				state_ = StallState::kHealthy;
				update.recovered = true;
				update.stalledForMs = stalledFor;
			}
			return update;
		}

		if (state_ != StallState::kStalled && a_nowMs - lastAdvanceMs_ > thresholdMs_) {
			state_ = StallState::kStalled;
			update.enteredStall = true;
			update.stalledForMs = a_nowMs - lastAdvanceMs_;
		}
		return update;
	}

	void StallWatch::Reset()
	{
		state_ = StallState::kUnknown;
		lastCounter_ = 0;
		lastAdvanceMs_ = 0;
	}

	namespace
	{
		StallWatch& GlobalWatch()
		{
			static StallWatch watch;
			return watch;
		}

		std::atomic<bool> g_stalled{ false };
	}

	bool SimulationStalled()
	{
		return g_stalled.load(std::memory_order_acquire);
	}

	void ResetSimulationStallWatch()
	{
		GlobalWatch().Reset();
		g_stalled.store(false, std::memory_order_release);
	}

	StallUpdate SampleSimulationStallWatch(bool a_active, std::uint64_t a_counter, std::uint64_t a_nowMs)
	{
		const auto update = GlobalWatch().Sample(a_active, a_counter, a_nowMs);
		g_stalled.store(GlobalWatch().Stalled(), std::memory_order_release);
		return update;
	}

	const char* MutationRefusalName(MutationRefusal a_refusal)
	{
		switch (a_refusal) {
		case MutationRefusal::kNone:
			return "ok";
		case MutationRefusal::kGameNotActive:
			return "the game is not running (no game loaded, paused, or at the menu)";
		case MutationRefusal::kSimulationStalled:
			return "physics is stalled (no character-proxy callback for over 2 s; the window is probably unfocused or the game paused) - refusing to write Havok state";
		case MutationRefusal::kNoPlayerProxy:
			return "no player proxy yet (load a game first)";
		}
		return "refused";
	}

	MutationRefusal EvaluateMutation(bool a_gameActive, bool a_stalled, bool a_havePlayerProxy)
	{
		if (!a_gameActive) {
			return MutationRefusal::kGameNotActive;
		}
		if (a_stalled) {
			return MutationRefusal::kSimulationStalled;
		}
		if (!a_havePlayerProxy) {
			return MutationRefusal::kNoPlayerProxy;
		}
		return MutationRefusal::kNone;
	}

	const char* PushApplyRefusalName(PushApplyRefusal a_refusal)
	{
		switch (a_refusal) {
		case PushApplyRefusal::kNone:
			return "ok";
		case PushApplyRefusal::kStalled:
			return "physics stalled while the request was pending - dropped without writing Havok state";
		case PushApplyRefusal::kNoController:
			return "the request carries no controller";
		case PushApplyRefusal::kNonFinite:
			return "the request carries a non-finite direction or dv";
		}
		return "rejected";
	}

	PushApplyRefusal EvaluatePushApply(bool a_stalled, bool a_haveController,
		float a_dx, float a_dy, float a_dz, float a_dv)
	{
		// Stall first: a stale request must be dropped even if it is otherwise
		// well-formed, and a stalled simulation must never be written to.
		if (a_stalled) {
			return PushApplyRefusal::kStalled;
		}
		if (!a_haveController) {
			return PushApplyRefusal::kNoController;
		}
		if (!std::isfinite(a_dx) || !std::isfinite(a_dy) || !std::isfinite(a_dz) || !std::isfinite(a_dv)) {
			return PushApplyRefusal::kNonFinite;
		}
		return PushApplyRefusal::kNone;
	}
}
