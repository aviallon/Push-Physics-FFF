#include "PCH.h"

#include "PushManager.h"

#include "Config.h"
#include "FrameClock.h"
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

		// Attach retry policy. The tick used to call AttachTo every frame, and a
		// refusal was logged every time (663 KB in 75 s in game). Attach is now
		// driven by the verified controller, so it succeeds on the first try; a
		// refusal can only mean the player has no usable proxy. Even then the
		// retry is rate-limited and a refusal is logged on its first occurrence
		// and then one "still refusing" summary per backoff window, never per
		// frame.
		constexpr std::uint64_t kAttachRetryMs = 1000;
		// One summary line per this many refused attempts (= seconds).
		constexpr std::uint32_t kRefusalLogEvery = 30;

		std::uint64_t g_lastAttachAttemptMs = 0;
		std::uint32_t g_attachAttempts = 0;
		std::uint32_t g_lastRefusalLogAttempt = 0;

		void NoteAttachSucceeded()
		{
			if (g_attachAttempts > 0) {
				logger::info("listener attach succeeded after {} refused attempt(s)", g_attachAttempts);
			}
			g_attachAttempts = 0;
			g_lastRefusalLogAttempt = 0;
		}

		void NoteAttachRefused()
		{
			++g_attachAttempts;
			const bool first = g_lastRefusalLogAttempt == 0;
			const bool backoff = g_attachAttempts - g_lastRefusalLogAttempt >= kRefusalLogEvery;
			if (!first && !backoff) {
				return;
			}
			g_lastRefusalLogAttempt = g_attachAttempts;
			logger::warn("listener attach still refused after {} attempt(s) (retry 1/s); "
						 "the verified player controller yielded no proxy",
				g_attachAttempts);
		}
	}

	PushListener* GetPushListener()
	{
		return &g_listener;
	}

	bool InstallPushListener()
	{
		try {
			auto* controller = PlayerController();
			if (!controller) {
				// Expected before a game is loaded; the main-thread tick retries.
				return false;
			}
			auto* proxy = controller->GetCharacterProxy();
			if (!proxy) {
				return false;
			}

			if (!g_listener.AttachTo(controller)) {
				return false;
			}
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
		auto* controller = PlayerController();
		auto* proxy = controller ? controller->GetCharacterProxy() : nullptr;
		if (!proxy) {
			return;  // mounted / ragdolled / mid-load: stop pushing, do not tear down
		}

		if (g_listener.Owner() == proxy) {
			return;  // already attached
		}

		// Attempt at most once per second: this is the fix for the per-frame log
		// flood, and it still re-attaches promptly after a controller rebuild.
		const auto now = FrameClock::NowMs();
		if (g_lastAttachAttemptMs != 0 && now - g_lastAttachAttemptMs < kAttachRetryMs) {
			return;
		}
		g_lastAttachAttemptMs = now;

		if (g_listener.Owner()) {
			logger::info("player proxy changed (0x{:X} -> 0x{:X}); re-attaching listener",
				reinterpret_cast<std::uintptr_t>(g_listener.Owner()),
				reinterpret_cast<std::uintptr_t>(proxy));
		}

		if (g_listener.AttachTo(controller)) {
			PushModel::ApplyProxyTuning(proxy);
			PushModel::LogCalibrationOnce();
			NoteAttachSucceeded();
		} else {
			NoteAttachRefused();
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
