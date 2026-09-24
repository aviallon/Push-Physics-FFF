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

	RE::bhkCharRigidBodyController* AsRigidBodyController(RE::bhkCharacterController* a_ctrl)
	{
		if (!a_ctrl) {
			return nullptr;
		}
		// The controller subobject is the primary base (bhkCharacterController at
		// offset 0), whose vtable is VTABLE_bhkCharRigidBodyController[0] (AE
		// 240580); the chained hkpCharacterRigidBodyListener lives at +0x330 and is
		// [1] (AE 240583). Read off the engine's bhkCharRigidBodyController vtable
		// pair (RVA 0x1A89BF0 controller / 0x1A89C98 listener).
		static REL::Relocation<std::uintptr_t> kControllerVtable{ RE::VTABLE_bhkCharRigidBodyController[0] };
		const auto                             expected = kControllerVtable.address();
		if (!VtableResolved(expected, "bhkCharRigidBodyController[0]")) {
			return nullptr;
		}
		if (*reinterpret_cast<const void* const*>(a_ctrl) !=
			reinterpret_cast<const void*>(expected)) {
			return nullptr;
		}
		return static_cast<RE::bhkCharRigidBodyController*>(a_ctrl);
	}

	RE::hkpCharacterRigidBody* CharacterRigidBodyFor(RE::bhkCharRigidBodyController* a_ctrl)
	{
		if (!a_ctrl) {
			return nullptr;
		}
		// bhkCharacterRigidBody::referencedObject is the hkpCharacterRigidBody; the
		// engine's own getter (RVA 0x109AA00) reads controller+0x350 and returns
		// [that + 0x20] (the hkpRigidBody), which pins this layout.
		return static_cast<RE::hkpCharacterRigidBody*>(a_ctrl->charRigidBody.referencedObject.get());
	}

	RE::bhkCharProxyController* PlayerController()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return nullptr;
		}
		return AsProxyController(player->GetCharController());
	}

	RE::hkpCharacterProxy* PlayerProxy()
	{
		auto* pc = PlayerController();
		return pc ? pc->GetCharacterProxy() : nullptr;
	}

	void LogRetiredInversionOnce(RE::hkpCharacterProxy* a_proxy)
	{
		if (!a_proxy) {
			return;
		}
		// One call per process: this is a diagnostic, not a gate.
		static std::atomic<bool> logged{ false };
		bool                     expected = false;
		if (!logged.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
			return;
		}

		static REL::Relocation<std::uintptr_t> kListenerVtable{ RE::VTABLE_bhkCharProxyController[0] };
		const auto                             expectedVtable = kListenerVtable.address();
		if (expectedVtable == 0) {
			return;
		}

		// The retired inversion assumed the hkpCharacterProxy were embedded at
		// controller + 0x350. It is not: the controller holds a pointer to a
		// separately allocated proxy, so this qword is read only to *show* the
		// mismatch. The old code already read it every frame without faulting.
		const auto observed = *reinterpret_cast<const std::uintptr_t*>(
			reinterpret_cast<const std::uint8_t*>(a_proxy) - 0x350);
		if (observed == expectedVtable) {
			return;  // the retired inversion happens to hold here; nothing to report
		}

		logger::warn(
			"retired controller inversion diagnostic (one-shot, no longer gates attach): "
			"*(void**)(proxy-0x350)=0x{:X}, expected listener vtable 0x{:X}; "
			"the verified controller is used instead",
			observed, expectedVtable);
	}
}
