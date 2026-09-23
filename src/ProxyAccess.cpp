#include "PCH.h"

#include "ProxyAccess.h"

#include "Health.h"

namespace pa
{
	namespace
	{
		// RE::VTABLE_bhkCharProxyController resolves through the VTABLE Address
		// Library. An unresolved relocation has address 0, which would make every
		// live vptr comparison fail silently (no listener ever attaches, health
		// still OK). Say so once and DEGRADE instead of accepting it quietly - the
		// per-frame path must not swallow an unresolved relocation.
		[[nodiscard]] bool VtableResolved(std::uintptr_t a_vtable, const char* a_which)
		{
			if (a_vtable != 0) {
				return true;
			}
			static bool warned = false;
			if (!warned) {
				warned = true;
				logger::error("{} vtable relocation is unresolved (address 0); controller verification is disabled", a_which);
				Health::Get().Degrade("bhkCharProxyController vtable relocation unresolved");
			}
			return false;
		}
	}

	RE::bhkCharProxyController* AsProxyController(RE::bhkCharacterController* a_ctrl)
	{
		if (!a_ctrl) {
			return nullptr;
		}
		// bhkCharacterController subobject vtable is VTABLE_bhkCharProxyController[1]
		// (AE 240560); the listener subobject at offset 0 carries [0] (AE 240558).
		static REL::Relocation<std::uintptr_t> kControllerVtable{ RE::VTABLE_bhkCharProxyController[1] };
		const auto                             expected = kControllerVtable.address();
		if (!VtableResolved(expected, "bhkCharProxyController[1]")) {
			return nullptr;
		}
		if (*reinterpret_cast<const void* const*>(a_ctrl) !=
			reinterpret_cast<const void*>(expected)) {
			return nullptr;
		}
		return static_cast<RE::bhkCharProxyController*>(a_ctrl);
	}

	RE::bhkCharProxyController* ControllerOf(RE::hkpCharacterProxy* a_proxy)
	{
		if (!a_proxy) {
			return nullptr;
		}

		auto* candidate = reinterpret_cast<RE::bhkCharProxyController*>(
			reinterpret_cast<std::uint8_t*>(a_proxy) - 0x350);

		// 1. the candidate must be a bhkCharProxyController (listener vtable 240558)
		static REL::Relocation<std::uintptr_t> kListenerVtable{ RE::VTABLE_bhkCharProxyController[0] };
		const auto                             expected = kListenerVtable.address();
		if (!VtableResolved(expected, "bhkCharProxyController[0]")) {
			return nullptr;
		}
		if (*reinterpret_cast<const void* const*>(candidate) !=
			reinterpret_cast<const void*>(expected)) {
			return nullptr;
		}

		// 2. and it must point back at exactly this proxy (definitive, free)
		return candidate->GetCharacterProxy() == a_proxy ? candidate : nullptr;
	}

	RE::hkpCharacterProxy* PlayerProxy()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return nullptr;
		}
		auto* pc = AsProxyController(player->GetCharController());
		return pc ? pc->GetCharacterProxy() : nullptr;
	}
}
