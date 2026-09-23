#include "PCH.h"

#include "Config.h"

#include <cerrno>
#include <cstdlib>
#include <vector>

namespace pa
{
	namespace
	{
		std::filesystem::path g_pluginDir;

		[[nodiscard]] std::string ReadString(const char* a_section, const char* a_key, const std::filesystem::path& a_ini)
		{
			std::vector<char> buffer(512);
			for (;;) {
				const auto len = ::GetPrivateProfileStringA(
					a_section, a_key, "", buffer.data(), static_cast<DWORD>(buffer.size()), a_ini.string().c_str());
				if (len < buffer.size() - 1) {
					return std::string(buffer.data(), len);
				}
				buffer.resize(buffer.size() * 2);
			}
		}

		// A malformed value logs and keeps the default, so a typo in the INI can
		// never turn a gameplay constant into 0 (or into whatever strtof leaves).
		[[nodiscard]] float ReadFloat(const char* a_section, const char* a_key, float a_default, const std::filesystem::path& a_ini)
		{
			const auto text = ReadString(a_section, a_key, a_ini);
			if (text.empty()) {
				return a_default;
			}
			errno = 0;
			char*       end = nullptr;
			const float value = std::strtof(text.c_str(), &end);
			if (end == text.c_str() || *end != '\0' || errno == ERANGE) {
				logger::warn("config: [{}] {}='{}' is not a valid float; keeping default {}", a_section, a_key, text, a_default);
				return a_default;
			}
			return value;
		}

		[[nodiscard]] std::uint32_t ReadUInt(const char* a_section, const char* a_key, std::uint32_t a_default, const std::filesystem::path& a_ini)
		{
			const auto text = ReadString(a_section, a_key, a_ini);
			if (text.empty()) {
				return a_default;
			}
			errno = 0;
			char*                 end = nullptr;
			const unsigned long   value = std::strtoul(text.c_str(), &end, 10);
			if (end == text.c_str() || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFul) {
				logger::warn("config: [{}] {}='{}' is not a valid unsigned integer; keeping default {}", a_section, a_key, text, a_default);
				return a_default;
			}
			return static_cast<std::uint32_t>(value);
		}

		[[nodiscard]] bool ReadBool(const char* a_section, const char* a_key, bool a_default, const std::filesystem::path& a_ini)
		{
			const auto text = ReadString(a_section, a_key, a_ini);
			if (text.empty()) {
				return a_default;
			}
			if (text == "0" || text == "false" || text == "False" || text == "no") {
				return false;
			}
			if (text == "1" || text == "true" || text == "True" || text == "yes") {
				return true;
			}
			logger::warn("config: [{}] {}='{}' is not a valid boolean (use 0/1); keeping default {}", a_section, a_key, text, a_default ? 1 : 0);
			return a_default;
		}

		void ResolvePluginDir()
		{
			HMODULE self = nullptr;
			if (!::GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&ResolvePluginDir),
					&self)) {
				return;
			}

			char buffer[MAX_PATH]{};
			const auto len = ::GetModuleFileNameA(self, buffer, MAX_PATH);
			if (len == 0) {
				return;
			}

