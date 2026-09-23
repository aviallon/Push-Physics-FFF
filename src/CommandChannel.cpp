#include "PCH.h"

#include "CommandChannel.h"

#include "CommandParse.h"
#include "Config.h"
#include "FrameClock.h"
#include "HkMath.h"
#include "LiveConfig.h"
#include "PhysicsMath.h"
#include "ProxyAccess.h"
#include "ProxyRegistry.h"
#include "PushListener.h"
#include "PushManager.h"
#include "PushModel.h"
#include "PushRequest.h"
#include "TraceChannel.h"
#include "WorldContactListener.h"

#include <RE/A/Actor.h>
#include <RE/H/hkpCharacterProxy.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/H/hkpShapePhantom.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace pa::CommandChannel
{
	namespace
	{
		constexpr std::uint64_t kPollIntervalMs = 100;  // ~10 Hz

		std::filesystem::path      g_cmdPath;
		bool                       g_firstObservation = true;
		std::uintmax_t             g_lastSize = 0;
		std::filesystem::file_time_type g_lastMtime{};
		std::uintmax_t             g_readOffset = 0;
		std::uint64_t              g_lastPollMs = 0;

		[[nodiscard]] std::string HexPtr(std::uintptr_t a_value)
		{
			char buf[24]{};
			std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(a_value));
			return buf;
		}

		[[nodiscard]] std::string HexId(std::uint32_t a_value)
		{
			char buf[16]{};
			std::snprintf(buf, sizeof(buf), "%08X", a_value);
			return buf;
		}

		[[nodiscard]] std::string Num(double a_value)
		{
			char buf[32]{};
			std::snprintf(buf, sizeof(buf), "%.3f", a_value);
			return buf;
		}

		[[nodiscard]] std::string Vec3Text(const float a_v[3])
		{
			char buf[64]{};
			std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)",
				static_cast<double>(a_v[0]), static_cast<double>(a_v[1]), static_cast<double>(a_v[2]));
			return buf;
		}

		[[nodiscard]] const char* SafeName(RE::TESObjectREFR* a_refr)
		{
			if (!a_refr) {
				return "";
			}
			const char* name = a_refr->GetDisplayFullName();
			return name ? name : "";
		}

		[[nodiscard]] std::string ProxyFlagsText(std::uint32_t a_flags)
		{
			static constexpr std::pair<std::uint32_t, const char*> kNames[] = {
				{ kProxyKnown, "known" },
				{ kProxyRagdoll, "ragdoll" },
				{ kProxyDead, "dead" },
				{ kProxyInCombat, "inCombat" },
				{ kProxySwimming, "swimming" },
				{ kProxyAirborne, "airborne" },
				{ kProxyKillMove, "killMove" },
				{ kProxyBleedout, "bleedout" },
				{ kProxyNotPushable, "notPushable" },
				{ kProxyNoCharacterCollisions, "noCharCollisions" },
				{ kProxyStaggering, "staggering" },
				{ kProxyPlayer, "player" },
			};
			std::string out;
			for (const auto& [bit, name] : kNames) {
				if (a_flags & bit) {
					if (!out.empty()) {
						out += '|';
					}
					out += name;
				}
			}
			return out.empty() ? "none" : out;
		}

		// --- vtable resolution ------------------------------------------------

		[[nodiscard]] std::uintptr_t ProxyVtable(std::size_t a_index)
		{
			static REL::Relocation<std::uintptr_t> v0{ RE::VTABLE_bhkCharProxyController[0] };
			static REL::Relocation<std::uintptr_t> v1{ RE::VTABLE_bhkCharProxyController[1] };
			return a_index == 0 ? v0.address() : v1.address();
		}

		[[nodiscard]] std::uintptr_t RigidVtable(std::size_t a_index)
		{
			static REL::Relocation<std::uintptr_t> v0{ RE::VTABLE_bhkCharRigidBodyController[0] };
			static REL::Relocation<std::uintptr_t> v1{ RE::VTABLE_bhkCharRigidBodyController[1] };
			return a_index == 0 ? v0.address() : v1.address();
		}

		[[nodiscard]] std::string ClassifyController(std::uintptr_t a_vptr)
		{
			if (a_vptr == 0) {
				return "none";
			}
			if (a_vptr == ProxyVtable(0)) {
				return "bhkCharProxyController[0] (listener)";
			}
			if (a_vptr == ProxyVtable(1)) {
				return "bhkCharProxyController[1] (controller)";
			}
			if (a_vptr == RigidVtable(0)) {
				return "bhkCharRigidBodyController[0]";
			}
			if (a_vptr == RigidVtable(1)) {
				return "bhkCharRigidBodyController[1]";
			}
			return "other";
		}

		// --- responses --------------------------------------------------------

		void Respond(std::string_view a_command, std::string_view a_body)
		{
			const auto path = LogDir() / "PushAside.out";
			std::FILE* f = std::fopen(path.string().c_str(), "ab");
			if (!f) {
				logger::warn("command channel: cannot append to {}", path.string());
				return;
			}
			char head[48]{};
			std::snprintf(head, sizeof(head), "[%llums] ", static_cast<unsigned long long>(FrameClock::NowMs()));
			std::fwrite(head, 1, std::strlen(head), f);
			std::fwrite(a_command.data(), 1, a_command.size(), f);
			std::fputc('\n', f);

			std::size_t start = 0;
			while (start < a_body.size()) {
				const auto nl = a_body.find('\n', start);
				const auto end = nl == std::string_view::npos ? a_body.size() : nl;
				if (end > start) {
					std::fwrite("  ", 1, 2, f);
					std::fwrite(a_body.data() + start, 1, end - start, f);
					std::fputc('\n', f);
				}
				if (nl == std::string_view::npos) {
					break;
				}
				start = nl + 1;
			}
			std::fclose(f);
		}

		// --- command bodies ---------------------------------------------------

		[[nodiscard]] std::string HelpBody()
		{
			return "help - this text\n"
				   "status - counters, registry size, config summary\n"
				   "registry - dump every registry entry (actor, proxy, controller, collidable, mass, flags)\n"
				   "bump - dump the live bump record and its full resolution chain\n"
				   "vtables - resolved bhkCharProxy/RigidBodyController vtable addresses\n"
				   "actors - ProcessLists high actors with controller ptr, vptr and class\n"
				   "watch <formID> - record that actor in every trace row\n"
				   "unwatch - stop watching\n"
				   "trace on|off|status|every <n> - control PushAside.trace\n"
				   "set - list live-settable config keys\n"
				   "set <Section>:<Key> <value> - change a config value live (General is a wildcard section)\n"
				   "push <formID> <dv> [ctrl|rb|both] - one explicit push on the physics thread\n"
				   "pushhere [dv] [ctrl|rb|both] - push whatever the bump record names\n";
		}

		[[nodiscard]] std::string StatusBody()
		{
			auto&       listener = *GetPushListener();
			const auto  contact = GetWorldContactListener().Snapshot();
			const auto& push = LastPushStatus();
			auto&       registry = ProxyRegistry::Get();

			std::string out;
			out += "registry: size=" + std::to_string(registry.Size()) +
				" capacity=" + std::to_string(registry.Capacity()) +
				" generation=" + std::to_string(registry.Generation()) + "\n";
			out += "config: " + Config::Get().Summary() + "\n";
			out += "listener: character=" + std::to_string(listener.CharacterCalls()) +
				" object=" + std::to_string(listener.ObjectCalls()) +
				" constraints=" + std::to_string(listener.ConstraintCalls()) +
				" orphans=" + std::to_string(OrphanCallCount()) + "\n";
			out += "world-contact: collisionAdded=" + std::to_string(contact.collisionAdded) +
				" pcActor=" + std::to_string(contact.pcActor) +
				" pcObject=" + std::to_string(contact.pcObject) +
				" actorActor=" + std::to_string(contact.actorActor) +
				" other=" + std::to_string(contact.other) +
				" bodyPhantom=" + std::to_string(contact.bodyPhantom) +
				" bodyPhantomPlayer=" + std::to_string(contact.bodyPhantomPlayer) +
				" pcGroup=" + std::to_string(contact.pcGroup) +
				" nullVsActor=" + std::to_string(contact.nullVsActor) + "\n";
			out += "bump: targets=" + std::to_string(BumpTargetCount()) +
				" pushes=" + std::to_string(BumpPushAppliedCount()) +
				" pairs=" + std::to_string(PushModel::PairCount()) + "\n";
			out += "push: active=" + std::to_string(push.active ? 1 : 0) +
				" mode=" + PushModeName(push.requestedMode) +
				" dv=" + Num(push.requestedDv) +
				" mech=" + PushMechanismName(push.mechanism) +
				" changed=" + std::to_string(push.changed ? 1 : 0) +
				" target=" + HexId(push.targetFormId) + "\n";
			out += "  ctrl from=" + Vec3Text(push.ctrlFrom) + " to=" + Vec3Text(push.ctrlTo) + "\n";
			out += "  rb   from=" + Vec3Text(push.rbFrom) + " to=" + Vec3Text(push.rbTo) + "\n";
			out += TraceChannel::StatusLine();
			return out;
		}

		[[nodiscard]] std::string RegistryBody()
		{
			std::string out;
			std::uint32_t index = 0;
			ProxyRegistry::Get().ForEachEntry([&](const ProxyEntry& entry) {
				out += "entry " + std::to_string(index++) +
					": actor=" + (entry.actor ? HexId(entry.actor->GetFormID()) : std::string("none")) +
					" '" + (entry.actor ? SafeName(entry.actor) : "") + "'" +
					" proxy=" + HexPtr(reinterpret_cast<std::uintptr_t>(entry.proxy)) +
					" ctrl=" + HexPtr(reinterpret_cast<std::uintptr_t>(entry.controller)) +
					" collidable=" + HexPtr(reinterpret_cast<std::uintptr_t>(entry.collidable)) +
					" mass=" + Num(entry.mass) +
					" flags=0x" + [&] {
						char b[16]{};
						std::snprintf(b, sizeof(b), "%X", entry.flags);
						return std::string(b);
					}() +
					" [" + ProxyFlagsText(entry.flags) + "]\n";
			});
			if (index == 0) {
				out = "registry is empty (no game loaded, or no character proxies yet)\n";
			}
			return out;
		}

		[[nodiscard]] std::string BumpBody()
		{
			const auto view = ReadBumpRecord();
			std::string out;
			out += "charBody=" + HexPtr(reinterpret_cast<std::uintptr_t>(view.charBody)) + "\n";
			out += "refr=" + (view.refr ? HexId(view.refr->GetFormID()) : std::string("none")) +
				" '" + SafeName(view.refr) + "'\n";
			out += "actor=" + (view.actor ? HexId(view.actor->GetFormID()) : std::string("none")) +
				" '" + (view.actor ? SafeName(view.actor) : "") + "' ptr=" +
				HexPtr(reinterpret_cast<std::uintptr_t>(view.actor)) + "\n";
			out += "ctrl=" + HexPtr(reinterpret_cast<std::uintptr_t>(view.ctrl)) + "\n";

			std::uintptr_t vptr = 0;
			if (view.ctrl) {
				vptr = *reinterpret_cast<const std::uintptr_t*>(view.ctrl);
			}
			out += "ctrl_vptr=" + HexPtr(vptr) + " class=" + ClassifyController(vptr) + "\n";
			out += "vtable_match: proxy[0]=" + std::to_string(vptr != 0 && vptr == ProxyVtable(0) ? 1 : 0) +
				" proxy[1]=" + std::to_string(vptr != 0 && vptr == ProxyVtable(1) ? 1 : 0) +
				" rigid[0]=" + std::to_string(vptr != 0 && vptr == RigidVtable(0) ? 1 : 0) +
				" rigid[1]=" + std::to_string(vptr != 0 && vptr == RigidVtable(1) ? 1 : 0) + "\n";
			out += "target_proxy=" + HexPtr(reinterpret_cast<std::uintptr_t>(view.target)) +
				" (resolved=" + std::to_string(view.target != nullptr ? 1 : 0) + ")\n";
			out += "bumpedForce=" + Num(view.force) + "\n";

			if (view.ctrl) {
				if (auto* rb = view.ctrl->GetRigidBody()) {
					const auto vel = ToVec3(rb->motion.linearVelocity);
					const auto pos = ToVec3(rb->motion.motionState.transform.translation);
					const auto type = rb->motion.type.get();
					const bool dynamic = type == RE::hkpMotion::MotionType::kDynamic ||
						type == RE::hkpMotion::MotionType::kSphereInertia ||
						type == RE::hkpMotion::MotionType::kBoxInertia ||
						type == RE::hkpMotion::MotionType::kThinBoxInertia;
					out += "rigidBody=" + HexPtr(reinterpret_cast<std::uintptr_t>(rb)) + "\n";
					out += "  velocity=(" + Num(vel.x) + ", " + Num(vel.y) + ", " + Num(vel.z) + ")\n";
					out += "  position=(" + Num(pos.x) + ", " + Num(pos.y) + ", " + Num(pos.z) + ")\n";
					out += "  motion_type=" + std::to_string(static_cast<int>(type)) +
						" (" + (dynamic ? "dynamic" : "keyframed/fixed") + ")\n";
					out += "  mass=" + Num(rb->motion.GetMass()) + "\n";
				} else {
					out += "rigidBody=none (the controller has no rigid body)\n";
				}
			} else {
				out += "rigidBody=none (no controller)\n";
			}
			return out;
		}

		[[nodiscard]] std::string VtablesBody()
		{
			std::string out;
			out += "bhkCharProxyController[0]=" + HexPtr(ProxyVtable(0)) + "\n";
			out += "bhkCharProxyController[1]=" + HexPtr(ProxyVtable(1)) + "\n";
			out += "bhkCharRigidBodyController[0]=" + HexPtr(RigidVtable(0)) + "\n";
			out += "bhkCharRigidBodyController[1]=" + HexPtr(RigidVtable(1)) + "\n";
			return out;
		}

		[[nodiscard]] std::string ActorsBody()
		{
			std::string out;
			auto*       lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return "no ProcessLists (no game loaded)\n";
			}
			std::uint32_t count = 0;
			for (std::uint32_t i = 0; i < lists->highActorHandles.size(); ++i) {
				auto refr = RE::TESObjectREFR::LookupByHandle(lists->highActorHandles[i].native_handle());
				auto* actor = refr ? refr->As<RE::Actor>() : nullptr;
				if (!actor) {
					continue;
				}
				auto*       ctrl = actor->GetCharController();
				const auto  vptr = ctrl ? *reinterpret_cast<const std::uintptr_t*>(ctrl) : 0;
				out += "actor " + std::to_string(count++) +
					": form=" + HexId(actor->GetFormID()) +
					" '" + SafeName(actor) + "'" +
					" ctrl=" + HexPtr(reinterpret_cast<std::uintptr_t>(ctrl)) +
					" vptr=" + HexPtr(vptr) +
					" class=" + ClassifyController(vptr) + "\n";
			}
			if (count == 0) {
				out = "no high actors (no game loaded, or none nearby)\n";
			}
			return out;
		}

		[[nodiscard]] std::string SetBody(const ParsedCommand& a_cmd)
		{
			if (a_cmd.arg1.empty()) {
				return std::string("supported keys:\n") + LiveConfig::SupportedKeys();
			}
			auto&       config = Config::Get();
			const bool  traceBefore = config.trace;
			std::string result;
			if (!LiveConfig::ParseSet(a_cmd.arg1, a_cmd.arg2, config, result)) {
				return "set: " + result + "\n";
			}
			LiveConfig::Publish(config);
			if (config.trace != traceBefore) {
				TraceChannel::SetEnabled(config.trace);
			}
			return "set " + result + "\nconfig: " + config.Summary() + "\n";
		}

		[[nodiscard]] std::string TraceBody(const ParsedCommand& a_cmd)
		{
			switch (a_cmd.traceAction) {
			case TraceAction::kOn:
				// Keep the config key and the module in sync, so `status` reports the
				// same state the trace is actually in.
				Config::Get().trace = true;
				LiveConfig::Publish(Config::Get());
				TraceChannel::SetEnabled(true);
				break;
			case TraceAction::kOff:
				Config::Get().trace = false;
				LiveConfig::Publish(Config::Get());
				TraceChannel::SetEnabled(false);
				break;
			case TraceAction::kEvery:
				TraceChannel::SetEvery(a_cmd.every);
				break;
			case TraceAction::kStatus:
				break;
			}
			return TraceChannel::StatusLine() + "\n";
		}

		// Shared by `push` and `pushhere`: resolve the shove axis on the main
		// thread, then publish the request for the physics thread.
		[[nodiscard]] bool PublishPushFor(RE::Actor* a_actor, RE::bhkCharacterController* a_ctrl,
			RE::hkpCharacterProxy* a_target, float a_dv, PushMode a_mode, std::string& a_out)
		{
			if (!a_ctrl) {
				a_out = "the target has no character controller";
				return false;
			}
			auto* playerProxy = PlayerProxy();
			if (!playerProxy || !playerProxy->shapePhantom) {
				a_out = "no player proxy yet (load a game first)";
				return false;
			}
			math::Vec3 targetPos{};
			if (a_target && a_target->shapePhantom) {
				targetPos = ToVec3(a_target->shapePhantom->motionState.transform.translation);
			} else if (a_ctrl) {
				if (auto* rb = a_ctrl->GetRigidBody()) {
					targetPos = ToVec3(rb->motion.motionState.transform.translation);
				}
			}
			const math::Vec3 playerPos = ToVec3(playerProxy->shapePhantom->motionState.transform.translation);

			math::Vec3 dir{};
			if (!math::ComputePushDirection(playerPos, targetPos, {}, dir)) {
				a_out = "degenerate direction (player and target coincide)";
				return false;
			}

			PushRequest request{};
			request.target = a_target;
			request.ctrl = a_ctrl;
			request.dir[0] = dir.x;
			request.dir[1] = dir.y;
			request.dir[2] = dir.z;
			request.dv = a_dv;
			request.mode = a_mode;
			request.targetFormId = a_actor ? a_actor->GetFormID() : 0;
			PublishPushRequest(request);

			a_out = "requested " + std::string(PushModeName(a_mode)) +
				" push dv=" + Num(a_dv) +
				" dir=" + Vec3Text(request.dir) +
				" target=" + (a_actor ? HexId(a_actor->GetFormID()) + " '" + SafeName(a_actor) + "'" : std::string("none")) +
				" (applied on the next physics step; result follows in this file)";
			return true;
		}

		[[nodiscard]] std::string PushBody(const ParsedCommand& a_cmd)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_cmd.formId);
			if (!actor) {
				return "push: no actor with formID " + HexId(a_cmd.formId) + "\n";
			}
			auto* ctrl = actor->GetCharController();
			if (!ctrl) {
				return "push: actor " + HexId(a_cmd.formId) + " has no character controller\n";
			}
			RE::hkpCharacterProxy* target = nullptr;
			if (auto* pc = AsProxyController(ctrl)) {
				target = pc->GetCharacterProxy();
			}
			std::string out;
			if (!PublishPushFor(actor, ctrl, target, a_cmd.dv, a_cmd.mode, out)) {
				return "push: " + out + "\n";
			}
			return out + "\n";
		}

		[[nodiscard]] std::string PushHereBody(const ParsedCommand& a_cmd)
		{
			const auto view = ReadBumpRecord();
			if (!view.actor && !view.ctrl) {
				return "pushhere: the bump record is empty (no recent player bump)\n";
			}
			std::string out;
			if (!PublishPushFor(view.actor, view.ctrl, view.target, a_cmd.dv, a_cmd.mode, out)) {
				return "pushhere: " + out + "\n";
			}
			return out + "\n";
		}

		[[nodiscard]] std::string WatchBody(const ParsedCommand& a_cmd)
		{
			TraceChannel::SetWatch(a_cmd.formId);
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_cmd.formId);
			return "watch " + HexId(a_cmd.formId) +
				(actor ? " '" + std::string(SafeName(actor)) + "'" : std::string(" (not found yet)")) +
				" (recorded in every trace row)\n";
		}

		[[nodiscard]] std::string PushResultBody(const PushResult& a_res)
		{
			std::string out;
			out += "push result: target=" + HexId(a_res.targetFormId) +
				" mode=" + PushModeName(a_res.mode) +
				" dv=" + Num(a_res.dv) +
				" mech=" + PushMechanismName(a_res.mechanism) +
				" changed=" + std::to_string(a_res.changed ? 1 : 0) +
				" frame=" + std::to_string(a_res.frame) + "\n";
			out += "  ctrl=" + HexPtr(a_res.ctrl) +
				" applied=" + std::to_string(a_res.ctrlApplied ? 1 : 0) +
				" from=" + Vec3Text(a_res.ctrlFrom) + " to=" + Vec3Text(a_res.ctrlTo) + "\n";
			out += "  rb=" + HexPtr(a_res.rb) +
				" applied=" + std::to_string(a_res.rbApplied ? 1 : 0) +
				" from=" + Vec3Text(a_res.rbFrom) + " to=" + Vec3Text(a_res.rbTo);
			return out;
		}

		void Execute(const ParsedCommand& a_cmd)
		{
			std::string body;
			switch (a_cmd.kind) {
			case CommandKind::kHelp:
				body = HelpBody();
				break;
			case CommandKind::kStatus:
				body = StatusBody();
				break;
			case CommandKind::kRegistry:
				body = RegistryBody();
				break;
			case CommandKind::kBump:
				body = BumpBody();
				break;
			case CommandKind::kVtables:
				body = VtablesBody();
				break;
			case CommandKind::kActors:
				body = ActorsBody();
				break;
			case CommandKind::kWatch:
				body = WatchBody(a_cmd);
				break;
			case CommandKind::kUnwatch:
				TraceChannel::SetWatch(0);
				body = "watch cleared\n";
				break;
			case CommandKind::kTrace:
				body = TraceBody(a_cmd);
				break;
			case CommandKind::kSet:
				body = SetBody(a_cmd);
				break;
			case CommandKind::kPush:
				body = PushBody(a_cmd);
				break;
			case CommandKind::kPushHere:
				body = PushHereBody(a_cmd);
				break;
			default:
				body = "not a command\n";
				break;
			}
			Respond(a_cmd.line, body);
		}
	}

	void Tick()
	{
		const auto now = FrameClock::NowMs();
		if (g_lastPollMs != 0 && now - g_lastPollMs < kPollIntervalMs) {
			return;
		}
		g_lastPollMs = now;

		// A push result is produced on the physics thread; report it here, on the
		// main thread, where file I/O and name resolution are allowed.
		PushResult result{};
		if (DrainPushResult(result)) {
			Respond("push result", PushResultBody(result));
		}

		if (g_cmdPath.empty()) {
			g_cmdPath = LogDir() / "PushAside.cmd";
		}

		std::error_code ec;
		if (!std::filesystem::exists(g_cmdPath, ec)) {
			return;  // a shipped install has no command file: nothing to do
		}
		const auto size = std::filesystem::file_size(g_cmdPath, ec);
		if (ec) {
			return;
		}
		const auto mtime = std::filesystem::last_write_time(g_cmdPath, ec);
		if (ec) {
			return;
		}

		// First sight of an existing file is treated as the baseline: commands that
		// were already there before this session are not replayed.
		if (g_firstObservation) {
			g_firstObservation = false;
			g_lastSize = size;
			g_lastMtime = mtime;
			g_readOffset = size;
			return;
		}

		if (size == g_lastSize && mtime == g_lastMtime) {
			return;
		}
		g_lastSize = size;
		g_lastMtime = mtime;
		if (size < g_readOffset) {
			g_readOffset = 0;  // truncated or rotated
		}

		std::ifstream in(g_cmdPath, std::ios::binary);
		if (!in) {
			return;
		}
		in.seekg(static_cast<std::streamoff>(g_readOffset));
		std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		std::size_t start = 0;
		for (;;) {
			const auto nl = chunk.find('\n', start);
			if (nl == std::string::npos) {
				break;  // partial line: leave it for the next poll
			}
			const std::string_view line(chunk.data() + start, nl - start);
			const auto             cmd = ParseCommandLine(line);
			if (cmd.kind != CommandKind::kNone) {
				if (!cmd.valid) {
					Respond(cmd.line, cmd.error);
				} else {
					Execute(cmd);
				}
			}
			start = nl + 1;
		}
		g_readOffset += start;
	}
}
