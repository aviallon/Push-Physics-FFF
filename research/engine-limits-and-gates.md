# Engine limits and the gates they produced

Two hard facts about Skyrim AE drive the current architecture, and the failure modes that
revealed them are now enforced by CI gates rather than by memory. This file exists so the
next person does not have to rediscover either.

## 1. You cannot push a character by writing a velocity

**NPC bodies are `hkpMotion::MotionType::kCharacter`.** Their motion is re-derived from the
animation/AI every step, so every velocity field reachable from a plugin is an *output*, not an
input. Measured on a provably stationary target (Whiterun Guard `0x00078780`, 6 s baseline with
velocity 0.000 and position constant, `pushdry` control pass, 1 unit ≈ 1.4 cm):

| what was written | phase | read-back | net displacement |
|---|---|---|---|
| `motion.linearVelocity` (`= ctrl->SetLinearVelocityImpl`, both are `character+0x230`) | main thread, `world->worldLock` | changed to `dir·dv` | **0.000000 u** over 20 s |
| `initialVelocity` (+0xA0) + `velocityTime` (+0x220) | main thread, `world->worldLock` | persisted 20 s | **0.000000 u** |
| `outVelocity` (+0x90) **and** `SetLinearVelocity` (the engine's own input path) | inside the target's OWN `hkpCharacterRigidBodyListener::CharacterCallback` (post-simulation, 1851 calls / 51 writes) | 120 u/s, persisted 2195 trace rows | **0.000000 u**, position bit-identical |
| engine `ObjectReference::PushActorAway` → `AIProcess::KnockExplosion` | engine API | applied | 0.98 u (1.38 cm), **direction ignored**, magnitude-independent (100 vs 1000 identical) |

`outVelocity` is not folded in for a rigid-body controller: only `bhkCharacterStateOnGround` and
`...Swimming` fold `initialVelocity*velocityTime` into it. Writing it persists and nothing reads it.

**Consequences.** A single impulse cannot displace a standing `bhkCharRigidBodyController`
character at any phase reachable from SKSE. The only engine displacement is a directionless,
magnitude-independent stagger, because real knockback in this game is a *ragdoll* effect: ragdoll
bodies are dynamic and do integrate impulses, whereas the standing character body does not.

**Therefore physics is delegated.** Precision (`ApplyHitImpulse2`) already owns the ragdoll
activation / eased constraints / blend-in-out machinery that makes a reaction non-disruptive.
PushAside supplies detection, the strength/mass model, the safety gates and the harness; it does not
reimplement ragdoll physics.

### Related: the class asymmetry that made detection hard
The **player** is a `bhkCharProxyController` (a Havok phantom); **all NPCs** are
`bhkCharRigidBodyController` (no proxy at all). Hence the proxy-vs-proxy listener callback never
fires for the player, the character-proxy manifold never contains another character, and a
proxy-keyed registry silently contains only the player. Detection comes from the engine's own bump
record instead: `bhkCharacterController::bumpedCharCollisionObject` (+0x2C8) → `GetUserData()` →
`TESObjectREFR` → `Actor`, which names the bumped character.

## 2. Two ways an engine call fails *silently*, and the gates for each

**(a) A mis-typed RE'd signature.** `hkpCollisionCallbackUtil_requireCollisionCallbackUtil`
returns the util *pointer*, not `bool` (its tail is `inc word [rax+0x1c]; ret`). Reading a heap
pointer as `bool` tests its low byte — usually `0x00` — so the wrapper reported failure every tick
and returned *before* `hkpWorld_addContactListener`, with no log line: the contact channel was dead
for a whole session while every counter read a clean zero. Rule: **disassemble the tail of any
RE'd function whose return you branch on, and assert the effect (re-read the container) rather than
trusting the call.**

**(b) An unresolved Address Library id.** CommonLibSSE-NG *asserts* on it:
`REL/Relocation.h:695` is `assert(_impl != 0)` inside `Relocation::get()`. So an id absent from the
installed `versionlib` **aborts the game** (Wine shows an "Assertion failed!" dialog; CrashLogger
writes nothing because there is no fault) rather than returning null. Concrete repeat offender:
`Actor::IsInBleedout()` is `RELOCATION_ID(48461, 0)` — the AE id is 0. It was removed once in
`95f2485` and reintroduced later by new trace code, which is what makes this a *gate* problem
rather than a knowledge problem.

Gates (see `tools/check-relocations.py`, `tools/gen-clng-denylist.py`,
`tools/clng-ae-zero-ids.json`, `.clang-tidy`):

1. No relocation may be invoked (`.get()`, `operator()`, `operator->`, `invoke`, function-pointer
   cast) without an `.address() != 0` check in the same function; `// relocation-guard-ok: <reason>`
   is the only opt-out.
2. A **generated denylist** of CommonLibSSE-NG APIs whose implementation uses `RELOCATION_ID(x, 0)`
   (derived mechanically from the vendored library and committed, so a library update is caught by
   regenerating and diffing). Our sources must not call them.
3. `-Wall -Wextra -Werror` for our sources, plus `clang-tidy` (`bugprone-*`, `clang-analyzer-*`,
   `performance-*`) and `cppcheck`, and both lints are themselves tested against synthetic
   violating/compliant snippets — including the real regression as a fixture.

Both lints exist because a bug that recurs after being fixed once cannot be prevented by
documentation.

## 3. Provenance and traps worth keeping

- `hkpWorld_addContactListener` = `RELOCATION_ID(60543, 61383)`; `hkpCollisionCallbackUtil_requireCollisionCallbackUtil`
  = `RELOCATION_ID(60588, 61437)`. Verified three ways: disassembly of 1.7.104 (the push into
  `hkpWorld::contactListeners`, the `hkArray` at `+0x258` with capacity mask `0x3fffffff`), runtime
  resolved addresses matching the same module base, and structural match against the documented RVAs.
- Writing Havok state from inside the player's character-proxy solver callback **hangs** the engine
  (main thread spins in `sched_yield`; no crash log). Apply on the main thread under
  `world->worldLock` instead — the engine's own pattern for character velocities.
- A deployed DLL inside the game's pressure-vessel sandbox must be a **real file**: a symlink to a
  path outside it (e.g. `/tmp`) is not resolved and the plugin silently never loads.
- The command channel baselines its input file and never replays pre-existing commands; and
  `trace on every <n>` was once parsed as a bare `trace on` (fixed in `c5376f3`) — a silently
  ignored interval makes "how many frames did this last" unprovable.