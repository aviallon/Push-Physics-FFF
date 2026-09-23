-- Off-game test harness for PushAside's hook-verification infrastructure.
--
-- Deliberately a standalone xmake project rather than part of the plugin build:
-- the plugin is Windows-only, but src/Hooks/{HookTable,HookVerifier} are plain
-- C++ with no Windows dependency, so building and RUNNING these tests on Linux
-- as well as Windows is what turns the verification logic into a
-- cross-compiler claim instead of a claim about one compiler.

set_project("PushAsideTests")
set_version("0.1.0")
set_languages("c++23")
set_license("GPL-3.0")

set_allowedplats("windows", "linux", "macosx")
-- No set_allowedarchs: xmake maps x64 to x86_64 on Linux and rejects it when the
-- allowed list names the Windows spelling. The arch is irrelevant here anyway -
-- the tests only care about a 64-bit target.

add_rules("mode.debug", "mode.release")

if is_mode("debug") then
    set_optimize("none")
    set_symbols("debug")
else
    set_optimize("fastest")
    set_symbols("debug")
end

target("pushaside-tests")
    set_kind("binary")
    set_warnings("allextra")

    add_files("main.cpp")
    -- The hook-target registry/parser and the verifier are plain C++, so the
    -- committed table format and the verification rules are exercised off-game
    -- on both platforms. No game binary is involved.
    add_files("../src/Hooks/HookTable.cpp")
    add_files("../src/Hooks/HookVerifier.cpp")
    -- The push model maths is pure C++ (PhysicsMath has no RE/Havok/Windows
    -- dependency), so the same gates and buffer rules that run in-game are
    -- asserted here on Linux.
    add_files("../src/PhysicsMath.cpp")
    -- The command parser, the live-config override table and the trace row
    -- formatter are pure C++ too: the accepted/rejected command grammar and the
    -- exact CSV shape are asserted here rather than only in-game.
    add_files("../src/CommandParse.cpp")
    add_files("../src/LiveConfig.cpp")
    add_files("../src/TraceFormat.cpp")
    add_includedirs("..", "../src", ".")

    if is_plat("linux") or is_plat("macosx") then
        add_syslinks("pthread")
    end
    if is_plat("windows") then
        add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")
    end
target_end()
