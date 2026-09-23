#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/local-build.sh - build PushAside.dll on NixOS/Linux with clang-cl +
# lld-link against the Microsoft CRT/Windows SDK obtained via xwin (no Wine, no
# MSVC, nothing installed into the system or user environment).
#
# This reproduces what xmake's `commonlibsse-ng.plugin` rule does on Windows:
#   * kind=shared, arch=x64, /MD runtime
#   * a generated commonlib-plugin.rc VERSIONINFO resource
#   * a generated commonlibsse-ng-plugin.cpp that exports
#       SKSEPlugin_Version, SKSEPlugin_Query
#     (SKSEPlugin_Load comes from src/main.cpp's SKSE_PLUGIN_LOAD)
#   * the CommonLibSSE-NG static lib, MinHook, spdlog, DirectXMath/DirectXTK
#     headers and the syslink set from lib/CommonLibSSE-NG/xmake.lua
#
# Usage (from the repo root), overriding the submodule paths when the
# lib/ submodules are not checked out:
#
#   nix shell nixpkgs#lld nixpkgs#llvmPackages.llvm \
#              nixpkgs#llvmPackages.clang-unwrapped \
#     -c env CLNG=/path/to/CommonLibSSE-NG MINHOOK=/path/to/minhook \
#          bash tools/local-build.sh
#
# One-time sysroot (629 MB), if /tmp/xwin-sysroot is absent:
#
#   nix run nixpkgs#xwin -- --accept-license --cache-dir /tmp/xwin-cache \
#       splat --output /tmp/xwin-sysroot
#
# Output: dist/local-build/PushAside.dll
#
# This is a clang-cl build, NOT the CI MSVC toolchain: it is an iteration loop
# (compile + link locally instead of a CI round trip), never a byte-match oracle
# and never a substitute for in-game verification.
# ---------------------------------------------------------------------------
set -uo pipefail

PROJ=${PROJ:-$(pwd)}
CLNG=${CLNG:-$PROJ/lib/CommonLibSSE-NG}
MINHOOK=${MINHOOK:-$PROJ/lib/minhook}
XWIN=${XWIN:-/tmp/xwin-sysroot}
WORK=${WORK:-$PROJ/dist/local-build}
JOBS=${JOBS:-$(nproc)}

SPD=${SPD:-/tmp/spdlog-1.16.0}
DXM=${DXM:-/tmp/DirectXMath}
DXT=${DXT:-/tmp/DirectXTK}

die() { echo "local-build: $*" >&2; exit 1; }

[ -d "$XWIN/crt/include" ] || die "missing $XWIN - see the xwin command in this script's header"
[ -d "$CLNG/include/REL" ] || die "missing CommonLibSSE-NG checkout at $CLNG (set CLNG=)"
[ -d "$MINHOOK/include" ] || die "missing MinHook checkout at $MINHOOK (set MINHOOK=)"
command -v clang-cl >/dev/null || die "clang-cl not on PATH (nix shell nixpkgs#llvmPackages.clang-unwrapped)"
command -v lld-link >/dev/null || die "lld-link not on PATH (nix shell nixpkgs#lld)"
command -v llvm-lib >/dev/null || die "llvm-lib not on PATH"
command -v llvm-rc  >/dev/null || die "llvm-rc not on PATH"

# --- dependencies (headers only) -------------------------------------------
if [ ! -f "$SPD/include/spdlog/spdlog.h" ]; then
  echo "local-build: fetching spdlog v1.16.0 ..."
  ( cd /tmp && curl -sL -o spdlog-1.16.0.tar.gz \
      https://github.com/gabime/spdlog/archive/refs/tags/v1.16.0.tar.gz && \
    tar xzf spdlog-1.16.0.tar.gz ) || die "spdlog fetch failed"
