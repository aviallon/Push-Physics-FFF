#include "PCH.h"

#include "Hooks/FrameTickHook.h"

#include "Health.h"
#include "Hooks/HookTable.h"
#include "Hooks/HookTableData.gen.h"
#include "Hooks/HookTargets.h"
#include "Hooks/HookVerifier.h"
#include "ProxyRegistry.h"

#include <MinHook.h>

#include <cstring>
#include <string>
#include <vector>

namespace pa
{
	namespace
	{
		// The function Main::Update calls as its very last instruction before the
		// epilogue: `call 0x14154AF70` at Main::Update+0xC49 (SkyrimSE.exe
		// 1.7.104). Disassembly of that leaf (RVA 0x154AF70) is exactly
		// `incl 0xc8(%rcx); ret` - one pointer in RCX, no return value - so this
		// plain function pointer is ABI-compatible.
		//
		// A .text-wide scan for the E8 rel32 to 0x14154AF70 finds exactly one
		// caller (Main::Update+0xC49, in the unconditional tail after the
		// early-exit branch), so the entry detour runs once per frame. It patches
		// the leaf, not Main::Update, so HDT-SMP's Main::Update entry detour,
		// CommunityShaders' +0x160 call-site patch and SKSE's +0x9A dispatch are
		// all untouched.
		using FrameTailFn = void (*)(void*);

		FrameTailFn    g_original = nullptr;
		std::uintptr_t g_targetAddress = 0;
		bool           g_installed = false;
		bool           g_inTick = false;  // main thread only: never re-enter the tick

		// --- committed-table verification (same shape as HeapSentinel) -------

		[[nodiscard]] std::uint64_t ModuleImageSize()
		{
			const auto   base = REL::Module::get().base();
			const auto*  dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto*  nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			return nt->OptionalHeader.SizeOfImage;
		}

		[[nodiscard]] ModuleIdentity ReadModuleIdentity()
		{
			ModuleIdentity identity;
			identity.sizeOfImage = ModuleImageSize();

			const auto   base = REL::Module::get().base();
			const auto*  dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto*  nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			identity.timeDateStamp = nt->FileHeader.TimeDateStamp;

			char path[MAX_PATH]{};
			if (::GetModuleFileNameA(nullptr, path, MAX_PATH) != 0) {
				WIN32_FILE_ATTRIBUTE_DATA data{};
				if (::GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
					identity.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
				}
			}
			return identity;
		}

		// The process as the verifier sees it: Address Library ids resolve through
		// REL, and reads are bounded by SizeOfImage and the page's state, so a bad
		// id cannot turn the verifier into the fault.
		class EngineResolver final : public TargetResolver
		{
		public:
			[[nodiscard]] std::uint64_t Base() const override { return REL::Module::get().base(); }

			[[nodiscard]] bool ResolveId(std::uint64_t a_id, std::uint64_t& a_rvaOut) const override
			{
				const auto address = REL::RelocationID(0, a_id).address();
				if (address == 0 || address < Base()) {
					return false;
				}
				a_rvaOut = address - Base();
				return true;
			}

			[[nodiscard]] bool ReadBytes(std::uint64_t a_rva, std::uint8_t* a_out, std::size_t a_size) const override
			{
				const auto imageSize = ModuleImageSize();
				if (a_size == 0 || a_rva >= imageSize || a_size > imageSize - a_rva) {
					return false;
				}

				const auto* address = reinterpret_cast<const void*>(Base() + a_rva);
				MEMORY_BASIC_INFORMATION mbi{};
				if (::VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
					return false;
				}
				if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
					return false;
				}
				std::memcpy(a_out, address, a_size);
				return true;
			}
		};

		// The verified bytes travel inside the DLL, so a DLL-only install still
		// has them. There is no on-disk table the check could be edited out of.
		[[nodiscard]] const HookTable* SelectHookTable(const ModuleIdentity& a_actual)
		{
			static std::vector<HookTable> tables;
			static bool                   populated = false;

			if (!populated) {
				populated = true;
				for (std::size_t i = 0; i < kEmbeddedHookTableCount; ++i) {
					HookTable   table;
					std::string error;
					if (!ParseHookTable(kEmbeddedHookTables[i].json, table, error)) {
						logger::error("hook table {}: {}", kEmbeddedHookTables[i].source, error);
						continue;
					}
					table.source = kEmbeddedHookTables[i].source;
					tables.push_back(std::move(table));
				}
			}

			for (const auto& table : tables) {
				if (IdentityMatches(table.identity, a_actual)) {
					return &table;
				}
			}
			return nullptr;
		}

