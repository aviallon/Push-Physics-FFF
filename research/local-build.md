# Building PushAside.dll locally on Linux/NixOS — DONE, verified artifact

**Status: SUCCESS.** A linked, loadable Windows PE32+ x86-64 DLL with the three
SKSE exports and the VERSIONINFO resource was produced on this Linux/NixOS host
with `clang-cl` + `lld-link` against the MSVC CRT/SDK obtained via `xwin`
(no Wine, no Visual Studio, nothing installed system-wide).

Artifact: `dist/PushAside.dll`
- size: **736,256 bytes**
- sha256: `6d889db8bf2687bde5e02ee42e2806cde46543c474d3e270005cd8082086aa40`
- toolchain: clang-cl 21.1.8 (NixOS `llvmPackages.clang-unwrapped`), lld-link 21.1.8, MSVC ABI, `/MD` dynamic CRT
- pinned CommonLibSSE-NG `abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7`, MinHook `c3fcafdc10146beb5919319d0683e44e3c30d537` (identical to the CI pins)

**This is a linked DLL, not a tested mod.** It has not been loaded by SKSE or the
game. `xwin`+`clang-cl` is *not* MSVC: it is an iteration loop, never a byte-match
oracle, and the final in-game acceptance is still what decides.

---

## Why xmake itself was not used (step 2 of the task: measured, with verbatim errors)

`xmake` 3.1.0 (nixpkgs) cannot build a `-p windows` target on Linux:

```console
$ xmake f -p windows -a x64 -m release -y
checking for Microsoft Visual Studio (x64) version ... no
checking for Microsoft Visual Studio (x64) version ... no
error: target(probe): toolchain not found!
```

Pointing it at the xwin tree does not help. `--sdkdir` is not even a valid xmake
option, and `--sdk` + `--toolchain=clang-cl` fails before any compiler probe:

```console
$ xmake f -p windows -a x64 -m release -y --toolchain=clang-cl --sdk=/tmp/xwin-sysroot
error: target(probe): toolchain not found!
$ xmake f -p windows -a x64 -m release -y --toolchain=clang-cl --sdkdir=/tmp/xwin-sysroot
error: Invalid option: --sdkdir=/tmp/xwin-sysroot
```

xmake's windows platform clang-cl/msvc toolchains are both gated on locating a
real Visual Studio installation; there is no supported way to hand them an
xwin-generated CRT/SDK tree. `xwin` *is* available from nixpkgs
(`nix shell nixpkgs#xwin -c xwin --version` → `0.9.0`), it just has to be driven
by `clang-cl`/`lld-link` directly.

## What the `commonlibsse-ng.plugin` rule requires at link time

Read from `lib/CommonLibSSE-NG/xmake.lua` (rule at line ~488) and
`res/commonlibsse-ng-plugin.cpp.in`, `res/commonlib-plugin.rc.in`:

- **kind / arch:** `kind=shared`, `arch=x64`.
- **exports:** the generated `commonlibsse-ng-plugin.cpp` expands `SKSEPluginInfo(...)` to
  `extern "C" __declspec(dllexport)` `SKSEPlugin_Version` (a `constinit
  SKSE::PluginDeclaration`) and `SKSEPlugin_Query`; `SKSEPlugin_Load` comes from the
  project's `SKSE_PLUGIN_LOAD` in `src/main.cpp`. All three must be exported.
- **resource:** a `VERSIONINFO` `.rc` generated from `res/commonlib-plugin.rc.in`
  (FileVersion/ProductVersion = 0.1.0.0, description, author, license).
- **runtime / subsystem:** `set_runtimes("MD")` → dynamic CRT; `shared` DLL →
  subsystem WINDOWS.
- **defines:** `ENABLE_SKYRIM_AE=1` (SE/VR off for this host: no openvr submodule,
  so `ENABLE_SKYRIM_VR` is intentionally not defined); `SPDLOG_COMPILED_LIB`,
  `SPDLOG_USE_STD_FORMAT`, `SPDLOG_WCHAR_TO_UTF8_SUPPORT=1` (CLNG's spdlog package
  is `header_only=false`).
