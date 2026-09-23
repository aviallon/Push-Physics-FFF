// Off-game tests for PushAside's plain-C++ hook-verification infrastructure.
//
// The plugin itself is Windows-only, but src/Hooks/{HookTable,HookVerifier} are
// standard C++ with no Windows dependency, so they are built and RUN on Linux
// as well as Windows. That is the point: the parser and the verifier are
// exercised here rather than only on the game machine. No game binary and no
// Address Library are involved.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Hooks/HookTable.h"
#include "Hooks/HookTargets.h"
#include "Hooks/HookVerifier.h"
#include "BumpSlot.h"
#include "CommandParse.h"
#include "LiveConfig.h"
#include "PhysicsMath.h"
#include "TraceFormat.h"

#include <cmath>

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void Check(bool a_condition, const char* a_what)
	{
		++g_checks;
		if (!a_condition) {
			++g_failures;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	// A fake process: id -> RVA, plus a byte window at each RVA.
	class FakeResolver final : public pa::TargetResolver
	{
	public:
		std::uint64_t base = 0x140000000ull;
		std::uint64_t functionRva = 0x1000;
		std::uint64_t aeId = 1;
		std::vector<std::uint8_t> bytes;

		[[nodiscard]] std::uint64_t Base() const override { return base; }

		[[nodiscard]] bool ResolveId(std::uint64_t a_id, std::uint64_t& a_rvaOut) const override
		{
			if (a_id != aeId) {
				return false;
			}
			a_rvaOut = functionRva;
			return true;
		}

		[[nodiscard]] bool ReadBytes(std::uint64_t a_rva, std::uint8_t* a_out, std::size_t a_size) const override
		{
			if (a_rva != functionRva || a_size > bytes.size()) {
				return false;
			}
			std::memcpy(a_out, bytes.data(), a_size);
			return true;
		}
	};
}

int main()
{
	// The active list declares the frame-tail detour target that drives the
	// main-thread tick. The def, the committed table and the runtime check all
	// join on this display name, so pin it here. Main::Update itself is NOT a
	// target: HDT-SMP already detours its entry, so verifying its prologue would
	// make the plugin DEGRADE on every start.
	Check(pa::kHookTargetCount >= 1, "the hook-target list is not empty");
	{
		bool found = false;
		for (std::size_t i = 0; i < pa::kHookTargetCount; ++i) {
			const auto& target = pa::GetHookTarget(static_cast<pa::HookTargetId>(i));
			if (std::strcmp(target.name, "Main::Update frame-tail counter") == 0) {
				found = true;
				Check(target.kind == pa::HookKind::kRva, "the frame-tail target is an rva target");
				Check(target.aeId == 107306, "the frame-tail target AE id is 107306");
				Check(target.seId == 0, "the frame-tail target declares no SE id");
				Check(target.vtableId == 0 && target.vtableSlot == 0, "the frame-tail target carries no vtable id/slot");
			}
		}
		Check(found, "the frame-tail target is declared");
		for (std::size_t i = 0; i < pa::kHookTargetCount; ++i) {
			const auto& target = pa::GetHookTarget(static_cast<pa::HookTargetId>(i));
			Check(std::strcmp(target.name, "Main::Update") != 0,
				"Main::Update is not a hook target (its entry is detoured by HDT-SMP)");
		}
	}

	// FNV-1a 64 is pinned to known vectors so the C++ and the Python
	// implementation in tools/gen-hooktable.py cannot silently diverge.
	Check(pa::Fnv1a64(nullptr, 0) == 0xCBF29CE484222325ull, "FNV-1a 64 of empty input");
	Check(pa::Fnv1a64(reinterpret_cast<const std::uint8_t*>("abc"), 3) == 0xE71FA2190541574Bull, "FNV-1a 64 of \"abc\"");
	{
		std::uint8_t seq[8];
		for (int i = 0; i < 8; ++i) {
			seq[i] = static_cast<std::uint8_t>(i);
		}
		Check(pa::Fnv1a64(seq, sizeof(seq)) == 0xA4DC49E2B28ECB7Dull, "FNV-1a 64 of bytes 0..7");
		Check(pa::Fnv1a64(reinterpret_cast<const std::uint8_t*>("PushAside"), 9) == 0xCD5BD7DA23455D9Full, "FNV-1a 64 of \"PushAside\"");
	}

	// The parser rejects an empty target list: a verification table with
	// nothing in it is a configuration mistake, not a valid table.
	{
		pa::HookTable table;
		std::string error;
		const std::string json = R"({"schema":"pushaside.hooktable/1","identity":{"module":"SkyrimSE.exe","version":"1.7.104.0","size":1,"timeDateStamp":2,"sizeOfImage":3,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"},"targets":[]})";
		Check(!pa::ParseHookTable(json, table, error), "an empty targets array is rejected");
	}

	// A minimal one-record table parses, is findable by target name, and the
	// verifier accepts it against matching bytes.
	std::vector<std::uint8_t> prologue{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };
	const std::uint64_t hash = pa::Fnv1a64(prologue.data(), prologue.size());

	{
		pa::HookTable table;
		std::string error;
		const std::string json =
			R"({"schema":"pushaside.hooktable/1",)"
			R"("identity":{"module":"SkyrimSE.exe","version":"1.7.104.0","size":1,"timeDateStamp":2,"sizeOfImage":3,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"},)"
			R"("targets":[{"target":"TestTarget","kind":"rva","seId":0,"aeId":1,"name":"TestTarget","rva":4096,"pdataExtent":32,"slotLength":32,"prologueLength":8,"prologueHash":)" +
			std::to_string(hash) + R"(}]})";
		Check(pa::ParseHookTable(json, table, error), "a minimal one-record table parses");
		const auto* record = table.Find("TestTarget");
		Check(record != nullptr, "the record is findable by target name");
		if (record != nullptr) {
			Check(record->aeId == 1, "the parsed aeId is 1");
			Check(record->rva == 0x1000, "the parsed rva is 0x1000");

			FakeResolver resolver;
			resolver.bytes = prologue;
			const auto check = pa::VerifyHookTarget(*record, resolver);
			Check(check.Verified(), "matching bytes verify");
		}
	}

	// A single flipped byte must be reported as a prologue mismatch, not
	// silently accepted.
	{
		pa::HookTable table;
		std::string error;
		const std::string json =
			R"({"schema":"pushaside.hooktable/1",)"
			R"("identity":{"module":"SkyrimSE.exe","version":"1.7.104.0","size":1,"timeDateStamp":2,"sizeOfImage":3,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"},)"
			R"("targets":[{"target":"TestTarget","kind":"rva","seId":0,"aeId":1,"name":"TestTarget","rva":4096,"pdataExtent":32,"slotLength":32,"prologueLength":8,"prologueHash":)" +
			std::to_string(hash) + R"(}]})";
		if (pa::ParseHookTable(json, table, error)) {
			FakeResolver resolver;
			resolver.bytes = prologue;
			resolver.bytes[3] ^= 0xFF;
			const auto check = pa::VerifyHookTarget(*table.Find("TestTarget"), resolver);
			Check(check.verdict == pa::TargetVerdict::kPrologueMismatch, "one flipped byte is a prologue mismatch");
		} else {
			Check(false, "the mismatch fixture parsed");
		}
	}

	// --- pure push-model maths (design.md sec 4 and sec 5) --------------------
	using namespace pa::math;

	{
		Vec3 d;
		Check(ComputePushDirection({ 0, 0, 0 }, { 10, 0, 0 }, { 0, 1, 0 }, d) &&
				std::fabs(d.x - 1.0f) < 1e-5f && std::fabs(d.y) < 1e-5f,
			"direction is horizontalNormalize(p_o - p_s)");
		Check(ComputePushDirection({ 0, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 }, d) &&
				std::fabs(d.x) < 1e-5f && std::fabs(d.y - 1.0f) < 1e-5f,
			"coincident centres fall back to the contact normal");
		Check(!ComputePushDirection({ 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 5 }, d),
			"coincident centres + vertical normal is degenerate");
		Check(ComputePushDirection({ 0, 0, 0 }, { 5, 5, 120 }, { 0, 0, 1 }, d) &&
				std::fabs(d.z) < 1e-6f,
			"direction drops Z (horizontal push)");
	}

	{
		Check(std::fabs(ClosingSpeed({ 1, 0, 0 }, { 100, 0, 0 }, { -100, 0, 0 }, 20.0f) - 180.0f) < 1e-3f,
			"closing speed projects v_rel onto d and subtracts fMinRelSpeed");
		Check(ClosingSpeed({ 1, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, 20.0f) < 0.0f,
			"standing still is below fMinRelSpeed");
		Check(ClosingSpeed({ 1, 0, 0 }, { -100, 0, 0 }, { 100, 0, 0 }, 20.0f) < 0.0f,
			"walking away is refused");
	}

	{
		Check(std::fabs(MassRatio(100, 100, 2.0f) - 1.0f) < 1e-5f, "mu = 1 for equal masses");
		Check(std::fabs(MassRatio(100, 400, 2.0f) - 0.4f) < 1e-5f, "mu = 0.4 for a 4x heavier target");
		Check(std::fabs(MassRatio(100, 100, 0.5f) - 0.5f) < 1e-5f, "mu is clamped by fMassRatioMax");
	}

	{
		Check(HeavyGate(400, 100, 30.0f, 2.5f, 60.0f, 0.25f).refuse, "heavy + slow is refused");
		const auto heavy = HeavyGate(400, 100, 100.0f, 2.5f, 60.0f, 0.25f);
		Check(!heavy.refuse && std::fabs(heavy.scale - 0.25f) < 1e-5f, "heavy + fast is scaled by fHeavyScale");
		const auto notHeavy = HeavyGate(200, 100, 10.0f, 2.5f, 60.0f, 0.25f);
		Check(!notHeavy.refuse && std::fabs(notHeavy.scale - 1.0f) < 1e-5f, "below fHeavyMassRatio is not gated");
	}

	{
		Check(std::fabs(CapDeltaV(400.0f, 250.0f) - 250.0f) < 1e-5f, "dv is capped by fMaxDeltaV");
		Check(std::fabs(CapDeltaV(100.0f, 250.0f) - 100.0f) < 1e-5f, "dv below the cap is unchanged");
		Check(std::fabs(ApplyStateScales(100.0f, true, false, false, true, 0.5f, 1.0f, 0.35f, 0.5f, 0.5f) - 50.0f) < 1e-4f,
			"in-combat applies fCombatScale");
		Check(std::fabs(ApplyStateScales(100.0f, false, true, true, false, 0.5f, 1.0f, 0.35f, 0.5f, 0.5f) - 100.0f * 0.35f * 0.5f * 0.5f) < 1e-3f,
			"swim + airborne + unknown multiply together");
	}

	{
		// Cooldown, take-max, once-per-frame, stagger cooldown and expiry, using
		// explicit now/frame values so the result is deterministic.
		BufferParams p;
		p.cooldownMs = 200.0f;
		p.maxPushDurationMs = 600.0f;
		p.maxInjectedSpeed = 350.0f;
		p.staggerCooldownMs = 1500.0f;
		BufferState s;
		const Vec3  dir{ 1, 0, 0 };

		const auto u1 = UpdateBuffer(s, dir, 100.0f, 2000, 1, p, true, 50.0f);
		Check(u1.renewed, "first contact with no prior push is renewed");
		Check(u1.applyNow, "first contact in a frame applies");
		Check(u1.stagger, "dv above threshold with stagger allowed queues a stagger");
		Check(std::fabs(Length3(u1.velocity) - 100.0f) < 1e-4f, "buffered velocity is d * dv");

		const auto u2 = UpdateBuffer(s, dir, 50.0f, 2010, 2, p, true, 50.0f);
		Check(!u2.renewed, "a second contact inside the cooldown is not renewed");
		Check(std::fabs(Length3(u2.velocity) - 100.0f) < 1e-4f, "take-max keeps the larger velocity, never adds");
		Check(!u2.stagger, "stagger cooldown blocks a second stagger");

		// Same frame as u2, larger dv: take-max raises the value but must not apply
		// a second time in this frame.
		const auto u3 = UpdateBuffer(s, dir, 600.0f, 2020, 2, p, true, 50.0f);
		Check(!u3.applyNow, "a manifold's second contact in the same frame does not re-apply");
		Check(std::fabs(Length3(u3.velocity) - 350.0f) < 1e-4f, "velocity is clamped by fMaxInjectedSpeed");

		const auto u4 = UpdateBuffer(s, dir, 10.0f, 2030, 3, p, true, 50.0f);
		Check(u4.applyNow, "the next frame applies again");
		Check(std::fabs(Length3(u4.velocity) - 350.0f) < 1e-4f, "take-max keeps the injected value");

		const auto u5 = UpdateBuffer(s, dir, 100.0f, 4000, 4, p, true, 50.0f);
		Check(u5.renewed, "the cooldown eventually elapses");
		Check(u5.stagger, "the stagger cooldown eventually elapses");

		Check(!IsExpired(s, 4599), "an entry is live before fMaxPushDurationMs");
		Check(IsExpired(s, 4600), "an entry expires at fMaxPushDurationMs");
		Check(std::fabs(Length3(DampVelocity({ 100, 0, 0 }, 6.0f, 0.5f)) - 100.0f * std::exp(-3.0f)) < 1e-3f,
			"damping is exponential at fPushDamping per second");
	}

	{
		// Gate refusals: each negative case must name the matching gate, so a guard
		// that never fired is visible.
		const auto gate = [](auto a_mutate) {
			GateInputs in;
			a_mutate(in);
			return EvaluateGates(in);
		};
		Check(gate([](GateInputs& i) { i.enabled = false; }) == GateRefusal::kMasterOff, "gate 1: master off");
		Check(gate([](GateInputs& i) { i.useCharacterInteraction = false; }) == GateRefusal::kNoCharacterInteraction, "gate 1: Path-1 off");
		Check(gate([](GateInputs& i) { i.dialogueOpen = true; }) == GateRefusal::kDialogue, "gate 15: dialogue");
		Check(gate([](GateInputs& i) { i.ragdoll = true; }) == GateRefusal::kRagdoll, "gate 10: ragdoll");
		Check(gate([](GateInputs& i) { i.dead = true; }) == GateRefusal::kDead, "gate 11: dead");
		Check(gate([](GateInputs& i) { i.notPushable = true; }) == GateRefusal::kNotPushable, "gate 12: not pushable");
		Check(gate([](GateInputs& i) { i.noCharacterCollisions = true; }) == GateRefusal::kNoCharacterCollisions, "gate 12: no character collisions");
		Check(gate([](GateInputs& i) { i.killMove = true; }) == GateRefusal::kKillMove, "gate 13: kill move");
		Check(gate([](GateInputs& i) { i.bleedout = true; }) == GateRefusal::kBleedout, "gate 13: bleedout");
		Check(gate([](GateInputs& i) { i.disableInCombat = true; i.inCombat = true; }) == GateRefusal::kDisableInCombat, "gate 19: disable in combat");
		Check(gate([](GateInputs& i) { i.disableWhileSwimming = true; i.swimming = true; }) == GateRefusal::kDisableWhileSwimming, "gate 19: disable while swimming");
		Check(gate([](GateInputs& i) { i.budgetExceeded = true; }) == GateRefusal::kBudgetExceeded, "gate 18: budget");
		Check(gate([](GateInputs&) {}) == GateRefusal::kNone, "all gates pass on a healthy target");
		Check(std::strcmp(GateRefusalName(GateRefusal::kRagdoll), "target is ragdolled") == 0, "gate names are stable");
	}

	// --- character-strength derivation (Approach A) ---------------------------
	{
		using namespace pa::math;
		StrengthInputs in;  // defaults: base 5000, raceBaseMass 1.0, refMass 1.0, level 1, no skills

		// The three fitted anchors: L1/skill0.15 -> 5000, L50/skill0.80 -> 30000,
		// L252/skill1.00 -> ~133000 (humanoid raceBaseMass = referenceMass = 1.0).
		// Each skill set to the same value makes the weighted physical-skill mean
		// that value.
		const auto at = [](float a_level, float a_skill) {
			StrengthInputs s;
			s.level = a_level;
			s.oneHanded = a_skill;
			s.twoHanded = a_skill;
			s.block = a_skill;
			s.heavyArmor = a_skill;
			s.archery = a_skill;
			return DeriveCharacterStrength(s);
		};
		Check(std::fabs(at(1.0f, 15.0f) - 5000.0f) / 5000.0f < 0.01f, "anchor L1/skill0.15 -> 5000 (within 1%)");
		Check(std::fabs(at(50.0f, 80.0f) - 30000.0f) / 30000.0f < 0.01f, "anchor L50/skill0.80 -> 30000 (within 1%)");
		Check(std::fabs(at(252.0f, 100.0f) - 133000.0f) / 133000.0f < 0.01f, "anchor L252/skill1.00 -> ~133000 (within 1%)");

		// In-game regression (2026-09): a humanoid race has baseMass = 1.0, so with
		// the old fStrengthReferenceMass = 80 the factor was 1/80 and the observed
		// L7/P~1.418 derivation clamped to the 500 floor. With referenceMass = 1.0
		// the same inputs yield ~7090.
		StrengthInputs l7;
		l7.level = 7.0f;
		l7.oneHanded = l7.twoHanded = l7.block = l7.heavyArmor = l7.archery = 23.0f;
		Check(std::fabs(DeriveCharacterStrength(l7) - 7090.0f) / 7090.0f < 0.02f,
			"regression: humanoid race baseMass 1.0 at L7 derives ~7090, not the 500 floor");

		// Race scaling is linear in raceBaseMass / referenceMass. baseMass is the
		// engine's RELATIVE race multiplier, so 2.0 is a giant-scale race.
		StrengthInputs race1 = in;
		race1.oneHanded = race1.twoHanded = race1.block = race1.heavyArmor = race1.archery = 80.0f;
		race1.level = 50.0f;
		StrengthInputs race2 = race1;
		race2.raceBaseMass = 2.0f;
		Check(std::fabs(DeriveCharacterStrength(race2) - 2.0f * DeriveCharacterStrength(race1)) < 1e-2f,
			"race base mass scales strength linearly");

		// Monotonic in level (the fitted level term is 1 + gain * (level-1)).
		Check(at(1.0f, 80.0f) <= at(50.0f, 80.0f), "strength is monotonic in level (1 -> 50)");
		Check(at(50.0f, 80.0f) <= at(252.0f, 80.0f), "strength is monotonic in level (50 -> 252)");

		// Monotonic in the physical skills.
		Check(at(50.0f, 40.0f) < at(50.0f, 80.0f), "strength is monotonic in the physical-skill mean");

		// Clamped at both ends.
		StrengthInputs tiny = in;
		tiny.base = 1.0f;
		Check(std::fabs(DeriveCharacterStrength(tiny) - 500.0f) < 1e-3f, "derived strength clamps at fStrengthMin");
		StrengthInputs huge = in;
		huge.base = 1.0e9f;
		Check(std::fabs(DeriveCharacterStrength(huge) - 200000.0f) < 1e-3f, "derived strength clamps at fStrengthMax");

		// Explicit override is verbatim and wins; -1 derives instead of writing the
		// sentinel.
		Check(std::fabs(ResolveCharacterStrength(12345.0f, in) - 12345.0f) < 1e-3f,
			">= 0 fCharacterStrength is an explicit override");
		Check(std::fabs(ResolveCharacterStrength(-1.0f, in) - DeriveCharacterStrength(in)) < 1e-3f,
			"fCharacterStrength = -1 derives");
		Check(ResolveCharacterStrength(-1.0f, in) > 0.0f,
			"fCharacterStrength = -1 never writes the -1 sentinel");

		// Effective mass: fPlayerMass * P^fMassPowerExponent.
		Check(std::fabs(EffectivePlayerMass(100.0f, 1.0f, 0.7f) - 100.0f) < 1e-3f, "effective mass at P=1 is fPlayerMass");
		const float p50 = DeriveCharacterStrength(race1) / 5000.0f;  // P at L50/skill0.80
		Check(std::fabs(p50 - 6.0f) < 0.06f, "P at L50/skill0.80 is 6");
		Check(std::fabs(EffectivePlayerMass(100.0f, p50, 0.7f) - 100.0f * std::pow(p50, 0.7f)) < 1e-3f,
			"effective mass follows fPlayerMass * P^exponent");
		Check(EffectivePlayerMass(100.0f, p50, 0.7f) > 300.0f, "effective mass at L50/skill0.80 exceeds 300");
	}

	// --- bump-detection single-slot handoff (src/BumpSlot.h) -------------------
	// The value the physics thread takes is the only thing it may apply, so these
	// pin the exact semantics used by PushListener::ProcessConstraintsCallback:
	// latest-wins, exchange clears, an empty or null slot is never applied.
	{
		pa::SingleSlot<int> slot;
		int                first = 1;
		int                second = 2;
		int*               out = nullptr;

		Check(!pa::TakeForApply(slot, out), "an empty slot does not apply");
		Check(out == nullptr, "an empty consume yields nullptr");

		slot.Publish(&first);
		Check(slot.Peek() == &first, "a published value is visible before the consumer runs");
		Check(pa::TakeForApply(slot, out) && out == &first, "a published value applies exactly once");
		Check(slot.Peek() == nullptr, "exchange clears the slot");
		Check(!pa::TakeForApply(slot, out) && out == nullptr, "the same value cannot be applied twice");

		// Latest wins: an unconsumed value is replaced, never queued.
		slot.Publish(&first);
		slot.Publish(&second);
		Check(slot.Peek() == &second, "latest publish wins (the older value is dropped)");
		Check(pa::TakeForApply(slot, out) && out == &second, "the latest value is the one applied");

		// A null publish clears, and a null in the slot is consumed and dropped.
		slot.Publish(&first);
		slot.Publish(nullptr);
		Check(slot.Peek() == nullptr, "a null publish clears the slot");
		Check(!pa::TakeForApply(slot, out) && out == nullptr, "a null in the slot is never applied");

		slot.Publish(&first);
		slot.Clear();
		Check(slot.Peek() == nullptr, "Clear() empties the slot");
		Check(!pa::TakeForApply(slot, out) && out == nullptr, "a cleared slot does not apply");
	}

	// --- command-channel parser (src/CommandParse.h) --------------------------
	// The command file is the only input channel; a line that parses wrongly is a
	// command that silently does nothing, so every accepted and rejected form is
	// pinned here.
	{
		using pa::CommandKind;
		using pa::TraceAction;

		Check(pa::ParseCommandLine("  help  ").kind == CommandKind::kHelp, "help parses");
		Check(pa::ParseCommandLine("help").valid, "help is valid");
		Check(pa::ParseCommandLine("# a comment").kind == CommandKind::kNone, "a comment is ignored");
		Check(pa::ParseCommandLine("   ").kind == CommandKind::kNone, "a blank line is ignored");
		Check(pa::ParseCommandLine("\t\r").kind == CommandKind::kNone, "whitespace/CR is ignored");
		Check(pa::ParseCommandLine("status").kind == CommandKind::kStatus, "status parses");
		Check(pa::ParseCommandLine("registry").kind == CommandKind::kRegistry, "registry parses");
		Check(pa::ParseCommandLine("bump").kind == CommandKind::kBump, "bump parses");
		Check(pa::ParseCommandLine("vtables").kind == CommandKind::kVtables, "vtables parses");
		Check(pa::ParseCommandLine("actors").kind == CommandKind::kActors, "actors parses");
		Check(pa::ParseCommandLine("HELP").kind == CommandKind::kHelp, "commands are case-insensitive");

		const auto unknown = pa::ParseCommandLine("bogus");
		Check(unknown.kind == CommandKind::kUnknown && !unknown.valid, "an unknown command is rejected");

		const auto watch = pa::ParseCommandLine("watch 0x0001A2B3");
		Check(watch.kind == CommandKind::kWatch && watch.valid && watch.formId == 0x0001A2B3,
			"watch takes a hex formID");
		Check(!pa::ParseCommandLine("watch").valid, "watch without a formID is malformed");
		Check(!pa::ParseCommandLine("watch zzz").valid, "watch with a non-number is malformed");
		Check(pa::ParseCommandLine("unwatch").kind == CommandKind::kUnwatch, "unwatch parses");

		const auto on = pa::ParseCommandLine("trace on");
		Check(on.kind == CommandKind::kTrace && on.traceAction == TraceAction::kOn, "trace on parses");
		Check(pa::ParseCommandLine("trace off").traceAction == TraceAction::kOff, "trace off parses");
		Check(pa::ParseCommandLine("trace status").traceAction == TraceAction::kStatus, "trace status parses");
		Check(pa::ParseCommandLine("trace").traceAction == TraceAction::kStatus, "bare trace is status");
		const auto every = pa::ParseCommandLine("trace every 5");
		Check(every.traceAction == TraceAction::kEvery && every.every == 5, "trace every <n> parses");
		Check(!pa::ParseCommandLine("trace every 0").valid, "trace every 0 is rejected");
		Check(!pa::ParseCommandLine("trace every -1").valid, "trace every -1 is rejected");
		Check(!pa::ParseCommandLine("trace every").valid, "trace every without n is rejected");
		Check(!pa::ParseCommandLine("trace sideways").valid, "trace with a bad action is rejected");

		const auto set = pa::ParseCommandLine("set Physics:fPushScale 2.5");
		Check(set.kind == CommandKind::kSet && set.valid && set.arg1 == "Physics:fPushScale" && set.arg2 == "2.5",
			"set takes <Section>:<Key> and a value");
		Check(pa::ParseCommandLine("set").kind == CommandKind::kSet && pa::ParseCommandLine("set").valid,
			"bare set is valid (lists the keys)");
		Check(!pa::ParseCommandLine("set Physics:fPushScale").valid, "set without a value is malformed");

		const auto push = pa::ParseCommandLine("push 0x14 250 rb");
		Check(push.kind == CommandKind::kPush && push.valid && push.formId == 0x14 &&
				std::fabs(push.dv - 250.0f) < 1e-3f && push.mode == pa::PushMode::kRb,
			"push takes formID, dv and mode");
		Check(pa::ParseCommandLine("push 0x14 250").mode == pa::PushMode::kBoth, "push defaults to both");
		Check(!pa::ParseCommandLine("push 0x14").valid, "push without dv is malformed");
		Check(!pa::ParseCommandLine("push 0x14 250 sideways").valid, "push with a bad mode is malformed");
		Check(!pa::ParseCommandLine("push zzz 250").valid, "push with a bad formID is malformed");

		const auto here = pa::ParseCommandLine("pushhere");
		Check(here.kind == CommandKind::kPushHere && here.valid && here.dv > 0.0f,
			"pushhere with no arguments gets a default dv");
		Check(pa::ParseCommandLine("pushhere 300").dv == 300.0f, "pushhere takes a dv");
		Check(pa::ParseCommandLine("pushhere ctrl").mode == pa::PushMode::kCtrl, "pushhere takes a mode");
		Check(pa::ParseCommandLine("pushhere 300 both").mode == pa::PushMode::kBoth,
			"pushhere takes dv and mode");
		Check(!pa::ParseCommandLine("pushhere zzz").valid, "pushhere with garbage is malformed");

		std::uint32_t id = 0;
		Check(pa::ParseFormId("0x14", id) && id == 20, "ParseFormId reads hex");
		Check(pa::ParseFormId("20", id) && id == 20, "ParseFormId reads decimal");
		Check(pa::ParseFormId("010", id) && id == 10, "ParseFormId does not read a leading zero as octal");
		Check(!pa::ParseFormId("0x", id), "ParseFormId rejects a bare 0x");
		Check(!pa::ParseFormId("", id), "ParseFormId rejects an empty string");
		Check(!pa::ParseFormId("-1", id), "ParseFormId rejects a sign (unsigned literal)");

		Check(std::strcmp(pa::PushModeName(pa::PushMode::kCtrl), "ctrl") == 0, "PushModeName ctrl");
		Check(std::strcmp(pa::PushModeName(pa::PushMode::kRb), "rb") == 0, "PushModeName rb");
		Check(std::strcmp(pa::PushModeName(pa::PushMode::kBoth), "both") == 0, "PushModeName both");
		Check(std::strcmp(pa::PushMechanismName(0), "none") == 0, "PushMechanismName none");
		Check(std::strcmp(pa::PushMechanismName(3), "both") == 0, "PushMechanismName both");
		Check(std::strcmp(pa::PushMechanismName(2), "rb") == 0, "PushMechanismName rb");
	}

	// --- live-config override (src/LiveConfig.h) ------------------------------
	{
		pa::Config  config;
		std::string out;

		Check(pa::LiveConfig::ParseSet("Listener:bUseBumpDetection", "0", config, out) && !config.useBumpDetection,
			"ParseSet applies a bool");
		Check(out == "Listener:bUseBumpDetection=0", "ParseSet normalises the result line");
		Check(pa::LiveConfig::ParseSet("General:bEnabled", "false", config, out) && !config.enabled,
			"General is a wildcard section");
		Check(pa::LiveConfig::ParseSet("Physics:fPushScale", "2.5", config, out) &&
				std::fabs(config.pushScale - 2.5f) < 1e-6f,
			"ParseSet applies a float");
		Check(pa::LiveConfig::ParseSet("General:fPushScale", "3", config, out) &&
				std::fabs(config.pushScale - 3.0f) < 1e-6f,
			"General:fPushScale resolves to the Physics key");
		Check(pa::LiveConfig::ParseSet("Budget:uMaxInteractionsPerFrame", "64", config, out) &&
				config.maxInteractionsPerFrame == 64,
			"ParseSet applies a uint");
		Check(pa::LiveConfig::ParseSet("Diagnostics:bTrace", "1", config, out) && config.trace,
			"bTrace is a live-settable key");

		Check(!pa::LiveConfig::ParseSet("Physics:bUseBumpDetection", "1", config, out),
			"a key in the wrong section is rejected");
		Check(!pa::LiveConfig::ParseSet("Nope:nope", "1", config, out), "an unknown key is rejected");
		Check(!pa::LiveConfig::ParseSet("Listener:bUseBumpDetection", "maybe", config, out),
			"a malformed bool is rejected");
		Check(!pa::LiveConfig::ParseSet("Physics:fPushScale", "abc", config, out),
			"a malformed float is rejected");
		Check(!pa::LiveConfig::ParseSet("Budget:uMaxInteractionsPerFrame", "-1", config, out),
			"a negative uint is rejected");
		const float before = config.pushScale;
		Check(!pa::LiveConfig::ParseSet("Physics:fPushScale", "abc", config, out) && config.pushScale == before,
			"a failed parse leaves the value unchanged");
		Check(pa::LiveConfig::SupportedKeys().find("Listener:bUseBumpDetection") != std::string::npos,
			"SupportedKeys lists the documented keys");

		// The seqlock snapshot the physics callback reads.
		pa::Config published;
		published.pushScale = 7.25f;
		published.enabled = false;
		published.maxInteractionsPerFrame = 7;
		pa::LiveConfig::Publish(published);
		const auto snapshot = pa::LiveConfig::Snapshot();
		Check(std::fabs(snapshot.pushScale - 7.25f) < 1e-6f, "Publish/Snapshot round-trips a float");
		Check(!snapshot.enabled, "Publish/Snapshot round-trips a bool");
		Check(snapshot.maxInteractionsPerFrame == 7, "Publish/Snapshot round-trips a uint");
		pa::Config second = published;
		second.pushScale = 1.0f;
		pa::LiveConfig::Publish(second);
		Check(std::fabs(pa::LiveConfig::Snapshot().pushScale - 1.0f) < 1e-6f, "a later publish wins");
	}

	// --- trace row format (src/TraceFormat.h) ---------------------------------
	{
		const auto countColumns = [](std::string_view a_line) {
			std::size_t columns = 1;
			for (const char c : a_line) {
				if (c == ',') {
					++columns;
				}
			}
			return columns;
		};

		const std::string_view header{ pa::TraceHeader() };
		Check(!header.empty() && header.back() == '\n', "the trace header ends with a newline");
		Check(countColumns(header.substr(0, header.size() - 1)) == 51, "the trace header has 51 columns");
		Check(std::strstr(pa::TraceHeader(), "player_proxy") != nullptr, "the header names the player proxy");
		Check(std::strstr(pa::TraceHeader(), "push_mech") != nullptr, "the header names the push mechanism");
		Check(std::strstr(pa::TraceHeader(), "watch_ctrl_vx") != nullptr, "the header names the watched actor");
		Check(!std::string_view{ pa::TraceLegend() }.empty(), "a legend is available");

		pa::TraceSample sample;
		sample.frame = 12345;
		sample.ms = 678.5;
		sample.playerProxy = 0xABCDEF;
		sample.playerPos[0] = 1.0f;
		sample.playerPos[1] = 2.0f;
		sample.playerPos[2] = 3.0f;
		sample.bumpRbMotion = 4;
		sample.pushMode = 3;
		sample.pushMech = 3;
		sample.pushChanged = 1;
		sample.watchForm = 0x0001A2B3;

		char      buffer[2048]{};
		const int n = pa::FormatTraceRow(sample, buffer, sizeof(buffer));
		Check(n > 0 && static_cast<std::size_t>(n) < sizeof(buffer), "a trace row fits the stack buffer");
		Check(static_cast<std::size_t>(n) == std::strlen(buffer), "the reported row length matches the text");
		Check(std::string_view{ buffer, static_cast<std::size_t>(n - 1) }.rfind("12345,", 0) == 0,
			"the row starts with the frame number");
		Check(countColumns(std::string_view{ buffer, static_cast<std::size_t>(n - 1) }) == 51,
			"a trace row has the same 51 columns as the header");
		Check(std::string_view{ buffer }.find("ABCDEF") != std::string_view::npos,
			"the player proxy pointer is written in hex");

		char      tiny[8]{};
		const int truncated = pa::FormatTraceRow(sample, tiny, sizeof(tiny));
		Check(truncated >= static_cast<int>(sizeof(tiny)), "a too-small buffer reports truncation");

		char      comment[64]{};
		const int cn = pa::FormatTraceComment("hello", comment, sizeof(comment));
		Check(cn > 0 && std::strcmp(comment, "# hello\n") == 0, "a comment line is formatted");
	}

	std::printf("%d check(s) run, %d failure(s)\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
