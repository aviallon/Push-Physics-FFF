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

## Instrumentation side channel

An in-game command channel and a CSV trace exist so a running game can be
interrogated without a rebuild + restart. All three files live next to
`PushAside.log`, i.e. `Documents/My Games/Skyrim Special Edition/SKSE/`:

| file             | direction        | purpose                                              |
| ---------------- | ---------------- | ---------------------------------------------------- |
| `PushAside.cmd`  | host -> plugin   | one command per line; `#` comments; blank ignored    |
| `PushAside.out`  | plugin -> host   | each command echoed with a timestamp and its result  |
| `PushAside.trace`| plugin -> host   | rich CSV rows while `trace on` (default every frame) |

All three are inert when absent/off: a shipped install has none of them, and
the poll costs one `stat` per 100 ms.

### Commands (run on the main thread unless noted)

```
help                                             list the commands
status                                           counters, registry size, config summary
registry                                         every registry entry (actor, proxy, controller,
                                                 collidable, mass, flags)
bump                                             the live bump record and its full resolution
                                                 chain (charBody -> refr -> Actor -> ctrl ->
                                                 proxy, ctrl vptr + class, rigid body motion)
vtables                                          resolved controller vtable addresses
actors                                           ProcessLists high actors with controller vptr
                                                 and the class that vptr matches
watch <formID> / unwatch                         record one actor in every trace row
trace on|off|status|every <n>                    control PushAside.trace
set                                              list live-settable config keys
set <Section>:<Key> <value>                      change a config value live (General is a
                                                 wildcard section)
push <formID> <dv> [ctrl|rb|both]                one explicit push (applied on the physics
                                                 thread; the before/after velocities are
                                                 reported in PushAside.out). Refused with a
                                                 reason unless the game is active and the
                                                 simulation is stepping.
pushhere [dv] [ctrl|rb|both]                     push whatever the bump record names (same
                                                 refusal rules as push)
pushdry <formID> [dv] [ctrl|rb|both]             resolve a target and log exactly what a push
                                                 would write (pointers, current velocities,
                                                 dv/direction) without writing anything. Runs
                                                 even while the simulation is stalled, so the
                                                 plumbing can be verified with zero hang risk.
```

`set` writes the loaded `Config` and republishes it behind a seqlock
(`src/LiveConfig.{h,cpp}`); every physics-thread reader takes a consistent
`LiveConfig::Snapshot()`, so a live change cannot tear under the physics step.

`push` resolves the target actor, controller, proxy and shove axis on the main
thread, then publishes a request (`src/PushRequest.{h,cpp}`). The write is applied
on the **main thread**, once per frame, in `ProxyRegistry::MainThreadTick`, under
the Havok world write lock (`world->worldLock`, the lock Precision takes for
structural world changes): `ctrl` calls
`bhkCharacterController::SetLinearVelocityImpl(current + dv)`, `rb` adds `dv` to
the character rigid body's `motion.linearVelocity`, `both` does both. The engine
itself sets character velocities from the main thread, so this is the engine's
own usage pattern.

It deliberately no longer runs inside `PushListener::ProcessConstraintsCallback`:
that callback runs inside the physics step, in the player character's own solver
callback, and a push writes into a **different** character's controller / rigid
body. That re-entrant write hung two game sessions (the main thread ended up
spinning in `sched_yield`, no crash log). The observed before/after velocities and
whether they actually changed are published back and appended to
`PushAside.out`. `pushdry` exercises the same resolution path with no write.

### Live commands need a focused, running game

The `push` and `pushhere` commands write Havok state. Havok only steps while the
game window is **focused and the simulation is running**: an unfocused window or
a paused game still runs the main-thread tick, but the physics character-proxy
callback stops, so a request queued then would sit in the slot unapplied (or be
applied later against a stale simulation). This is also how the 2026-09 hang
looked: the `constraints` counter fell from ~170/s to ~3/s, then stopped, while
nothing in the log said so.

