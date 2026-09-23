# PushAside

An SKSE plugin that adds **physics-based character pushing** to Skyrim SE/AE:
actors and objects that collide with the player receive a configurable impulse,
applied through the engine's `bhkCharProxyController` interaction callbacks.

**This repository is currently a scaffold.** The plugin loads, reads and logs
its configuration, attaches a stub `hkpCharacterProxyListener` to the player's
character proxy, and ships the proven HeapSentinel verification infrastructure
(the hook-target table, the runtime verifier, the FOMOD packaging and the
off-game tests). It applies **no impulse yet**: the listener callbacks are empty
with TODOs. See the TODO list below.

The push mechanism is **listener-attach**, not a code hook: the engine calls the
player `bhkCharProxyController`'s `hkpCharacterProxyListener` virtuals. That
still needs a **main-thread tick** to re-attach when the player's controller is
rebuilt and to run the model's main-thread half. The tick is driven by a
MinHook **function-entry detour on the leaf `Main::Update` calls as its last
instruction before the epilogue** (AE Address Library id 107306, RVA
`0x154AF70` on 1.7.104), verified at load against the committed `hooks/` table
before anything is patched.

`Main::Update`'s **own entry is deliberately not hooked**: HDT-SMP already
installs a function-entry detour there, so its committed prologue no longer
matches and our entry hook is refused (the crash log from 2026-09-23 shows
exactly that). CommunityShaders also patches a call site around
`Main::Update+0x160`, and SKSE dispatches from `Main::Update+0x9A`. The frame-tail
leaf has exactly one caller in the whole `.text` (that last `Main::Update` call),
is untouched by all three, and takes one pointer argument and returns void, so it
is the simplest collision-free per-frame point.

### Why the tick is not an `SKSE::TaskInterface` task

The first implementation queued a task with
`SKSE::GetTaskInterface()->AddTask(*task)` and re-queued itself from **inside**
the task, with the lambda capturing a `shared_ptr` to its own `std::function`.
SKSE drains its task queue from within `Main::Update`, so a self-re-adding task
never lets the queue drain: the game froze at the main menu while the periodic
`listener stats:` heartbeat kept printing (the main thread was stuck in that
loop, and the crash log showed the plugin on the `Main::Update` dispatch stack).
The frame-tail detour above removes the task/queue mechanism entirely.

## Goal

Make ordinary movement tactile. Walking into an NPC or a loose object should
shove it, with strength, radius, vertical bias and cooldown controlled from
`Data/SKSE/Plugins/PushAside.ini` (or chosen in the installer wizard).

## Build

The plugin is a Windows PE x64 DLL and **can only be built on Windows with
MSVC**. xmake refuses `-p windows` on Linux, and MinGW is ABI-incompatible with
SKSE (Itanium vs MSVC name mangling, different `std::string` layout). See
[`research/build.md`](research/build.md) for the measured verdict.

CI does it (`.github/workflows/build.yml`, `windows-latest`):

```
git clone --recursive https://github.com/aviallon/Push-Physics-FFF
cd Skyrim-Push-Aside
xmake f -p windows -a x64 -m release -y
xmake build PushAside
```

Get the DLL / FOMOD / flat archive from the workflow artifacts on a branch, or
from the GitHub Release on a tag.

### Dependencies (git submodules)

Pinned revisions, also recorded in [`.gitmodules`](.gitmodules):

| Submodule | URL | Pin |
|---|---|---|
| `lib/CommonLibSSE-NG` | `https://github.com/alandtse/CommonLibVR.git` (branch `ng`) | `abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7` |
| `lib/minhook` | `https://github.com/TsudaKageyu/minhook.git` | `c3fcafdc10146beb5919319d0683e44e3c30d537` |

This scaffold was created **without cloning** them. Initialise them with:

```
git submodule update --init --recursive
cd lib/CommonLibSSE-NG && git checkout --detach abe9ca7b7318dbc04bccfcd59fc3fb670244c2d7 && cd -
cd lib/minhook         && git checkout --detach c3fcafdc10146beb5919319d0683e44e3c30d537 && cd -
```

The CommonLibSSE-NG pin is the revision whose `include/REL/IDDB.h` defines
`Format::SSEv5`, i.e. it can read the Skyrim AE 1.7.104 Address Library
(`versionlib-1-7-104-0.bin`, format 5). CI asserts that before compiling.

## Test (Linux or Windows, no game required)

The hook-target parser and the verifier are plain C++ and run off-game:

```
nix develop                       # or any shell with xmake + gcc
xmake f     -P tests -p linux -m release -y
xmake build -P tests -y
xmake run   -P tests
```

