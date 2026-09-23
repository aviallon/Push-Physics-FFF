#include "PCH.h"

#include "GameState.h"

#include <RE/M/Main.h>

namespace pa
{
	bool IsGameActive()
	{
		// RE::Main::GetSingleton() dereferences its REL::Relocation<Main**>
		// without checking it. Resolve and check the relocation ourselves, so an
		// unresolved id can never turn a per-frame path into a read at address 0.
		static REL::Relocation<RE::Main**> singleton{ REL::RelocationID(516943, 403449) };
		if (singleton.address() == 0) {
			return false;
		}
		auto* main = *singleton;
		return main != nullptr && main->GetRuntimeData().gameActive;
	}
}