The plugin therefore refuses `push`/`pushhere` unless **all** of these hold, and
writes the precise reason to `PushAside.out` instead of publishing anything:

* the game reports itself running (`RE::Main::gameActive`);
* the physics is not stalled: `ProcessConstraintsCallback` has advanced within
the last 2 s;
* a player proxy exists.

A stall is detected on the main thread from the already-visible `constraints`
counter. While stalled, one warning is logged (with the player proxy, the
counter and whether a push is pending) and the plugin stays stalled until the
callbacks advance again, at which point one recovery line is logged. `status`
now reports `sim: gameActive=... stalled=...` so a stalled run is visible even
without watching `PushAside.out`.

Read-only commands (`status`, `bump`, `registry`, `actors`, `vtables`) keep
working regardless of the simulation state: they are how a stall is
diagnosed. `pushdry` also runs while stalled: it reads state and writes nothing.
A request that was already pending when a stall began is dropped by the
main-thread apply (with a `push refused:` line in `PushAside.out`) rather than
applied. The whole watchdog is inert when `PushAside.cmd` is absent.

### Trace columns (51)

```
frame,ms,
player_proxy,player_x,player_y,player_z,player_vx,player_vy,player_vz,player_mass,
bump_charBody,bump_refr,bump_actor,bump_ctrl,bump_ctrl_vptr,bump_rb,
bump_rb_vx,bump_rb_vy,bump_rb_vz,bump_rb_x,bump_rb_y,bump_rb_z,
bump_rb_motion,bump_rb_mass,bump_rb_dynamic,
push_mode,push_dv,push_mech,push_changed,
push_ctrl_from_x,push_ctrl_from_y,push_ctrl_from_z,
push_ctrl_to_x,push_ctrl_to_y,push_ctrl_to_z,
push_rb_from_x,push_rb_from_y,push_rb_from_z,
push_rb_to_x,push_rb_to_y,push_rb_to_z,
watch_form,watch_x,watch_y,watch_z,
watch_ctrl_vx,watch_ctrl_vy,watch_ctrl_vz,
watch_rb_vx,watch_rb_vy,watch_rb_vz
```

The header and legend are written when the file is created; the trace stops at
64 MB and says so in the file and the log. Rows are built in a stack buffer and
written through one buffered `FILE*`, flushed once per second. Only the main
thread ever touches the files.

## Unattended in-game harness

`tools/ingame-harness.sh` drives an instrumented game run without a human at the
keyboard: it snapshots the saves, launches Skyrim through SKSE, loads the most
recent save, holds forward, runs a command list, collects the evidence and puts
the saves back. Every wait is bounded and a frozen simulation fails loudly
instead of producing a meaningless run.

One full pass:

```sh
# snapshot -> start -> load -> walk 10 s -> status/vtables -> collect -> stop
./tools/ingame-harness.sh run --walk 10 --cmd status --cmd vtables

# the same stages by hand, when you want to look between them
./tools/ingame-harness.sh snapshot
./tools/ingame-harness.sh start
./tools/ingame-harness.sh load
./tools/ingame-harness.sh walk 10
./tools/ingame-harness.sh cmd status
./tools/ingame-harness.sh collect /tmp/pa-run
./tools/ingame-harness.sh stop

# what the harness would do, without touching anything
./tools/ingame-harness.sh --dry-run run --walk 10 --cmd status
./tools/ingame-harness.sh doctor
```

`collect` writes `PushAside.log`, `PushAside.out`, `PushAside.trace`, the newest
`crash-*.log`/`CrashLogger.log`, `skse64_loader.log`, a thread sample on a hang,
and a `MANIFEST.txt` with the DLL's `sha256` and the observed counters.

### How it launches (and why)

