# Building & deploying a CommonLibSSE-NG SKSE plugin for Skyrim AE 1.7.104 on this Linux/NixOS host

**Question.** How do we BUILD and DEPLOY an SKSE plugin (Windows PE x64 DLL) for
Skyrim AE 1.7.104 from this machine — and is a *local* Linux build possible?

**Answer in one line.** The plugin **cannot be built locally**: the toolchain is
MSVC/Windows, xmake refuses `-p windows` on Linux, and the only non-MSVC
alternative (MinGW) is ABI-incompatible with SKSE. The plugin is built by **GitHub
Actions `windows-latest`** (already configured); Linux is used for the off-game
`tests/` target and for FOMOD packaging/validation only. Local Linux can still do
fast **compile-checking** via `xwin` + `clang-cl`, but not the final link.

Primary reference: **`/home/aviallon/Programing/Opensource/HeapSentinel`**
(mit). Secondary reference: `/home/aviallon/Projects/Skyrim-A-Pose-Fix`.

---

## 1. What the primary reference (HeapSentinel) actually is

| Fact | Value / evidence |
|---|---|
| Build system | xmake (`set_xmakever("3.0.0")`), C++23, `set_runtimes("MD")` |
| Platform gate | `xmake.lua`: `set_allowedplats("windows")`, `set_allowedarchs("x64")`, `set_defaultplat("windows")` |
| Game target | Skyrim AE **1.7.104**, Address Library **format 5** (`Format::SSEv5` in `include/REL/IDDB.h`) |
| CLNG pin | `env COMMONLIB_COMMIT: abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7` (`.github/workflows/build.yml`); submodule = `alandtse/CommonLibVR` branch `ng` |
| Other submodule | `TsudaKageyu/minhook` (function-entry detours) |
| Linux flake | `flake.nix` — **off-game `tests/` only**, explicitly: *"The plugin itself is a Windows SKSE DLL and builds on Windows (…); this shell exists for src/Ipc's shared-memory IPC layer"* |
| Local evidence | `.xmake/linux/x86_64/` and `build/linux/x86_64/{release,debug}` exist (tests built locally); **no `build/windows`** locally |

`.gitmodules`:
```
[submodule "lib/CommonLibSSE-NG"] path = lib/CommonLibSSE-NG  url = https://github.com/alandtse/CommonLibVR.git  branch = ng
[submodule "lib/minhook"]         path = lib/minhook          url = https://github.com/TsudaKageyu/minhook.git
```

## 2. The build/packaging pipeline (exact commands)

Three independent jobs. Only the first needs Windows.

### 2a. Plugin build — `windows-latest`, MSVC (the only way to get the DLL)
```bash
# runner: windows-latest (MSVC 2022), xmake-io/github-action-setup-xmake@v1 (latest)
git clone --recursive https://github.com/aviallon/HeapSentinel && cd HeapSentinel
cd lib/CommonLibSSE-NG && git fetch --depth 1 origin abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7 \
  && git checkout --detach abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7 && cd -

xmake f -p windows -a x64 -m release -y      # CI retries up to 8x (xmake-mirror 504s on directxtk)
xmake build HeapSentinel
```
- `-m release`: NDEBUG + `set_optimize("fastest")` + symbols.
- The `commonlibsse-ng.plugin` rule (in `lib/CommonLibSSE-NG/xmake.lua`) forces
  `kind=shared`, `arch=x64`, injects the `.rc` VERSIONINFO and a
  `commonlibsse-ng-plugin.cpp` version export, and registers:
  `target:targetfile()` → `SKSE/Plugins/<name>.dll` and
  `target:symbolfile()` → `SKSE/Plugins/<name>.pdb`.
- **Output artifact path:** `build/windows/x64/release/HeapSentinel.dll`
  (CI is deliberately path-agnostic: `find build -name HeapSentinel.dll | head -1`).
- CI gate: `strings` the DLL for the plugin name, config sections, report kinds,
  and signature-verification strings — a stub build fails.

### 2b. Flat mod archive — same Windows job
```bash
out/SKSE/Plugins/HeapSentinel.dll     # copied from build/...
out/SKSE/Plugins/HeapSentinel.ini     # copied from config/
out/{README,RESEARCH,DESIGN}.md
(cd out && 7z a -bd -y ../HeapSentinel-0.5.0.zip .)   # archive ROOT == mod root
7z l HeapSentinel-0.5.0.zip | tr '\\' '/' | grep -q "SKSE/Plugins/HeapSentinel.dll"
```
(The rule's own `xmake package` would emit `build/packages/HeapSentinel-0.5.0.zip`
with a leading `Data/` — the CI bypasses it so the archive root is the mod root.)

