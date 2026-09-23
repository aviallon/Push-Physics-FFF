#pragma once

// Simulation-health guards for the live-command side channel.
//
// Everything here is PURE: no clock, no logger, no game access, no file I/O, so
// the stall state machine and both refusal predicates are built and run by the
// off-game tests on Linux (tests/, SimGuard.cpp). The main thread owns the
// process-wide instance; the physics thread only reads the published stalled
// flag.

#include <cstdint>

namespace pa
{
	// -----------------------------------------------------------------------
	// Physics-stall watchdog.
	//
	// Skyrim does not step Havok while the window is unfocused or the game is
	// paused, but everything else keeps running: the main-thread tick still
	// fires, and a `push` request would be published into the slot and never
	// consumed (or consumed on a stale simulation). A stalled simulation used
	// to be indistinguishable from a healthy one in the logs. This machine
	// turns "the callbacks have not advanced" into an explicit state that
	// refuses mutating commands.
	// -----------------------------------------------------------------------

	// No ProcessConstraintsCallback progress for longer than this counts as
	// stalled. The physics callback runs ~170/s while the game is focused, so
	// 2 s is ~340 missed steps: not a scheduling hiccup.
	inline constexpr std::uint64_t kStallThresholdMs = 2000;

	enum class StallState
	{
		kUnknown,  // no active sample yet (main menu / no game loaded)
		kHealthy,  // the counter advanced within the threshold
		kStalled,  // the counter has been still for > kStallThresholdMs
	};

	struct StallUpdate
	{
		bool          enteredStall = false;
		bool          recovered = false;
		std::uint64_t stalledForMs = 0;  // set on enteredStall and recovered
	};

	class StallWatch
	{
	public:
		// One main-thread sample per tick. `a_active` is "the game reports
		// itself running"; `a_counter` is the physics callback counter
		// (monotonic, incremented on the physics thread); `a_nowMs` is a
		// monotonic clock in ms. An inactive sample resets to kUnknown and can
		// never report a stall.
		StallUpdate Sample(bool a_active, std::uint64_t a_counter, std::uint64_t a_nowMs);
		void        Reset();

		[[nodiscard]] StallState    State() const { return state_; }
		[[nodiscard]] bool          Stalled() const { return state_ == StallState::kStalled; }
		[[nodiscard]] std::uint64_t LastCounter() const { return lastCounter_; }
		[[nodiscard]] std::uint64_t LastAdvanceMs() const { return lastAdvanceMs_; }
		[[nodiscard]] std::uint64_t ThresholdMs() const { return thresholdMs_; }

	private:
		StallState    state_ = StallState::kUnknown;
		std::uint64_t lastCounter_ = 0;
		std::uint64_t lastAdvanceMs_ = 0;
		std::uint64_t thresholdMs_ = kStallThresholdMs;
	};

	// Process-wide instance, fed on the main thread from the existing tick and
	// read from the physics thread. The published flag is atomic.
	[[nodiscard]] bool SimulationStalled();
	void               ResetSimulationStallWatch();
	StallUpdate        SampleSimulationStallWatch(bool a_active, std::uint64_t a_counter, std::uint64_t a_nowMs);

	// -----------------------------------------------------------------------
	// Mutating-command refusal (main thread, before publishing a request).
	// -----------------------------------------------------------------------

	enum class MutationRefusal
	{
		kNone = 0,
		kGameNotActive,
		kSimulationStalled,
		kNoPlayerProxy,
	};

	[[nodiscard]] const char* MutationRefusalName(MutationRefusal a_refusal);

	// First failing check, in this order: game active, not stalled, player
	// proxy. Read-only commands never call this.
	[[nodiscard]] MutationRefusal EvaluateMutation(bool a_gameActive, bool a_stalled, bool a_havePlayerProxy);

	// -----------------------------------------------------------------------
	// Request validation (physics thread, before writing any Havok state).
	// -----------------------------------------------------------------------

	enum class PushApplyRefusal
	{
		kNone = 0,
		kStalled,      // simulation not stepping: a Havok write would be unsafe/stale
		kNoController, // the request carries no controller
		kNonFinite,    // direction or dv is NaN/inf
	};

	[[nodiscard]] const char* PushApplyRefusalName(PushApplyRefusal a_refusal);

	[[nodiscard]] PushApplyRefusal EvaluatePushApply(bool a_stalled, bool a_haveController,
		float a_dx, float a_dy, float a_dz, float a_dv);
}