fi
[ -d "$DXM/Inc" ] || { echo "local-build: fetching DirectXMath ..."; git clone --depth 1 https://github.com/microsoft/DirectXMath "$DXM" >/dev/null 2>&1; }
[ -d "$DXT/Inc" ] || { echo "local-build: fetching DirectXTK ...";  git clone --depth 1 https://github.com/microsoft/DirectXTK  "$DXT" >/dev/null 2>&1; }

INC="-imsvc $XWIN/crt/include -imsvc $XWIN/sdk/include/ucrt \
     -imsvc $XWIN/sdk/include/shared -imsvc $XWIN/sdk/include/um \
     -I $CLNG/include -I $CLNG/src \
     -I $MINHOOK/include \
     -I $DXM/Inc -I $DXT/Inc -I $SPD/include"

# spdlog is compiled as a static lib by CLNG's package config (header_only=false),
# which defines SPDLOG_COMPILED_LIB. Keep the defines identical everywhere.
SPDF="-DSPDLOG_COMPILED_LIB -DSPDLOG_USE_STD_FORMAT -DSPDLOG_WCHAR_TO_UTF8_SUPPORT=1"

CXXFLAGS="--driver-mode=cl --target=x86_64-pc-windows-msvc $INC \
  /TP -std:c++23preview -EHsc -O2 /MD /Gy /Zc:inline /Zc:preprocessor /Zc:enumTypes \
  /Zc:templateScope /permissive- /utf-8 /bigobj \
  $SPDF -DENABLE_SKYRIM_AE=1"

CFLAGS="--driver-mode=cl --target=x86_64-pc-windows-msvc $INC \
  /TC -O2 /MD -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS"

rm -rf "$WORK/obj"; mkdir -p "$WORK/obj"/{clng,proj,spd,minhook,gen} "$WORK/gen"

# --- generated files the xmake rule would create ----------------------------
cat > "$WORK/gen/commonlibsse-ng-plugin.cpp" <<'EOF'
#include <SKSE/SKSE.h>

SKSEPluginInfo(
    .Version = { 0, 1, 0, 0 },
    .Name = "PushAside",
    .Author = "aviallon",
    .SupportEmail = "",
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary
)
EOF

cat > "$WORK/gen/commonlib-plugin.rc" <<'EOF'
#include <winres.h>

1 VERSIONINFO
FILEVERSION 0, 1, 0, 0
PRODUCTVERSION 0, 1, 0, 0
FILEFLAGSMASK 0x17L
FILEFLAGS 0x0L
FILEOS 0x4L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "Comments", ""
            VALUE "FileDescription", "Physics-based character pushing for Skyrim AE"
            VALUE "FileVersion", "0.1.0.0"
            VALUE "InternalName", "PushAside"
            VALUE "LegalCopyright", "aviallon"
            VALUE "LegalTrademarks", "GPL-3.0 License"
            VALUE "ProductName", "PushAside"
            VALUE "ProductVersion", "0.1.0.0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
EOF

llvm-rc /I "$XWIN/sdk/include/um" /I "$XWIN/sdk/include/shared" \
  /fo "$WORK/obj/gen/commonlib-plugin.res" "$WORK/gen/commonlib-plugin.rc" \
  || die "resource compile failed"

# --- spdlog v1.16.0 compiled static lib -------------------------------------
for f in "$SPD"/src/*.cpp; do
  clang $CXXFLAGS -c "$f" -o "$WORK/obj/spd/$(basename "$f" .cpp).obj"
done || die "spdlog compile failed"

# --- generated plugin cpp (exports SKSEPlugin_Version/SKSEPlugin_Query) ------
clang $CXXFLAGS -c "$WORK/gen/commonlibsse-ng-plugin.cpp" \
  -o "$WORK/obj/gen/commonlibsse-ng-plugin.obj" || die "plugin export TU compile failed"

# --- CommonLibSSE-NG sources ------------------------------------------------
find "$CLNG/src" -name '*.cpp' > "$WORK/clng-list.txt"
export CXXFLAGS CLNG WORK
cat "$WORK/clng-list.txt" | xargs -P "$JOBS" -I{} bash -c '
  f="{}"; rel=${f#"$CLNG"/src/}; out="$WORK/obj/clng/${rel//\//_}.obj"
  clang $CXXFLAGS -FI "$CLNG/include/SKSE/Impl/PCH.h" -c "$f" -o "$out"
' || die "CommonLibSSE-NG compile failed"
echo "local-build: clng objects $(ls "$WORK"/obj/clng/*.obj 2>/dev/null | wc -l)/$(wc -l < "$WORK/clng-list.txt")"

# --- MinHook (C) -------------------------------------------------------------
for f in hook buffer trampoline; do
  clang $CFLAGS -I "$MINHOOK/include" -I "$MINHOOK/src" \
    -c "$MINHOOK/src/$f.c" -o "$WORK/obj/minhook/$f.obj" || die "minhook compile failed: $f"
done
clang $CFLAGS -I "$MINHOOK/include" -I "$MINHOOK/src" -I "$MINHOOK/src/hde" \
  -c "$MINHOOK/src/hde/hde64.c" -o "$WORK/obj/minhook/hde64.obj" || die "minhook compile failed: hde64"

# --- project sources ---------------------------------------------------------
for f in $(find "$PROJ/src" -name '*.cpp'); do
  rel=${f#"$PROJ"/src/}; out="$WORK/obj/proj/${rel//\//_}"
  clang $CXXFLAGS -I "$PROJ/src" -DPA_BUILD_ID='"local"' \
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -FI "$PROJ/src/PCH.h" -c "$f" \
    -o "${out%.cpp}.obj" || die "project compile failed: $f"
done

# --- archive + link (plain /O2, no LTCG) ------------------------------------
ls "$WORK"/obj/spd/*.obj  | sed 's/^/"/;s/$/"/' > "$WORK/spd.rsp"
ls "$WORK"/obj/clng/*.obj | sed 's/^/"/;s/$/"/' > "$WORK/clng.rsp"
llvm-lib /nologo /out:"$WORK/spdlog.lib" @"$WORK/spd.rsp"
llvm-lib /nologo /out:"$WORK/clng.lib"   @"$WORK/clng.rsp"
llvm-lib /nologo /out:"$WORK/minhook.lib" \
  "$WORK/obj/minhook/hook.obj" "$WORK/obj/minhook/buffer.obj" \
  "$WORK/obj/minhook/trampoline.obj" "$WORK/obj/minhook/hde64.obj"

lld-link /nologo /dll /machine:x64 /subsystem:windows /debug \
  /out:"$WORK/PushAside.dll" \
  /libpath:"$XWIN/crt/lib/x86_64" /libpath:"$XWIN/sdk/lib/ucrt/x86_64" \
  /libpath:"$XWIN/sdk/lib/um/x86_64" \
  "$WORK"/obj/proj/*.obj "$WORK"/obj/gen/*.obj "$WORK"/obj/gen/commonlib-plugin.res \
  "$WORK/spdlog.lib" "$WORK/clng.lib" "$WORK/minhook.lib" \
  advapi32.lib bcrypt.lib d3d11.lib d3dcompiler.lib dbghelp.lib dxgi.lib \
  ole32.lib shell32.lib user32.lib version.lib \
  /OPT:REF /OPT:ICF > "$WORK/link.log" 2>&1 || {
    tail -30 "$WORK/link.log" >&2; die "link failed (full log: $WORK/link.log)"; }

echo "local-build: OK -> $WORK/PushAside.dll ($(stat -c%s "$WORK/PushAside.dll") bytes)"
