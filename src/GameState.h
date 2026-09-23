#pragma once

namespace pa
{
	// Main thread. RE::Main reports the game running. The REL::Relocation is
	// resolved and null-checked by the caller of this helper, so an unresolved
	// id can never turn a per-frame path into a read at address 0.
	[[nodiscard]] bool IsGameActive();
}
