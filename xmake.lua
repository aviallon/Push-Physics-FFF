-- PushAside - physics-based character pushing for Skyrim SE/AE.
--
-- Build system: xmake + CommonLibSSE-NG (git submodule) + MinHook (git
-- submodule), the same shape as the other local SKSE ports (HeapSentinel,
-- CrosshairRefEventsFix, SexLabpp). CommonLibSSE-NG is pinned to the revision
-- whose include/REL/IDDB.h defines Format::SSEv5, i.e. the one that can read
-- the 1.7.104 Address Library (versionlib-1-7-104-0.bin, format 5). CI asserts
-- that before compiling.
--
-- See README.md and research/build.md: the plugin is a Windows PE x64 DLL and
-- can only be built on Windows with MSVC; Linux runs the off-game tests/ target.

set_xmakever("3.0.0")

includes("lib/CommonLibSSE-NG/xmake.lua")

set_project("PushAside")
set_version("0.1.0")
set_languages("c++23")
set_license("GPL-3.0")

-- Build identity stamped into the DLL and printed in the startup log. xmake's
-- script sandbox exposes only a small os API, so this uses the environment
-- rather than shelling out to git: CI sets GITHUB_SHA, a developer can set
-- PA_BUILD_ID, and a plain local build honestly reports "unknown".
local build_id = os.getenv("PA_BUILD_ID")
if not build_id or build_id == "" then
    build_id = os.getenv("GITHUB_SHA")
end
if not build_id or build_id == "" then
    build_id = "unknown"
end
build_id = build_id:sub(1, 12)
add_defines("PA_BUILD_ID=\"" .. build_id .. "\"")

set_allowedplats("windows")
set_allowedarchs("x64")
set_defaultplat("windows")
set_defaultarchs("x64")

add_rules("mode.debug", "mode.release")
set_runtimes("MD")
set_warnings("allextra")

if is_mode("debug") then
    add_defines("DEBUG")
    set_optimize("none")
elseif is_mode("release") then
    add_defines("NDEBUG")
    set_optimize("fastest")
    set_symbols("debug")
end

target("PushAside")
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "PushAside",
        author = "aviallon",
        description = "Physics-based character pushing for Skyrim AE",
    })

    set_pcxxheader("src/PCH.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")

    -- MinHook is wired in but unused by the current listener-attach mechanism:
    -- it is retained so a future fallback can install a function-entry detour
    -- (the SKSE trampoline only redirects an existing branch).
    add_files("lib/minhook/src/hook.c", "lib/minhook/src/buffer.c", "lib/minhook/src/trampoline.c", "lib/minhook/src/hde/hde64.c")
    add_includedirs("lib/minhook/include", "lib/minhook/src", "lib/minhook/src/hde")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
target_end()