		// Run once per frame, after the original frame-tail leaf. The former
		// StartMainThreadPump body: no task is queued, nothing re-adds itself, and
		// the tick neither allocates nor recurses.
		void FrameTailThunk(void* a_this)
		{
			if (g_original) {
				g_original(a_this);
			}
			if (g_inTick) {
				return;  // never re-enter the pump from inside itself
			}
			g_inTick = true;
			ProxyRegistry::Get().MainThreadTick();
			g_inTick = false;
		}
	}

	bool InstallFrameTickHook()
	{
		if (g_installed) {
			return true;
		}

		const auto init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error("frame-tick hook: MH_Initialize failed ({})", MH_StatusToString(init));
			Health::Get().Degrade("frame-tick hook: MH_Initialize failed");
			return false;
		}

		// The committed table is AE-only (1.7.104.0); on any other runtime the
		// table cannot verify the target, so nothing is patched.
		if (REL::Module::GetRuntime() != REL::Module::Runtime::AE) {
			logger::error("frame-tick hook: the committed table covers Skyrim AE 1.7.104.0 only; refusing to hook this runtime");
			Health::Get().Degrade("frame-tick hook: runtime is not AE");
			return false;
		}

		EngineResolver resolver;
		const auto     actual = ReadModuleIdentity();
		const auto*    table = SelectHookTable(actual);
		if (table == nullptr) {
			logger::error("frame-tick hook: running SkyrimSE.exe (size {}, timestamp 0x{:X}, sizeofimage 0x{:X}) is not "
						  "the build this revision was verified against; refusing to hook. Regenerate hooks/ with "
						  "tools/gen-hooktable.py for this build.",
				actual.size, actual.timeDateStamp, actual.sizeOfImage);
			Health::Get().Degrade("frame-tick hook: running binary is not the verified build");
			return false;
		}

		const auto&  target = GetHookTarget(HookTargetId::kMainUpdateFrameTail);
		const auto*  record = table->Find(target.name);
		if (record == nullptr) {
			logger::error("frame-tick hook: no verification entry named '{}' in {}", target.name, table->source);
			Health::Get().Degrade("frame-tick hook: no verification entry");
			return false;
		}

		const auto check = VerifyHookTarget(*record, resolver);
		if (!check.Verified()) {
			logger::error("frame-tick hook: REFUSED - {} (rva 0x{:X}; expected fnv1a64 0x{:016X}, got 0x{:016X})",
				TargetVerdictName(check.verdict), check.rva, check.expectedHash, check.actualHash);
			Health::Get().Degrade("frame-tick hook: target not verified");
			return false;
		}

		const auto address = resolver.Base() + check.rva;
		const auto created = MH_CreateHook(reinterpret_cast<LPVOID>(address),
			reinterpret_cast<LPVOID>(&FrameTailThunk),
			reinterpret_cast<LPVOID*>(&g_original));
		if (created != MH_OK) {
			logger::error("frame-tick hook: MH_CreateHook failed ({})", MH_StatusToString(created));
			Health::Get().Degrade("frame-tick hook: MH_CreateHook failed");
			return false;
		}

		const auto enabled = MH_EnableHook(reinterpret_cast<LPVOID>(address));
		if (enabled != MH_OK) {
			logger::error("frame-tick hook: MH_EnableHook failed ({})", MH_StatusToString(enabled));
			Health::Get().Degrade("frame-tick hook: MH_EnableHook failed");
			return false;
		}

		g_installed = true;
		g_targetAddress = address;
		logger::info("frame-tick hook: verified rva 0x{:X} ({} bytes, fnv1a64 0x{:016X}) and enabled; "
					 "main-thread tick running (Main::Update frame-tail leaf, once per frame)",
			check.rva, record->prologueLength, record->prologueHash);
		return true;
	}

	void RemoveFrameTickHook()
	{
		if (!g_installed) {
			return;
		}
		MH_DisableHook(reinterpret_cast<LPVOID>(g_targetAddress));
		MH_Uninitialize();
		g_installed = false;
		g_original = nullptr;
		g_targetAddress = 0;
	}
}