- **libs:** CommonLibSSE-NG static lib (`src/**.cpp`), spdlog compiled static lib,
  MinHook (`hook.c`, `buffer.c`, `trampoline.c`, `hde/hde64.c`), and syslinks
  `advapi32 bcrypt d3d11 d3dcompiler dbghelp dxgi ole32 shell32 user32 version`.

## Exact reproducible command sequence

```bash
# 0. one-time MSVC CRT + Windows SDK sysroot (~629 MB), no Wine, no MSVC
nix run nixpkgs#xwin -- --accept-license --cache-dir /tmp/xwin-cache \
    splat --output /tmp/xwin-sysroot

# 1. the build (from the repo root). The lib/ submodules are not checked out in
#    this working tree, so CLNG/MINHOOK point at the local pinned checkouts.
cd /home/aviallon/Programing/Opensource/Skyrim-Push-Aside
nix shell nixpkgs#lld nixpkgs#llvmPackages.llvm nixpkgs#llvmPackages.clang-unwrapped \
  -c env \
     CLNG=/home/aviallon/Programing/Opensource/HeapSentinel/lib/CommonLibSSE-NG \
     MINHOOK=/home/aviallon/Programing/Opensource/HeapSentinel/lib/minhook \
     bash tools/local-build.sh

# 2. result
cp dist/local-build/PushAside.dll dist/PushAside.dll
```

Run it under `nice`/`ionice` if desired; a full build is ~17 minutes on this
machine (508 CommonLibSSE-NG translation units + the project + MinHook).

## Verification performed (no game involved)

```console
$ llvm-objdump --section-headers dist/PushAside.dll
# coff-x86-64, sections .text .rdata .data .pdata .tls .rsrc .reloc

$ llvm-readobj --coff-exports dist/PushAside.dll
Export { Name: SKSEPlugin_Load }
Export { Name: SKSEPlugin_Query }
Export { Name: SKSEPlugin_Version }

$ llvm-objdump -p dist/PushAside.dll
Magic                   020b  (PE32+)
Subsystem               00000002  (Windows GUI)
Resource Directory      present (.rsrc, VERSIONINFO, size 0x328)
DLL Name: MSVCP140.dll / VCRUNTIME140.dll / api-ms-win-crt-*.dll   # /MD dynamic CRT
DLL Name: KERNEL32/USER32/SHELL32/ole32/VERSION                     # system only
```

- No import outside the system set (no `commonlibsse-ng.dll`, no game module):
  CommonLibSSE-NG and MinHook are statically linked in.
- The VERSIONINFO block decodes to FileVersion `0.1.0.0`, InternalName `PushAside`,
  LegalCopyright `aviallon`, license `GPL-3.0 License`.
- CI-recognisability strings present: `PushAside`, `PushAside.ini`,
  `PushAside v0.1.0`, `bVerifyTargets`, `vtable slot does not hold the expected function`.

## Known discrepancy with the CI string gate (source issue, not the build)

Two of the `.github/workflows/build.yml` `strings` assertions are **not** satisfied
by this tree, and would fail the CI gate on `windows-latest` too:

- `uStrengthPercent` — this literal does not exist anywhere in `src/` or `config/`
  (the config key is `fCharacterStrength`). The workflow assertion appears stale.
- `pushaside.hooktable/1` — present in `src/Hooks/HookTable.cpp`, but
  `HookTable`'s parse path is not reachable from the plugin's exports, so
  `/OPT:REF` dead-strips it from both this build and an MSVC release build.

## Recommendation

1. Use this DLL for a **local in-game smoke test** now (Amethyst install, or copy
   to `Data/SKSE/Plugins/PushAside.dll` with `config/PushAside.ini`), rather than
   waiting for CI.
2. Keep `windows-latest` as the **shipping** build: MSVC is the real toolchain and
   the CI path is what the project commits to.
3. Before the next CI run, either drop the stale `uStrengthPercent` assertion or
   restore the string, and make `HookTable` reachable (or drop the resource check)
   so the artifact gate matches the source.
