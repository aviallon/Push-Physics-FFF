#include "LiveConfig.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pa::LiveConfig
{
	namespace
	{
		enum class Kind
		{
			kBool,
			kFloat,
			kUInt,
		};

		struct KeyEntry
		{
			const char*   section;
			const char*   key;
			Kind          kind;
			bool Config::*asBool;
			float Config::*asFloat;
			std::uint32_t Config::*asUInt;
		};

		// Every key Config.cpp reads, in the section it is read from. The `set`
		// command resolves against this table, so a renamed key fails the command
		// rather than silently doing nothing.
		constexpr KeyEntry kKeys[] = {
			{ "General", "bEnabled", Kind::kBool, &Config::enabled, nullptr, nullptr },

			{ "Listener", "bUseCharacterInteraction", Kind::kBool, &Config::useCharacterInteraction, nullptr, nullptr },
			{ "Listener", "bUseObjectInteraction", Kind::kBool, &Config::useObjectInteraction, nullptr, nullptr },
			{ "Listener", "bUseManifoldScan", Kind::kBool, &Config::useManifoldScan, nullptr, nullptr },
			{ "Listener", "bUseBumpDetection", Kind::kBool, &Config::useBumpDetection, nullptr, nullptr },
			{ "Listener", "bUseEscalationHooks", Kind::kBool, &Config::useEscalationHooks, nullptr, nullptr },
			{ "Listener", "bTargetSideInjection", Kind::kBool, &Config::targetSideInjection, nullptr, nullptr },

			{ "Hooks", "bVerifyTargets", Kind::kBool, &Config::verifyTargets, nullptr, nullptr },

			{ "Physics", "fCharacterStrength", Kind::kFloat, nullptr, &Config::characterStrength, nullptr },
			{ "Physics", "fCharacterMass", Kind::kFloat, nullptr, &Config::characterMass, nullptr },
			{ "Physics", "fStrengthBase", Kind::kFloat, nullptr, &Config::strengthBase, nullptr },
			{ "Physics", "fStrengthReferenceMass", Kind::kFloat, nullptr, &Config::strengthReferenceMass, nullptr },
			{ "Physics", "fStrengthLevelGain", Kind::kFloat, nullptr, &Config::strengthLevelGain, nullptr },
			{ "Physics", "fStrengthSkillGain", Kind::kFloat, nullptr, &Config::strengthSkillGain, nullptr },
			{ "Physics", "fStrengthMin", Kind::kFloat, nullptr, &Config::strengthMin, nullptr },
			{ "Physics", "fStrengthMax", Kind::kFloat, nullptr, &Config::strengthMax, nullptr },
			{ "Physics", "fMassPowerExponent", Kind::kFloat, nullptr, &Config::massPowerExponent, nullptr },
			{ "Physics", "fStrengthWeightOneHanded", Kind::kFloat, nullptr, &Config::strengthWeightOneHanded, nullptr },
			{ "Physics", "fStrengthWeightTwoHanded", Kind::kFloat, nullptr, &Config::strengthWeightTwoHanded, nullptr },
			{ "Physics", "fStrengthWeightBlock", Kind::kFloat, nullptr, &Config::strengthWeightBlock, nullptr },
			{ "Physics", "fStrengthWeightHeavyArmor", Kind::kFloat, nullptr, &Config::strengthWeightHeavyArmor, nullptr },
			{ "Physics", "fStrengthWeightArchery", Kind::kFloat, nullptr, &Config::strengthWeightArchery, nullptr },
			{ "Physics", "fPlayerMass", Kind::kFloat, nullptr, &Config::playerMass, nullptr },
			{ "Physics", "fDefaultCharacterMass", Kind::kFloat, nullptr, &Config::defaultCharacterMass, nullptr },
			{ "Physics", "fMassRatioMax", Kind::kFloat, nullptr, &Config::massRatioMax, nullptr },
			{ "Physics", "fHeavyMassRatio", Kind::kFloat, nullptr, &Config::heavyMassRatio, nullptr },
			{ "Physics", "fHeavyScale", Kind::kFloat, nullptr, &Config::heavyScale, nullptr },
			{ "Physics", "fHeavyMinSpeed", Kind::kFloat, nullptr, &Config::heavyMinSpeed, nullptr },
			{ "Physics", "fMinRelSpeed", Kind::kFloat, nullptr, &Config::minRelSpeed, nullptr },
			{ "Physics", "fRestitution", Kind::kFloat, nullptr, &Config::restitution, nullptr },
			{ "Physics", "fPushScale", Kind::kFloat, nullptr, &Config::pushScale, nullptr },
			{ "Physics", "fMaxDeltaV", Kind::kFloat, nullptr, &Config::maxDeltaV, nullptr },
			{ "Physics", "fMaxInjectedSpeed", Kind::kFloat, nullptr, &Config::maxInjectedSpeed, nullptr },
			{ "Physics", "fPushDamping", Kind::kFloat, nullptr, &Config::pushDamping, nullptr },
			{ "Physics", "fPushCooldownMs", Kind::kFloat, nullptr, &Config::pushCooldownMs, nullptr },
			{ "Physics", "fMaxPushDurationMs", Kind::kFloat, nullptr, &Config::maxPushDurationMs, nullptr },
			{ "Physics", "fReactionOnPusher", Kind::kFloat, nullptr, &Config::reactionOnPusher, nullptr },
			{ "Physics", "fCombatScale", Kind::kFloat, nullptr, &Config::combatScale, nullptr },
			{ "Physics", "fOutOfCombatScale", Kind::kFloat, nullptr, &Config::outOfCombatScale, nullptr },
			{ "Physics", "fSwimScale", Kind::kFloat, nullptr, &Config::swimScale, nullptr },
			{ "Physics", "fAirborneScale", Kind::kFloat, nullptr, &Config::airborneScale, nullptr },
			{ "Physics", "fUnknownTargetScale", Kind::kFloat, nullptr, &Config::unknownTargetScale, nullptr },
			{ "Physics", "fLift", Kind::kFloat, nullptr, &Config::lift, nullptr },
			{ "Physics", "fStaggerCooldownMs", Kind::kFloat, nullptr, &Config::staggerCooldownMs, nullptr },
			{ "Physics", "fStaggerDeltaV", Kind::kFloat, nullptr, &Config::staggerDeltaV, nullptr },
			{ "Physics", "fStaggerMassExponent", Kind::kFloat, nullptr, &Config::staggerMassExponent, nullptr },
			{ "Physics", "bStaggerOutOfCombat", Kind::kBool, &Config::staggerOutOfCombat, nullptr, nullptr },
			{ "Physics", "bObjectCustomImpulse", Kind::kBool, &Config::objectCustomImpulse, nullptr, nullptr },
			{ "Physics", "fObjectShoveScale", Kind::kFloat, nullptr, &Config::objectShoveScale, nullptr },
			{ "Physics", "fObjectRestitution", Kind::kFloat, nullptr, &Config::objectRestitution, nullptr },
			{ "Physics", "fMaxObjectImpulse", Kind::kFloat, nullptr, &Config::maxObjectImpulse, nullptr },

			{ "Policy", "bNpcVsNpc", Kind::kBool, &Config::npcVsNpc, nullptr, nullptr },
			{ "Policy", "bPlayerAsTarget", Kind::kBool, &Config::playerAsTarget, nullptr, nullptr },
			{ "Policy", "bDisableDuringDialogue", Kind::kBool, &Config::disableDuringDialogue, nullptr, nullptr },
			{ "Policy", "bDisableInCombat", Kind::kBool, &Config::disableInCombat, nullptr, nullptr },
			{ "Policy", "bDisableWhileSwimming", Kind::kBool, &Config::disableWhileSwimming, nullptr, nullptr },

			{ "Budget", "uMaxInteractionsPerFrame", Kind::kUInt, nullptr, nullptr, &Config::maxInteractionsPerFrame },
			{ "Budget", "uRegistryCapacity", Kind::kUInt, nullptr, nullptr, &Config::registryCapacity },
			{ "Budget", "fRegistryRefreshSec", Kind::kFloat, nullptr, &Config::registryRefreshSec, nullptr },

			{ "Diagnostics", "bDebugLog", Kind::kBool, &Config::debugLog, nullptr, nullptr },
			{ "Diagnostics", "uDebugLogMaxPerSec", Kind::kUInt, nullptr, nullptr, &Config::debugLogMaxPerSec },
			{ "Diagnostics", "fCalibrationLogAfterSec", Kind::kFloat, nullptr, &Config::calibrationLogAfterSec, nullptr },
			{ "Diagnostics", "bTrace", Kind::kBool, &Config::trace, nullptr, nullptr },
		};

		[[nodiscard]] bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
		{
			if (a_lhs.size() != a_rhs.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_lhs.size(); ++i) {
				char l = a_lhs[i];
				char r = a_rhs[i];
				if (l >= 'A' && l <= 'Z') {
					l = static_cast<char>(l + ('a' - 'A'));
				}
				if (r >= 'A' && r <= 'Z') {
					r = static_cast<char>(r + ('a' - 'A'));
				}
				if (l != r) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool ParseBool(std::string_view a_text, bool& a_out)
		{
			if (IEquals(a_text, "1") || IEquals(a_text, "true") || IEquals(a_text, "yes") || IEquals(a_text, "on")) {
				a_out = true;
				return true;
			}
			if (IEquals(a_text, "0") || IEquals(a_text, "false") || IEquals(a_text, "no") || IEquals(a_text, "off")) {
				a_out = false;
				return true;
			}
			return false;
		}

		[[nodiscard]] bool ParseFloat(std::string_view a_text, float& a_out)
		{
			if (a_text.empty()) {
				return false;
			}
			std::string text(a_text);
			errno = 0;
			char*       end = nullptr;
			const float value = std::strtof(text.c_str(), &end);
			if (end == text.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
				return false;
			}
			a_out = value;
			return true;
		}

		[[nodiscard]] bool ParseUInt(std::string_view a_text, std::uint32_t& a_out)
		{
			if (a_text.empty()) {
				return false;
			}
			std::string text(a_text);
			errno = 0;
			char*               end = nullptr;
			const unsigned long value = std::strtoul(text.c_str(), &end, 10);
			if (end == text.c_str() || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFul) {
				return false;
			}
			a_out = static_cast<std::uint32_t>(value);
			return true;
		}

		// Seqlock: main thread is the only writer; any thread may read.
		Config                     g_buffers[2]{};
		std::atomic<std::uint32_t> g_seq{ 0 };
		std::atomic<std::uint32_t> g_index{ 0 };
	}

	void Publish(const Config& a_cfg)
	{
		const auto seq = g_seq.load(std::memory_order_relaxed);
		g_seq.store(seq + 1, std::memory_order_release);  // odd: publishing
		const auto next = g_index.load(std::memory_order_relaxed) ^ 1u;
		g_buffers[next] = a_cfg;
		g_index.store(next, std::memory_order_relaxed);
		g_seq.store(seq + 2, std::memory_order_release);  // even: stable
	}

	Config Snapshot()
	{
		for (;;) {
			const auto s1 = g_seq.load(std::memory_order_acquire);
			if (s1 & 1u) {
				continue;  // a publish is in flight
			}
			const auto index = g_index.load(std::memory_order_acquire);
			Config     out = g_buffers[index];
			const auto s2 = g_seq.load(std::memory_order_acquire);
			if (s1 == s2) {
				return out;
			}
		}
	}

	bool ParseSet(std::string_view a_key, std::string_view a_value, Config& a_cfg, std::string& a_out)
	{
		std::string_view section{};
		std::string_view name = a_key;
		const auto       colon = a_key.find(':');
		if (colon != std::string_view::npos) {
			section = a_key.substr(0, colon);
			name = a_key.substr(colon + 1);
		}
		if (name.empty()) {
			a_out = "empty key";
			return false;
		}

		const KeyEntry* found = nullptr;
		for (const auto& entry : kKeys) {
			if (!IEquals(entry.key, name)) {
				continue;
			}
			const bool wildcard = colon == std::string_view::npos || section.empty() || IEquals(section, "General");
			if (!wildcard && !IEquals(section, entry.section)) {
				continue;
			}
			if (found != nullptr) {
				a_out = "ambiguous key '" + std::string(name) + "'; qualify it as " +
					entry.section + ":" + entry.key;
				return false;
			}
			found = &entry;
		}
		if (found == nullptr) {
			a_out = "unknown key '" + std::string(a_key) + "' (see help)";
			return false;
		}

		char value[64]{};
		switch (found->kind) {
		case Kind::kBool:
			{
				bool parsed = false;
				if (!ParseBool(a_value, parsed)) {
					a_out = "not a boolean (use 0/1)";
					return false;
				}
				a_cfg.*(found->asBool) = parsed;
				std::snprintf(value, sizeof(value), "%d", parsed ? 1 : 0);
			}
			break;
		case Kind::kFloat:
			{
				float parsed = 0.0f;
				if (!ParseFloat(a_value, parsed)) {
					a_out = "not a float";
					return false;
				}
				a_cfg.*(found->asFloat) = parsed;
				std::snprintf(value, sizeof(value), "%.6g", static_cast<double>(parsed));
			}
			break;
		case Kind::kUInt:
			{
				std::uint32_t parsed = 0;
				if (!ParseUInt(a_value, parsed)) {
					a_out = "not an unsigned integer";
					return false;
				}
				a_cfg.*(found->asUInt) = parsed;
				std::snprintf(value, sizeof(value), "%u", parsed);
			}
			break;
		}

		a_out = std::string(found->section) + ":" + found->key + "=" + value;
		return true;
	}

	std::string SupportedKeys()
	{
		std::string out;
		for (const auto& entry : kKeys) {
			out += entry.section;
			out += ':';
			out += entry.key;
			out += (entry.kind == Kind::kBool ? " (bool)\n" :
				entry.kind == Kind::kFloat ? " (float)\n" :
											 " (uint)\n");
		}
		return out;
	}
}