The game dir's `skse64_loader.exe` is a **symlink** into the Amethyst mod dir.
Wine resolves it, so `GetModuleFileName` returns the mod dir and the loader
aborts with `Couldn't find SkyrimSE.exe. You have installed the loader to the
wrong folder.` `start` replaces the symlinks (`skse64_loader.exe`,
`skse64_1_7_104.dll`, and `skse64_steam_loader.dll` when present) with real
copies, records the original targets, and `stop` recreates the symlinks.
`fix-links` / `restore-links` do the same thing on their own.

The launcher is chosen from the app's real Steam launch options: this install's
`489830` options are `DXVK_HDR=1 PROTON_ENABLE_WAYLAND=1 PROTON_ENABLE_HDR=1
gamemoderun %command%`, which do **not** mention SKSE, so `steam -applaunch
489830` would run `SkyrimSE.exe` without SKSE. The harness therefore uses
`protontricks-launch --appid 489830 <game>/skse64_loader.exe`. Override with
`PA_LAUNCH_CMD='...'` if that ever changes.

### Caveats

- **Focus is mandatory.** Skyrim does not step Havok while its window is
  unfocused, so an unfocused run silently stalls. `start` activates the window
  through KWin (`kdotool windowactivate`) and refuses to continue unless it can
  confirm the window is active. `walk` fails if the plugin's `constraints=`
  counter does not advance.
- **Saves are restored.** `snapshot` tars the Proton-prefix `Saves` directory to
  a run-scoped archive; `restore` mirrors it back with `rsync -a --delete`, so
  autosaves created during a run do not survive. If the run archive is missing,
  `restore` falls back to the orchestrator's full `~/pa-saves-backup-*` and says
  so. Steam Cloud for this appid is **disabled**, so there is no cloud-sync
  conflict; the restore exists purely so the user's 270-file save history is
  never altered.
- **`kdotool`/`dotool` come from nix.** They are taken from `PATH` when present,
  otherwise from `nix shell nixpkgs#kdotool` / `nixpkgs#dotool`. They are never
  installed system-wide. `dotool` needs write access to `/dev/uinput` (already
  granted to this user).
- **Hang safety.** A previous session hung with the plugin's counters frozen and
  the main thread spinning. Every wait has a timeout; a freeze longer than
  `PA_FREEZE_SEC` (default 8 s) grabs a `ps -L`/gdb `info threads` sample,
  collects the evidence, kills the game and reports. `run` also has a hard
  global timeout (`--timeout`, default 900 s) and always restores the saves.
- The harness never touches the deployed `PushAside.dll` or `PushAside.ini`; the
  caller manages those.

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
  SimGuard.{h,cpp}        PURE physics-stall watchdog + mutating-command and
                          request-refusal predicates - Linux-tested
  GameState.{h,cpp}       checked RE::Main gameActive accessor
  ProxyAccess.{h,cpp}     AsProxyController / PlayerController / PlayerProxy
  ProxyRegistry.{h,cpp}   main-thread actor<->proxy snapshot behind a seqlock
  PushRegistry.{h,cpp}    per-target push buffer (fixed table, spinlocked)
  StaggerQueue.{h,cpp}    SPSC ring: callback queues a stagger, main thread drains
  PushModel.{h,cpp}       Path 1 (character) + Path 2 (rigid body) model adapter
  PushListener.{h,cpp}    hkpCharacterProxyListener subclass (real overrides, orphan check)
  PushManager.{h,cpp}     vtable-checked attach, re-attach, calibration line
  LiveConfig.{h,cpp}      PURE live config override (seqlock snapshot + key table) - Linux-tested
  CommandParse.{h,cpp}    PURE PushAside.cmd parser - Linux-tested
  TraceFormat.{h,cpp}     PURE trace header/row formatter - Linux-tested
  CommandChannel.{h,cpp}  main-thread PushAside.cmd poll + dispatch -> PushAside.out
  TraceChannel.{h,cpp}    main-thread PushAside.trace writer
  PushRequest.{h,cpp}     push request/result slots; applied on the main thread
                          under world->worldLock
  CommandTail.h           pure append/truncate/rewrite tailer for PushAside.cmd
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