### 2c. FOMOD package + validation — `ubuntu-latest` (Linux!)
```bash
# needs: unzip, xmllint (libxml2-utils), python3; downloads the DLL build artifact
python3 tools/validate_fomod.py \
    --dll <artifact>/SKSE/Plugins/HeapSentinel.dll \
    --zip "HeapSentinel-0.5.0-fomod.zip" --stage stage-fomod
unzip -Z1 HeapSentinel-0.5.0-fomod.zip   # must contain fomod/ModuleConfig.xml,
                                         # fomod/info.xml, SKSE/Plugins/HeapSentinel.dll,
                                         # fomod/profiles/*.ini
```
`fomod/{info.xml,ModuleConfig.xml,profiles/*.ini}` and `config/HeapSentinel.ini`
are **generated** from one source of truth:
`python3 tools/gen-fomod-profiles.py` (or `--check`; `validate_fomod.py` runs that
drift gate as check #1). Reference: `tools/validate_fomod.py`,
`tools/check-amethyst-fomod.py` (drives Amethyst's own FOMOD resolver headlessly).
Uploaded as artifact `HeapSentinel-FOMOD`; attached to the GitHub Release on tags.

Secondary reference `Skyrim-A-Pose-Fix/.github/workflows/build.yml` is the same
shape but needs an extra Windows step first: the Rust FFI staticlib
`serde_hkx_ffi.lib` built with `cargo build --release -p serde_hkx_ffi`
(crate `SARDONYX-sard/serde-hkx` @ `3f53c059…`), then
`xmake f -y -p windows -a x64 -m releasedbg --ccache=y && xmake build -y`,
DLL at `build/windows/x64/releasedbg/APoseFix.dll`.

## 3. Can it be built on this Linux machine? — No (measured, not assumed)

`nix shell nixpkgs#xmake` gives **xmake 3.1.0** (nixpkgs). A scratch project was
used so nothing was written into any repo:

```console
$ nix shell nixpkgs#xmake -c xmake f -y -p windows -a x64 -m releasedbg
checking for Microsoft Visual Studio (x64) version ... no
error: target(probe): toolchain not found!

$ nix shell nixpkgs#xmake nixpkgs#llvmPackages.clang-unwrapped -c \
      xmake f -y -p windows -a x64 -m releasedbg --toolchain=clang-cl
checking for Microsoft Visual Studio (x64) version ... no
error: target(probe): toolchain not found!     # xmake's windows platform routes via MSVC anyway
```

| Route | Verdict |
|---|---|
| xmake `-p windows` + MSVC | **Impossible** — no MSVC, and xmake's Windows platform requires it. |
| xmake `-p windows --toolchain=clang-cl` | **Impossible** — still detects MSVC first, then fails. |
| xmake `-p windows --toolchain=mingw` | Configure *succeeds* (`checking for Mingw SDK ... /usr`), **but MinGW is ABI-dead for SKSE**: Itanium vs MSVC name mangling, and `std::string`/`std::string_view` field layout differ (MSVC: `_Bx`@0, `_Mysize`@16, `_Myres`@24). Papyrus passes `std::string` by value from the MSVC-built game → silent corruption. Cannot ship. |
| `msvc-wine` (real `cl.exe`) | Runs Wine, but `cl.exe` is a separate ~2–3 GB VS Build Tools download and the VS EULA assumes Windows. Deliberately not used. |
| **`xwin` + `clang-cl` (MSVC ABI)** | **Works for compile-checking** (see §4). No Wine. Full link not attempted locally. |

**Linux CAN build/run the off-game `tests/` target** (HeapSentinel's stated
purpose for `flake.nix`), and that is the only Linux-local build in the project:
```console
$ nix develop    # provides pkgs.xmake, pkgs.gcc, pkgs.binutils, pkgs.git
$ xmake f     -P tests -p linux -m release -y   # action BEFORE -P, or xmake
$ xmake build -P tests -y                       # walks up to the Windows plugin
$ xmake run   -P tests
```
CI also runs this on `ubuntu-latest`, plus an `asan+ubsan` Linux job and two pure
Linux jobs (`hook-table`, `fomod`).

## 4. Local Linux compile-check loop (proven, optional, not a build)

The MSVC CRT + Windows SDK can be obtained without Wine and the real MSVC STL
headers can be used to type-check a translation unit in seconds. The sysroot is
already present: `/tmp/xwin-sysroot` (629 MB) + `/tmp/xwin-cache` (1.1 GB).
Recreate with:
```bash
nix run nixpkgs#xwin -- --accept-license --cache-dir /tmp/xwin-cache \
    splat --output /tmp/xwin-sysroot        # global opts BEFORE the subcommand
```
Proven on the secondary reference (no project mutation, output to `/tmp`):
```bash
cd /home/aviallon/Projects/Skyrim-A-Pose-Fix
nix shell nixpkgs#llvmPackages.clang-unwrapped -c bash -c '
  XWIN=/tmp/xwin-sysroot; PROJ=$PWD; CLNG=$PROJ/lib/commonlibsse-ng
  INC="-imsvc $XWIN/crt/include -imsvc $XWIN/sdk/include/ucrt \
       -imsvc $XWIN/sdk/include/shared -imsvc $XWIN/sdk/include/um \
       -I $CLNG/include -I $CLNG/src -I $CLNG/extern/openvr/headers \
       -I /tmp/DirectXMath/Inc -I /tmp/DirectXTK/Inc \
       -I /tmp/spdlog-1.16.0/include -I $PROJ/lib/serdehkx -I $PROJ/src"
  clang-cl --driver-mode=cl --target=x86_64-pc-windows-msvc $INC \
    /TP -std:c++23preview -EHsc -O2 /MD /bigobj \
    -DSPDLOG_USE_STD_FORMAT -DSPDLOG_COMPILED_LIB -DENABLE_SKYRIM_AE=1 \
    -FI $PROJ/src/pch.h -c $PROJ/src/main.cpp -o /tmp/aposef-probe/main.obj'
# clang version 21.1.8; EXIT=0; main.obj = 497,011 bytes (COFF x86_64); warnings only
```
A full working local clang-cl *link* of a CLNG plugin exists as a reference script:
`/home/aviallon/Projects/SexLabPPrism-re/build/tools/local-build.sh`
(+ `build/docs/local-build-loop.md`). Caveats it documents: clang-cl ≠ MSVC, it
uses no LTCG, it is **not** a byte-match oracle, and it hand-builds
spdlog/CLNG/mingw-ish deps. It is an iteration aid, never the release artifact.
Gotchas: `clang-unwrapped` (wrapped clang reads glibc headers into the Windows
target); `/TP` needed for `.h`; `/FI` not `-include` under the cl driver;
`-DENABLE_SKYRIM_AE=1` else `UNKNOWN_RUNTIME` size asserts; `-DSPDLOG_COMPILED_LIB`
else header-only spdlog pulls `windows.h` and collides with `REX/W32`.

## 5. Deployment into the game (documented; nothing written here)

Game install: `/home/aviallon/.local/share/Steam/steamapps/common/Skyrim Special Edition`
- `SkyrimSE.exe` present; `skse64_1_7_104.dll` + `skse64_loader.exe` are symlinks
  into Amethyst's mods dir.
- `skse64_readme.txt` says **SKSE64 v2.3.1 beta — Steam: 1.7.104** → **matches**.
- `Data/SKSE/Plugins/` exists and is a symlink farm into
  `/home/aviallon/Games/Amethyst/Skyrim Special Edition/profiles/default/mods/<Mod>/SKSE/Plugins/`.
- Address Library: `Data/SKSE/Plugins/versionlib-1-7-104-0.bin` **present**
  (format 5; the version the CLNG pin asserts).
- HeapSentinel is **already installed via Amethyst** as
  `mods/HeapSentinel-0.5.0-fomod/`, and is running:
  `Data/SKSE/Plugins/HeapSentinel.log` and `HeapSentinel-reports.log` exist.
  `APoseFix.dll` is likewise installed as `mods/A-Pose Fix/`.

**Install paths (do not do now):**
1. **Amethyst (recommended):** Mods → *Install mod from file…* → pick
   `HeapSentinel-0.5.0-fomod.zip`; the FOMOD wizard picks the profile, and the
   manager copies the chosen `fomod/profiles/*.ini` to `SKSE/Plugins/HeapSentinel.ini`.
   The flat `HeapSentinel-0.5.0.zip` (archive root = mod root) is the manual-install
   variant. Amethyst manages the mod into its `mods/<name>/SKSE/Plugins/` tree and
   the game's `Data/SKSE/Plugins/` gets symlinks, exactly as seen today.
2. **Manual copy:** `HeapSentinel.dll` → `Data/SKSE/Plugins/HeapSentinel.dll` and
   the chosen ini → `Data/SKSE/Plugins/HeapSentinel.ini`. Nothing else; logs land
   next to the DLL. (Not done here — the game install was not touched.)

## 6. Recommendation

1. **Build the plugin in CI** (`windows-latest`, MSVC) using
   `xmake f -p windows -a x64 -m release -y && xmake build HeapSentinel`.
   That is already committed. Get the DLL/FOMOD from the workflow artifacts (or a
   Release on a tag). Locally producing a shipping DLL is not possible.
2. **Use Linux for everything but the DLL:** the `tests/` xmake project, the
   FOMOD generator/validator and the Amethyst resolver cross-check — all already
   in CI on `ubuntu-latest`.
3. **If fast local iteration is needed**, use the `xwin` + `clang-cl`
   compile-check loop (§4) to catch syntax/type errors in seconds; treat a green
   local compile as *"it compiles"*, never as *"the DLL works"*.
4. **Deploy through Amethyst** with the FOMOD zip so a profile is selected and the
   ini is placed by the manager, matching how HeapSentinel 0.5.0 is installed here
   today.