The xmake action must come **before** `-P`, or xmake walks up and configures
the Windows-only plugin project instead of `tests/`.

## Install / deploy

PushAside is installed through **Amethyst** (or MO2/Vortex), which understands
the FOMOD: install `PushAside-<version>-fomod.zip` and pick a push profile in
the wizard. The wizard copies the chosen `fomod/profiles/<profile>.ini` to
`SKSE/Plugins/PushAside.ini`.

Manual install: copy `PushAside.dll` to `Data/SKSE/Plugins/` and the config to
`Data/SKSE/Plugins/PushAside.ini`. Logs land next to the other SKSE logs.

## Layout

```
src/
  BuildInfo.h             version + build id
  main.cpp                SKSE entry point (messages, config, registry init, frame-tick detour)
  Config.{h,cpp}          INI config (research/design.md sec 6.3 schema), plugin directory
  Health.{h,cpp}          OK / DEGRADED / OFF verdict (gate 20)
  FrameClock.{h,cpp}      monotonic ms + once-per-frame key
  HkMath.h                hkVector4 <-> math::Vec3 helpers
  PhysicsMath.{h,cpp}     PURE push-model maths (direction, mu, heavy gate, caps,
                          cooldown/take-max, expiry, gate refusals) - Linux-tested
  ProxyAccess.{h,cpp}     AsProxyController / ControllerOf / PlayerProxy
  ProxyRegistry.{h,cpp}   main-thread actor<->proxy snapshot behind a seqlock
  PushRegistry.{h,cpp}    per-target push buffer (fixed table, spinlocked)
  StaggerQueue.{h,cpp}    SPSC ring: callback queues a stagger, main thread drains
  PushModel.{h,cpp}       Path 1 (character) + Path 2 (rigid body) model adapter
  PushListener.{h,cpp}    hkpCharacterProxyListener subclass (real overrides, orphan check)
  PushManager.{h,cpp}     vtable-checked attach, re-attach, calibration line
  Hooks/
    HookTargets.def       the frame-tail detour target + E1..E5 (commented)
    HookTargets.h         X-macro expansion -> enum + metadata
    HookTable.{h,cpp}     strict parser for the committed verification table
    HookVerifier.{h,cpp}  identity + id + vtable slot + prologue-hash check
    FrameTickHook.{h,cpp} verified MinHook entry detour that drives the tick
    HookTableData.gen.h   committed tables embedded in the DLL (generated)
tools/
  gen-hooktable.py        regenerate the committed table (needs the game binary)
  check-hooktable.py      CI gate: completeness / consistency (no game needed)
  validate_fomod.py       self-contained FOMOD gate (keys, XML, package, flags)
fomod/                    info.xml, ModuleConfig.xml, profiles/, schema/
config/PushAside.ini      documented defaults
hooks/                    committed verification tables (AE 1.7.104: Main::Update frame-tail leaf)
tests/                    off-game xmake project: verification infra + pure model maths
research/build.md         the build/deploy verdict for this host
```

## Implemented off-game / not yet verified in game

The off-game half of [`research/design.md`](research/design.md) is implemented:
config schema (sec 6.3/6.5), the listener-attach mechanism, the seqlock
`ProxyRegistry`, `PushRegistry`, `PushModel` Paths 1 and 2, the deferred
`StaggerQueue`, and the evaluable failsafe gates. `tests/` asserts the pure
model maths (direction, mu, heavy gate, caps, cooldown/take-max, once-per-frame,
expiry, gate refusals) and the hook-table infrastructure on Linux.

Still to do in game (design.md sec 7 step 3 and 12):

- [ ] Attach and observe with no physics: confirm slot-4 dispatch, the callback
      thread, `characterMass`/`characterStrength`, and the slot-1 ABI (U1-U4).
- [ ] Pin the `ObjectInteractionCallback` normal sign from the logged dot().
- [ ] Implement the behaviour-graph stagger event behind the marked TODO in
      `PushModel::TickMainThread` (design.md 4.1 step 8).
- [ ] Tune against the `fMinRelSpeed` / `fStaggerDeltaV` calibration line.
- [ ] Escalations E1..E5 only if the live evidence asks for them: uncomment the
      entry in `HookTargets.def`, regenerate `hooks/`, set `bUseEscalationHooks=1`.
- [ ] Stress-test in game with other physics mods.

## Licence

GPL-3.0-or-later **with the Modding Exception** — see [`LICENSE`](LICENSE) and
[`EXCEPTIONS`](EXCEPTIONS). The exception grants the additional right to link
this plugin with the game and with modding libraries whose licences are
GPL-incompatible. MinHook is BSD-2-Clause; CommonLibSSE-NG is GPL-3.0-or-later
with the Modding Exception (linked, not modified).