			g_pluginDir = std::filesystem::path(std::string(buffer, len)).parent_path();
		}
	}

	const std::filesystem::path& PluginDir()
	{
		if (g_pluginDir.empty()) {
			ResolvePluginDir();
		}
		return g_pluginDir;
	}

	Config& Config::Get()
	{
		static Config config;
		return config;
	}

	void Config::Load()
	{
		const auto ini = PluginDir() / "PushAside.ini";
		if (!std::filesystem::exists(ini)) {
			logger::info("no {} - using compiled-in defaults", ini.string());
			return;
		}

		// [General]
		enabled = ReadBool("General", "bEnabled", enabled, ini);

		// [Listener]
		useCharacterInteraction = ReadBool("Listener", "bUseCharacterInteraction", useCharacterInteraction, ini);
		useObjectInteraction = ReadBool("Listener", "bUseObjectInteraction", useObjectInteraction, ini);
		useManifoldScan = ReadBool("Listener", "bUseManifoldScan", useManifoldScan, ini);
		useBumpDetection = ReadBool("Listener", "bUseBumpDetection", useBumpDetection, ini);
		useEscalationHooks = ReadBool("Listener", "bUseEscalationHooks", useEscalationHooks, ini);
		targetSideInjection = ReadBool("Listener", "bTargetSideInjection", targetSideInjection, ini);

		// [Hooks]
		verifyTargets = ReadBool("Hooks", "bVerifyTargets", verifyTargets, ini);

		// [Physics] Approach A
		characterStrength = ReadFloat("Physics", "fCharacterStrength", characterStrength, ini);
		characterMass = ReadFloat("Physics", "fCharacterMass", characterMass, ini);

		// [Physics] character-strength derivation
		strengthBase = ReadFloat("Physics", "fStrengthBase", strengthBase, ini);
		strengthReferenceMass = ReadFloat("Physics", "fStrengthReferenceMass", strengthReferenceMass, ini);
		strengthLevelGain = ReadFloat("Physics", "fStrengthLevelGain", strengthLevelGain, ini);
		strengthSkillGain = ReadFloat("Physics", "fStrengthSkillGain", strengthSkillGain, ini);
		strengthMin = ReadFloat("Physics", "fStrengthMin", strengthMin, ini);
		strengthMax = ReadFloat("Physics", "fStrengthMax", strengthMax, ini);
		massPowerExponent = ReadFloat("Physics", "fMassPowerExponent", massPowerExponent, ini);
		strengthWeightOneHanded = ReadFloat("Physics", "fStrengthWeightOneHanded", strengthWeightOneHanded, ini);
		strengthWeightTwoHanded = ReadFloat("Physics", "fStrengthWeightTwoHanded", strengthWeightTwoHanded, ini);
		strengthWeightBlock = ReadFloat("Physics", "fStrengthWeightBlock", strengthWeightBlock, ini);
		strengthWeightHeavyArmor = ReadFloat("Physics", "fStrengthWeightHeavyArmor", strengthWeightHeavyArmor, ini);
		strengthWeightArchery = ReadFloat("Physics", "fStrengthWeightArchery", strengthWeightArchery, ini);

		// [Physics] mass model
		playerMass = ReadFloat("Physics", "fPlayerMass", playerMass, ini);
		defaultCharacterMass = ReadFloat("Physics", "fDefaultCharacterMass", defaultCharacterMass, ini);
		massRatioMax = ReadFloat("Physics", "fMassRatioMax", massRatioMax, ini);
		heavyMassRatio = ReadFloat("Physics", "fHeavyMassRatio", heavyMassRatio, ini);
		heavyScale = ReadFloat("Physics", "fHeavyScale", heavyScale, ini);
		heavyMinSpeed = ReadFloat("Physics", "fHeavyMinSpeed", heavyMinSpeed, ini);

		// [Physics] Path 1
		minRelSpeed = ReadFloat("Physics", "fMinRelSpeed", minRelSpeed, ini);
		restitution = ReadFloat("Physics", "fRestitution", restitution, ini);
		pushScale = ReadFloat("Physics", "fPushScale", pushScale, ini);
		maxDeltaV = ReadFloat("Physics", "fMaxDeltaV", maxDeltaV, ini);
		maxInjectedSpeed = ReadFloat("Physics", "fMaxInjectedSpeed", maxInjectedSpeed, ini);
		pushDamping = ReadFloat("Physics", "fPushDamping", pushDamping, ini);
		pushCooldownMs = ReadFloat("Physics", "fPushCooldownMs", pushCooldownMs, ini);
		maxPushDurationMs = ReadFloat("Physics", "fMaxPushDurationMs", maxPushDurationMs, ini);
		reactionOnPusher = ReadFloat("Physics", "fReactionOnPusher", reactionOnPusher, ini);

		// [Physics] state scales / gates
		combatScale = ReadFloat("Physics", "fCombatScale", combatScale, ini);
		outOfCombatScale = ReadFloat("Physics", "fOutOfCombatScale", outOfCombatScale, ini);
		swimScale = ReadFloat("Physics", "fSwimScale", swimScale, ini);
		airborneScale = ReadFloat("Physics", "fAirborneScale", airborneScale, ini);
		unknownTargetScale = ReadFloat("Physics", "fUnknownTargetScale", unknownTargetScale, ini);
		lift = ReadFloat("Physics", "fLift", lift, ini);
		staggerCooldownMs = ReadFloat("Physics", "fStaggerCooldownMs", staggerCooldownMs, ini);
		staggerDeltaV = ReadFloat("Physics", "fStaggerDeltaV", staggerDeltaV, ini);
		staggerMassExponent = ReadFloat("Physics", "fStaggerMassExponent", staggerMassExponent, ini);
		staggerOutOfCombat = ReadBool("Physics", "bStaggerOutOfCombat", staggerOutOfCombat, ini);

		// [Physics] Path 2
		objectCustomImpulse = ReadBool("Physics", "bObjectCustomImpulse", objectCustomImpulse, ini);
		objectShoveScale = ReadFloat("Physics", "fObjectShoveScale", objectShoveScale, ini);
		objectRestitution = ReadFloat("Physics", "fObjectRestitution", objectRestitution, ini);
		maxObjectImpulse = ReadFloat("Physics", "fMaxObjectImpulse", maxObjectImpulse, ini);

		// [Policy]
		npcVsNpc = ReadBool("Policy", "bNpcVsNpc", npcVsNpc, ini);
		playerAsTarget = ReadBool("Policy", "bPlayerAsTarget", playerAsTarget, ini);
		disableDuringDialogue = ReadBool("Policy", "bDisableDuringDialogue", disableDuringDialogue, ini);
		disableInCombat = ReadBool("Policy", "bDisableInCombat", disableInCombat, ini);
		disableWhileSwimming = ReadBool("Policy", "bDisableWhileSwimming", disableWhileSwimming, ini);

		// [Budget]
		maxInteractionsPerFrame = ReadUInt("Budget", "uMaxInteractionsPerFrame", maxInteractionsPerFrame, ini);
		registryCapacity = ReadUInt("Budget", "uRegistryCapacity", registryCapacity, ini);
		registryRefreshSec = ReadFloat("Budget", "fRegistryRefreshSec", registryRefreshSec, ini);

		// [Diagnostics]
		debugLog = ReadBool("Diagnostics", "bDebugLog", debugLog, ini);
		debugLogMaxPerSec = ReadUInt("Diagnostics", "uDebugLogMaxPerSec", debugLogMaxPerSec, ini);
		calibrationLogAfterSec = ReadFloat("Diagnostics", "fCalibrationLogAfterSec", calibrationLogAfterSec, ini);

		logger::info("config loaded from {}", ini.string());
	}

	std::string Config::Summary() const
	{
		return "enabled=" + std::to_string(enabled ? 1 : 0) +
			" charInteraction=" + std::to_string(useCharacterInteraction ? 1 : 0) +
			" objInteraction=" + std::to_string(useObjectInteraction ? 1 : 0) +
			" manifoldScan=" + std::to_string(useManifoldScan ? 1 : 0) +
			" bumpDetection=" + std::to_string(useBumpDetection ? 1 : 0) +
			" escalations=" + std::to_string(useEscalationHooks ? 1 : 0) +
			" pushScale=" + std::to_string(pushScale) +
			" minRelSpeed=" + std::to_string(minRelSpeed) +
			" maxDeltaV=" + std::to_string(maxDeltaV) +
			" cooldownMs=" + std::to_string(pushCooldownMs) +
			" playerMass=" + std::to_string(playerMass) +
			" defaultMass=" + std::to_string(defaultCharacterMass) +
			" heavyRatio=" + std::to_string(heavyMassRatio) +
			" combatScale=" + std::to_string(combatScale) +
			" debug=" + std::to_string(debugLog ? 1 : 0);
	}
}
