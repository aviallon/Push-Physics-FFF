#pragma once

// Pure CSV formatting for PushAside.trace. No Windows/RE dependency, so the
// header/row shape is pinned by the off-game tests.

#include <cstddef>
#include <cstdint>

namespace pa
{
	// One trace row's worth of plain values. The game side fills it on the main
	// thread; nothing here allocates.
	struct TraceSample
	{
		std::uint64_t frame = 0;
		double        ms = 0.0;

		std::uintptr_t playerProxy = 0;
		float          playerPos[3]{};
		float          playerVel[3]{};
		float          playerMass = 0.0f;

		std::uintptr_t bumpCharBody = 0;
		std::uint32_t  bumpRefr = 0;
		std::uintptr_t bumpActor = 0;
		std::uintptr_t bumpCtrl = 0;
		std::uintptr_t bumpCtrlVptr = 0;
		std::uintptr_t bumpRb = 0;
		float          bumpRbVel[3]{};
		float          bumpRbPos[3]{};
		int            bumpRbMotion = -1;   // hkpMotion::MotionType, -1 = no rigid body
		float          bumpRbMass = 0.0f;
		int            bumpRbDynamic = -1;  // 1 dynamic, 0 keyframed/fixed, -1 unknown

		int   pushMode = 0;                 // PushMode value, 0 = none requested
		float pushDv = 0.0f;
		int   pushMech = 0;                 // bitmask: 1 ctrl, 2 rb
		int   pushChanged = 0;
		float pushCtrlFrom[3]{};
		float pushCtrlTo[3]{};
		float pushRbFrom[3]{};
		float pushRbTo[3]{};

		std::uint32_t watchForm = 0;
		float         watchPos[3]{};
		float         watchCtrlVel[3]{};
		float         watchRbVel[3]{};
		// The engine character-state push fields on the watched controller, plus
		// its rigid body's motion type. These are what the `state` mechanism writes
		// and what the character update consumes.
		float         watchInitVel[3]{};  // bhkCharacterController::initialVelocity (+0xA0)
		float         watchVTime = 0.0f;  // bhkCharacterController::velocityTime (+0x220)
		float         watchOutVel[3]{};   // bhkCharacterController::outVelocity (+0x90)
		int           watchMotion = -1;   // hkpMotion::MotionType, -1 = no rigid body
		int           watchDynamic = -1;  // 1 dynamic, 0 keyframed/fixed, -1 unknown
	};

	// The CSV column header, including its trailing '\n'. Static storage.
	[[nodiscard]] const char* TraceHeader();

	// Legend lines explaining the numeric enums, including their trailing '\n'.
	[[nodiscard]] const char* TraceLegend();

	// Format one row (with a trailing '\n') into a_out. Returns the number of
	// characters the full row would need, like snprintf: when it is >= a_size the
	// row was truncated and the caller must NOT write it.
	[[nodiscard]] int FormatTraceRow(const TraceSample& a_sample, char* a_out, std::size_t a_size);

	// Format a "# ..." comment line (with a trailing '\n').
	[[nodiscard]] int FormatTraceComment(const char* a_text, char* a_out, std::size_t a_size);
}
