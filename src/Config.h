#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace pa
{
	// Directory containing PushAside.dll (Data/SKSE/Plugins), resolved from the
	// module handle rather than from the current working directory.
	[[nodiscard]] const std::filesystem::path& PluginDir();

	// Configuration, read once from <PluginDir>/PushAside.ini.
	//
	// Every field is the compiled-in default, so deleting the INI yields a working
	// plugin. A malformed value (not a number, trailing garbage, overflow) logs a
	// warning and keeps the default rather than silently becoming 0: that is the
	// difference between "the knob did nothing" and "the knob is broken".
	struct Config
	{
		// [General] Master switch. When false the plugin installs nothing at all.
		bool enabled = true;

		// [Listener] Path 1 detection: our listener's CharacterInteractionCallback.
		bool useCharacterInteraction = true;
		// [Listener] Path 2: our listener's ObjectInteractionCallback.
		bool useObjectInteraction = true;
		// [Listener] Fallback manifold scan inside ProcessConstraintsCallback. Off
		// by default: its leading-proxy ABI is an unverified NG reconstruction and
		// it is only enabled after the invariant self-check passes on live data.
		bool useManifoldScan = false;
		// [Listener] MinHook escalations (design.md 3.8). 0 = no detour at all.
		bool useEscalationHooks = false;
		// [Listener] E1 target-side pre-solve injection.
		bool targetSideInjection = false;

		// [Hooks] Verify escalation targets against the committed table before any
		// detour is created. Keep ON; it is only consulted when escalations are on.
		bool verifyTargets = true;

		// [Physics] Approach A: the proxy's own knobs (rigid bodies only).
		// < 0 leaves the engine value (characterStrength defaults to FLT_MAX,
		// characterMass to 0).
		float characterStrength = -1.0f;
		float characterMass = -1.0f;

		// [Physics] Mass model. Only ratios matter; absolute values do not.
		float playerMass = 100.0f;
		float defaultCharacterMass = 80.0f;
		float massRatioMax = 2.0f;
		float heavyMassRatio = 2.5f;
		float heavyScale = 0.25f;
		float heavyMinSpeed = 60.0f;

		// [Physics] Path 1: character vs character.
		float minRelSpeed = 20.0f;
		float restitution = 0.15f;
		float pushScale = 1.0f;
		float maxDeltaV = 250.0f;
		float maxInjectedSpeed = 350.0f;
		float pushDamping = 6.0f;           // 1/s decay of the per-target buffer
		float pushCooldownMs = 200.0f;
		float maxPushDurationMs = 600.0f;
		float reactionOnPusher = 0.0f;      // fraction of the push the pusher loses

		// [Physics] State scales and gates.
		float combatScale = 0.5f;
		float outOfCombatScale = 1.0f;
		float swimScale = 0.35f;
		float airborneScale = 0.5f;
		float unknownTargetScale = 0.5f;
		float lift = 0.0f;                  // reserved: injected direction is horizontal
		float staggerCooldownMs = 1500.0f;
		float staggerDeltaV = 120.0f;
		float staggerMassExponent = 0.5f;
		bool  staggerOutOfCombat = false;

		// [Physics] Path 2: character vs rigid body.
		bool  objectCustomImpulse = false;
		float objectShoveScale = 1.0f;
		float objectRestitution = 0.2f;
		float maxObjectImpulse = 0.0f;      // 0 = uncapped

		// [Policy]
		bool npcVsNpc = false;
		bool playerAsTarget = false;
		bool disableDuringDialogue = true;
		bool disableInCombat = false;       // gate 19 kill switches
		bool disableWhileSwimming = false;

		// [Budget]
		std::uint32_t maxInteractionsPerFrame = 32;
		std::uint32_t registryCapacity = 512;
		float         registryRefreshSec = 1.0f;

		// [Diagnostics]
		bool          debugLog = false;
		std::uint32_t debugLogMaxPerSec = 32;
		float         calibrationLogAfterSec = 5.0f;

		static Config& Get();
		void           Load();

		// One-line summary for the startup log. It names the settings that change
		// gameplay so a reader of a run can see the configuration.
		[[nodiscard]] std::string Summary() const;
	};
}
