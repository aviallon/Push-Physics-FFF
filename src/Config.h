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
		// fCharacterStrength < 0 (default -1) derives the value from the player's
		// race mass, level and physical skills (see the fields below); >= 0 is an
		// explicit override used verbatim. characterMass < 0 leaves the engine
		// value (defaults to 0).
		float characterStrength = -1.0f;
		float characterMass = -1.0f;

		// [Physics] Character-strength derivation. Havok's CharacterInteractionDemo
		// uses 5000 as m_characterStrength; there is no engine-set value (measured
		// FLT_MAX), so 5000 is the base. P is normalised so P = 1 at level 1 with a
		// physical-skill mean of 0.15, then the fitted anchors are L50/skill0.80 ->
		// P = 6 (~30k) and L252/skill1.00 -> P ~ 26.65 (~133k).
		// characterStrength = base * P * (raceBaseMass / referenceMass), clamped to
		// [fStrengthMin, fStrengthMax].
		float strengthBase = 5000.0f;
		float strengthReferenceMass = 80.0f;
		float strengthLevelGain = 0.05f;
		float strengthSkillGain = 1.371f;
		float strengthMin = 500.0f;
		float strengthMax = 200000.0f;
		// Effective player mass contested in the push model:
		// fPlayerMass * P^fMassPowerExponent (100 at base, ~350 at L50/skill0.80,
		// ~1000 at L252/skill1.00), so a maxed player can contest dragon-scale mass.
		float massPowerExponent = 0.7f;
		// Physical-skill weights (One-Handed, Two-Handed, Block, Heavy Armor,
		// Archery). They are normalised by their sum, so only their ratios matter.
		float strengthWeightOneHanded = 0.30f;
		float strengthWeightTwoHanded = 0.25f;
		float strengthWeightBlock = 0.20f;
		float strengthWeightHeavyArmor = 0.15f;
		float strengthWeightArchery = 0.10f;

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
