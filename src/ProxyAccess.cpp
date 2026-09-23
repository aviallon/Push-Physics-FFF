#include "PCH.h"

#include "ProxyAccess.h"

namespace pa
{
	RE::bhkCharProxyController* AsProxyController(RE::bhkCharacterController* a_ctrl)
	{
		if (!a_ctrl) {
			return nullptr;
		}
		// bhkCharacterController subobject vtable is VTABLE_bhkCharProxyController[1]
		// (AE 240560); the listener subobject at offset 0 carries [0] (AE 240558).
		static REL::Relocation<std::uintptr_t> kControllerVtable{ RE::VTABLE_bhkCharProxyController[1] };
		if (*reinterpret_cast<const void* const*>(a_ctrl) !=
			reinterpret_cast<const void*>(kControllerVtable.address())) {
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
		if (*reinterpret_cast<const void* const*>(candidate) !=
			reinterpret_cast<const void*>(kListenerVtable.address())) {
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
