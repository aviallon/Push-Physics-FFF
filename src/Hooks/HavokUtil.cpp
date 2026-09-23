#include "PCH.h"

#include "Hooks/HavokUtil.h"

#include <RE/H/hkpContactListener.h>
#include <RE/H/hkpWorld.h>

namespace pa::havok
{
	namespace
	{
		using RequireUtilFn = bool (*)(RE::hkpWorld*);
		using AddListenerFn = void* (*)(RE::hkpWorld*, RE::hkpContactListener*);

		// One-shot per function: an unresolved Address Library id would otherwise
		// be retried (and logged) every frame from the registration tick.
		void LogUnresolvedOnce(std::atomic<bool>& a_flag, const char* a_name, std::uint64_t a_seId, std::uint64_t a_aeId)
		{
			bool expected = false;
			if (!a_flag.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
				return;
			}
			logger::error("{} relocation unresolved (AE id {} / SE id {}): world contact listener disabled",
				a_name, a_aeId, a_seId);
		}
	}

	bool EnsureContactCallbackUtil(RE::hkpWorld* a_world)
	{
		if (!a_world) {
			return false;
		}
		static REL::Relocation<RequireUtilFn> fn{ REL::RelocationID(60588, 61437) };
		if (fn.address() == 0) {
			static std::atomic<bool> logged{ false };
			LogUnresolvedOnce(logged, "hkpCollisionCallbackUtil_requireCollisionCallbackUtil", 60588, 61437);
			return false;
		}
		return fn(a_world);
	}

	bool TryAddContactListener(RE::hkpWorld* a_world, RE::hkpContactListener* a_listener)
	{
		if (!a_world || !a_listener) {
			return false;
		}
		static REL::Relocation<AddListenerFn> fn{ REL::RelocationID(60543, 61383) };
		if (fn.address() == 0) {
			static std::atomic<bool> logged{ false };
			LogUnresolvedOnce(logged, "hkpWorld_addContactListener", 60543, 61383);
			return false;
		}
		fn(a_world, a_listener);
		return true;
	}

	bool HasContactListener(RE::hkpWorld* a_world, const RE::hkpContactListener* a_listener)
	{
		if (!a_world || !a_listener) {
			return false;
		}
		const auto& listeners = a_world->contactListeners;
		for (std::int32_t i = 0; i < listeners.size(); ++i) {
			if (listeners[i] == a_listener) {
				return true;
			}
		}
		return false;
	}
}
