#include "PCH.h"

#include "TraceChannel.h"

#include "Config.h"
#include "FrameClock.h"
#include "HkMath.h"
#include "ProxyAccess.h"
#include "PushModel.h"
#include "PushRequest.h"
#include "TraceFormat.h"
#include "WorldContactListener.h"

#include <RE/A/Actor.h>
#include <RE/B/bhkCharacterController.h>
#include <RE/H/hkpCharacterProxy.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/H/hkpShapePhantom.h>
#include <RE/T/TESForm.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace pa::TraceChannel
{
	namespace
	{
		// Bound on the trace file. Reaching it stops the trace rather than letting
		// it fill the disk; the stop is written into the file and the log.
		constexpr std::uint64_t kMaxBytes = 64ull * 1024 * 1024;
		constexpr std::size_t   kRowBuffer = 2048;
		constexpr std::uint64_t kFlushMs = 1000;

		bool          g_inited = false;
		bool          g_enabled = false;
		bool          g_stopped = false;
		std::uint32_t g_every = 1;
		std::uint64_t g_tickCounter = 0;
		std::uint32_t g_watchForm = 0;
		std::FILE*    g_file = nullptr;
		std::uint64_t g_rows = 0;
		std::uint64_t g_bytes = 0;
		std::uint64_t g_lastFlushMs = 0;
		std::uint64_t g_session = 0;

		[[nodiscard]] std::filesystem::path TracePath()
		{
			return LogDir() / "PushAside.trace";
		}

		void WriteRaw(const char* a_data, int a_len)
		{
			if (!g_file || a_len <= 0) {
				return;
			}
			const auto written = std::fwrite(a_data, 1, static_cast<std::size_t>(a_len), g_file);
			g_bytes += written;
		}

		void Stop(const char* a_reason)
		{
			char buf[kRowBuffer]{};
			const int n = FormatTraceComment(a_reason, buf, sizeof(buf));
			WriteRaw(buf, n);
			if (g_file) {
				std::fflush(g_file);
				std::fclose(g_file);
				g_file = nullptr;
			}
			g_stopped = true;
			g_enabled = false;
			logger::warn("trace stopped: {} ({} bytes, {} rows)", a_reason, g_bytes, g_rows);
		}

		void Open()
		{
			const auto path = TracePath();
			std::error_code ec;
			const auto existing = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
			g_bytes = ec ? 0 : existing;

			g_file = std::fopen(path.string().c_str(), "ab");
			if (!g_file) {
				logger::error("trace: cannot open {} for append", path.string());
				g_enabled = false;
				return;
			}
			// One 64 KiB CRT buffer, allocated once (not per row).
			std::setvbuf(g_file, nullptr, _IOFBF, 1u << 16);

			if (g_bytes == 0) {
				WriteRaw(TraceHeader(), static_cast<int>(std::strlen(TraceHeader())));
				WriteRaw(TraceLegend(), static_cast<int>(std::strlen(TraceLegend())));
			}

			char buf[kRowBuffer]{};
			char comment[128]{};
			++g_session;
			std::snprintf(comment, sizeof(comment), "session %llu start at %llu ms (every %u frame(s), watch=%08X)",
				static_cast<unsigned long long>(g_session),
				static_cast<unsigned long long>(FrameClock::NowMs()),
				g_every, g_watchForm);
			WriteRaw(buf, FormatTraceComment(comment, buf, sizeof(buf)));
			g_lastFlushMs = FrameClock::NowMs();
			logger::info("trace: appending to {} ({} bytes so far)", path.string(), g_bytes);
		}

		void Close()
		{
			if (g_file) {
				std::fflush(g_file);
				std::fclose(g_file);
				g_file = nullptr;
			}
		}

		[[nodiscard]] bool IsDynamicMotion(RE::hkpMotion::MotionType a_type)
		{
			using MT = RE::hkpMotion::MotionType;
			switch (a_type) {
			case MT::kDynamic:
			case MT::kSphereInertia:
			case MT::kBoxInertia:
			case MT::kThinBoxInertia:
				return true;
			default:
				return false;
			}
		}

		void FillPhantomPosition(const RE::hkpCharacterProxy* a_proxy, float a_out[3])
		{
			if (!a_proxy || !a_proxy->shapePhantom) {
				return;
			}
			const auto p = ToVec3(a_proxy->shapePhantom->motionState.transform.translation);
			a_out[0] = p.x;
			a_out[1] = p.y;
			a_out[2] = p.z;
		}

		void FillWatch(TraceSample& a_sample)
		{
			a_sample.watchForm = g_watchForm;
			if (g_watchForm == 0) {
				return;
			}
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(g_watchForm);
			if (!actor) {
				return;
			}
			auto* ctrl = actor->GetCharController();
			if (!ctrl) {
				return;
			}
			RE::hkVector4 vel{};
			ctrl->GetLinearVelocityImpl(vel);
			const auto v = ToVec3(vel);
			a_sample.watchCtrlVel[0] = v.x;
			a_sample.watchCtrlVel[1] = v.y;
			a_sample.watchCtrlVel[2] = v.z;

			if (auto* pc = AsProxyController(ctrl)) {
				FillPhantomPosition(pc->GetCharacterProxy(), a_sample.watchPos);
			} else {
				// A rigid-body controller has no character proxy; use its body's
				// position instead.
				if (auto* rb = ctrl->GetRigidBody()) {
					const auto p = ToVec3(rb->motion.motionState.transform.translation);
					a_sample.watchPos[0] = p.x;
					a_sample.watchPos[1] = p.y;
					a_sample.watchPos[2] = p.z;
				}
			}
			if (auto* rb = ctrl->GetRigidBody()) {
				const auto rv = ToVec3(rb->motion.linearVelocity);
				a_sample.watchRbVel[0] = rv.x;
				a_sample.watchRbVel[1] = rv.y;
				a_sample.watchRbVel[2] = rv.z;
			}
		}

		void FillSample(TraceSample& s)
		{
			s.frame = FrameClock::CurrentFrame();
			s.ms = static_cast<double>(FrameClock::NowMs());

			if (auto* proxy = PlayerProxy()) {
				s.playerProxy = reinterpret_cast<std::uintptr_t>(proxy);
				FillPhantomPosition(proxy, s.playerPos);
				const auto v = ToVec3(proxy->velocity);
				s.playerVel[0] = v.x;
				s.playerVel[1] = v.y;
				s.playerVel[2] = v.z;
				s.playerMass = PushModel::PlayerEffectiveMass();
			}

			const auto bump = ReadBumpRecord();
			s.bumpCharBody = reinterpret_cast<std::uintptr_t>(bump.charBody);
			s.bumpRefr = bump.refr ? bump.refr->GetFormID() : 0;
			s.bumpActor = reinterpret_cast<std::uintptr_t>(bump.actor);
			s.bumpCtrl = reinterpret_cast<std::uintptr_t>(bump.ctrl);
			if (bump.ctrl) {
				s.bumpCtrlVptr = *reinterpret_cast<const std::uintptr_t*>(bump.ctrl);
				if (auto* rb = bump.ctrl->GetRigidBody()) {
					s.bumpRb = reinterpret_cast<std::uintptr_t>(rb);
					const auto rv = ToVec3(rb->motion.linearVelocity);
					s.bumpRbVel[0] = rv.x;
					s.bumpRbVel[1] = rv.y;
					s.bumpRbVel[2] = rv.z;
					const auto rp = ToVec3(rb->motion.motionState.transform.translation);
					s.bumpRbPos[0] = rp.x;
					s.bumpRbPos[1] = rp.y;
					s.bumpRbPos[2] = rp.z;
					const auto type = rb->motion.type.get();
					s.bumpRbMotion = static_cast<int>(type);
					s.bumpRbMass = rb->motion.GetMass();
					s.bumpRbDynamic = IsDynamicMotion(type) ? 1 : 0;
				}
			}

			const auto& push = LastPushStatus();
			if (push.active) {
				s.pushMode = static_cast<int>(push.requestedMode);
				s.pushDv = push.requestedDv;
				s.pushMech = push.mechanism;
				s.pushChanged = push.changed ? 1 : 0;
				for (int i = 0; i < 3; ++i) {
					s.pushCtrlFrom[i] = push.ctrlFrom[i];
					s.pushCtrlTo[i] = push.ctrlTo[i];
					s.pushRbFrom[i] = push.rbFrom[i];
					s.pushRbTo[i] = push.rbTo[i];
				}
			}

			FillWatch(s);
		}
	}

	void Tick()
	{
		if (!g_inited) {
			g_inited = true;
			g_enabled = Config::Get().trace;
		}
		if (!g_enabled || g_stopped) {
			return;
		}
		// Tick() is called once per frame, so this is a real every-Nth-frame gate
		// (not the millisecond key the model uses for its once-per-step guard).
		++g_tickCounter;
		if (g_every > 1 && (g_tickCounter % g_every) != 0) {
			return;
		}
		if (PlayerProxy() == nullptr) {
			return;  // no world yet: a row with everything zero is not evidence
		}
		if (!g_file) {
			Open();
			if (!g_file) {
				return;
			}
		}

		TraceSample sample{};
		FillSample(sample);

		char      buf[kRowBuffer]{};
		const int n = FormatTraceRow(sample, buf, sizeof(buf));
		if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
			logger::warn("trace: row did not fit the {} byte buffer; dropped", sizeof(buf));
			return;
		}
		if (g_bytes + static_cast<std::uint64_t>(n) > kMaxBytes) {
			Stop("reached the 64 MB cap; restart with 'trace on' after moving the file");
			return;
		}
		WriteRaw(buf, n);
		++g_rows;

		const auto now = FrameClock::NowMs();
		if (now - g_lastFlushMs >= kFlushMs) {
			g_lastFlushMs = now;
			std::fflush(g_file);
		}
	}

	void SetEnabled(bool a_enabled)
	{
		if (!g_inited) {
			g_inited = true;
			g_enabled = Config::Get().trace;
		}
		if (a_enabled == g_enabled && (a_enabled ? g_file != nullptr : true)) {
			return;
		}
		g_enabled = a_enabled;
		if (a_enabled) {
			g_stopped = false;
			Open();
		} else {
			Close();
		}
	}

	bool Enabled()
	{
		return g_enabled;
	}

	void SetEvery(std::uint32_t a_every)
	{
		g_every = a_every == 0 ? 1 : a_every;
	}

	std::uint32_t Every()
	{
		return g_every;
	}

	void SetWatch(std::uint32_t a_formId)
	{
		g_watchForm = a_formId;
	}

	std::uint32_t Watch()
	{
		return g_watchForm;
	}

	std::string StatusLine()
	{
		std::string out = "trace: ";
		out += g_enabled ? "on" : (g_stopped ? "stopped" : "off");
		out += " every=" + std::to_string(g_every);
		out += " rows=" + std::to_string(g_rows);
		out += " bytes=" + std::to_string(g_bytes);
		out += " watch=" + (g_watchForm == 0 ? std::string("none") : [](std::uint32_t id) {
			char b[16]{};
			std::snprintf(b, sizeof(b), "%08X", id);
			return std::string(b);
		}(g_watchForm));
		return out;
	}

	std::uint64_t RowsWritten()
	{
		return g_rows;
	}

	std::uint64_t BytesWritten()
	{
		return g_bytes;
	}
}
