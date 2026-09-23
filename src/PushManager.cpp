#include "PCH.h"

#include "PushManager.h"

#include "Config.h"
#include "Hooks/HookTargets.h"
#include "ProxyAccess.h"
#include "PushListener.h"
#include "PushModel.h"

namespace pa
{
	namespace
	{
		// Static storage: the listener must outlive the engine's raw pointer to
		// it, i.e. the whole process.
		PushListener g_listener;
	}

	PushListener* GetPushListener()
	{
		return &g_listener;
	}

	bool InstallPushListener()
	{
		try {
			auto* proxy = PlayerProxy();
			if (!proxy) {
				// Expected before a game is loaded; the main-thread pump retries.
				return false;
			}

			g_listener.AttachTo(proxy);
			PushModel::ApplyProxyTuning(proxy);
			PushModel::LogCalibrationOnce();
			return true;
		} catch (...) {
			logger::error("push listener: attach threw; no listener installed");
			return false;
		}
	}

	void DetachPushListener()
	{
		g_listener.Detach();
	}

	void PushManagerMainThreadTick()
	{
		auto* proxy = PlayerProxy();
		if (!proxy) {
			return;  // mounted / ragdolled / mid-load: stop pushing, do not tear down
		}

		if (g_listener.Owner() != proxy) {
			if (g_listener.Owner()) {
				logger::info("player proxy changed (0x{:X} -> 0x{:X}); re-attaching listener",
					reinterpret_cast<std::uintptr_t>(g_listener.Owner()),
					reinterpret_cast<std::uintptr_t>(proxy));
			}
			g_listener.AttachTo(proxy);
			PushModel::ApplyProxyTuning(proxy);
			PushModel::LogCalibrationOnce();
		}
	}

	bool InstallEscalationHooks()
	{
		if (kHookTargetCount == 0) {
			logger::error("escalation hooks requested but HookTargets.def declares no targets; refusing to patch anything");
			return false;
		}
		// v1 declares no targets, so this path is unreachable. A future escalation
		// verifies every record against the committed table before creating any
		// MinHook detour (design.md 3.8); it is deliberately not implemented here.
		logger::error("escalation hooks requested but the installer is not implemented for {} target(s)", kHookTargetCount);
		return false;
	}

	void RemoveEscalationHooks()
	{
		// No detour is ever installed in v1.
	}
}
