#include "PCH.h"

#include "BuildInfo.h"
#include "Config.h"
#include "Health.h"
#include "Hooks/FrameTickHook.h"
#include "LiveConfig.h"
#include "ProxyRegistry.h"
#include "PushManager.h"
#include "PushModel.h"
#include "PushRegistry.h"
#include "SimGuard.h"
#include "WorldContactListener.h"

namespace
{
	void SetupLog()
	{
		SKSE::log::init();
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// Main thread, but the world is still being torn down / rebuilt (the
			// player and every character controller may be mid-reconstruction).
			// Do NOT iterate ProcessLists or touch an Actor here: set the dirty
			// flag and let ProxyRegistry::MainThreadTick() rebuild once
			// PlayerCharacter exists, RE::Main reports gameActive and the settle
			// frames have elapsed. This handler caused the 2026-09-23 null call
			// by running RebuildNow() -> StateFlags() -> IsInBleedout() here.
			pa::ProxyRegistry::Get().RequestRebuild();
			pa::PushRegistry::Get().Clear();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			// Never keep a proxy across a load; the orphan check ignores any late
			// calls the retired proxy might still make. Only invalidate - no world
			// iteration and no attach.
			pa::DetachPushListener();
			pa::PushRegistry::Get().Clear();
			pa::ProxyRegistry::Get().Invalidate();
			pa::ResetWorldContactRegistration();
			// The listener is retired, so ProcessConstraintsCallback stops until the
			// next attach. Drop the stall baseline so the gap is not reported as a
			// physics stall on the new game's first tick.
			pa::ResetSimulationStallWatch();
			break;
		default:
			break;
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();

	logger::info("PushAside v" PA_VERSION " (Skyrim AE 1.7.104, CommonLibSSE-NG, build " PA_BUILD_ID ") loading");

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("SKSE", OnMessage);
	}

	auto& config = pa::Config::Get();
	config.Load();
	if (!config.enabled) {
		logger::info("disabled in PushAside.ini - attaching nothing");
		pa::Health::Get().Off("disabled in PushAside.ini");
		return true;
	}

	logger::info("config summary: {}", config.Summary());

	// Publish the loaded config as the live snapshot the physics callback reads.
	// `set` in PushAside.cmd republishes after each change; without this the
	// callback would keep seeing the compiled-in defaults.
	pa::LiveConfig::Publish(config);

	pa::ProxyRegistry::Get().Init(config.registryCapacity);
	pa::PushRegistry::Get().Init(config.registryCapacity);

	pa::PushModel::SetMainThreadId(std::this_thread::get_id());

	// The main-thread tick is driven by a verified MinHook function-entry detour
	// on the leaf Main::Update calls last before its epilogue
	// (src/Hooks/FrameTickHook.cpp). Main::Update's own entry is left alone: it is
	// already detoured by HDT-SMP, so our entry hook was refused. Attaching needs a
	// player proxy, which does not exist until a game is loaded; the tick attaches
	// on load and re-attaches whenever the player's controller is rebuilt.
	if (config.useEscalationHooks && !pa::InstallEscalationHooks()) {
		logger::error("escalation hooks requested but none could be verified; continuing listener-only");
	}

	if (!pa::InstallFrameTickHook()) {
		logger::error("frame-tick hook not installed; attach/re-attach will not run per frame");
	}

	logger::info("health: {}", pa::Health::Get().Line());
	logger::info("...ready");
	return true;
}
