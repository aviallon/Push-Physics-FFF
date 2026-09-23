#include "PCH.h"

#include "BuildInfo.h"
#include "Config.h"
#include "Health.h"
#include "Hooks/MainUpdateHook.h"
#include "ProxyRegistry.h"
#include "PushManager.h"
#include "PushModel.h"
#include "PushRegistry.h"

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
			// Main thread: refresh the actor<->proxy map, drop stale buffers, attach.
			pa::ProxyRegistry::Get().RebuildNow();
			pa::PushRegistry::Get().Clear();
			(void)pa::InstallPushListener();
			pa::PushModel::LogCalibrationOnce();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			// Never keep a proxy across a load; the orphan check ignores any late
			// calls the retired proxy might still make.
			pa::DetachPushListener();
			pa::PushRegistry::Get().Clear();
			pa::ProxyRegistry::Get().Invalidate();
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

	pa::ProxyRegistry::Get().Init(config.registryCapacity);
	pa::PushRegistry::Get().Init(config.registryCapacity);

	pa::PushModel::SetMainThreadId(std::this_thread::get_id());

	// The main-thread tick is driven by a verified MinHook function-entry detour
	// on RE::Main::Update (src/Hooks/MainUpdateHook.cpp). Attaching needs a player
	// proxy, which does not exist until a game is loaded; the tick attaches on
	// load and re-attaches whenever the player's controller is rebuilt.
	if (config.useEscalationHooks && !pa::InstallEscalationHooks()) {
		logger::error("escalation hooks requested but none could be verified; continuing listener-only");
	}

	if (!pa::InstallMainUpdateHook()) {
		logger::error("Main::Update hook not installed; attach/re-attach will not run per frame");
	}

	logger::info("health: {}", pa::Health::Get().Line());
	logger::info("...ready");
	return true;
}
