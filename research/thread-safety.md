# PushAside thread safety audit

In-game evidence (build b8bc332): the Havok callbacks run on thread 564 while the
main-thread tick and attach run on 356. `objectInteractionCallback` logged from
564 and the tick from 356, so the two really do overlap. This audit covers every
structure reachable from a callback (`ProcessConstraintsCallback`,
`CharacterInteractionCallback`, `ObjectInteractionCallback`,
`ContactPoint*Callback`; `ProcessConstraintsCallback` also drives the manifold
scan).

| Structure | Callback access | Owner / mutation | Verdict |
|---|---|---|---|
| `PushListener::owner_` | read (orphan check) | main thread writes in `AttachTo`/`Detach` | `std::atomic<proxy*>` acquire/release |
| `PushListener` counters (`characterCalls_`, `objectCalls_`, `constraintCalls_`), `g_orphanCalls` | read-modify-write | callback | `std::atomic<uint64_t>`, relaxed |
| `Invariant` (`consecutiveFailures_`, `disabled_`) | read/write | `ProcessConstraintsCallback` only, one Havok thread | callback-confined; no sync needed |
| `Config` | read | written once by `Config::Load()` at `SKSE_PLUGIN_LOAD`, before the tick hook exists | effectively read-only; safe |
| `ProxyRegistry` (`entries_`, `size_`, `seq_`, `dialogueOpen_`, `generation_`) | `Lookup`/`ProxyForCollidable` read | main thread publishes under seqlock (odd/even `seq_`) | seqlock; reader retries on torn sequence |
| `ProxyRegistry::capacity_`, `PushRegistry::capacity_` | read | set in `Init()` before any reader | read-only after init |
| `PushRegistry` (`slots_`, `size_`) | `WithEntry` read/write under `lock_` | `Sweep`/`Clear`/`Scale` under `lock_` | spinlock + atomic `size_` |
| `StaggerQueue` (`entries_`, `head_`, `tail_`) | callback `Push` only | main thread `Pop` only | SPSC ring, atomic indices |
| `PushModel` published globals (`g_effectivePlayerMass`, gate/debug counters, thread ids, `g_unknownTargets`, `g_pairsSeen`, `g_pairCount`) | read/write | mixed; single writer each | all `std::atomic` (CAS for the identity sets) |
| `Health` | `Degrade` (rare) | any thread | atomics + mutex |
| Havok proxy `velocity` writes in `OnCharacterContact` | read/write | physics callback thread, i.e. where Havok itself mutates | intended physics mutation, not a shared-structure race |

| `PushModel::ApplyProxyTuning` is **not** reachable from a callback: it runs on
the main thread at attach / re-attach / config change. It writes
`hkpCharacterProxy::characterStrength`/`characterMass` (aligned 32-bit floats)
that the physics thread may read. This is left in place and documented: on
x86-64 the stores are single-word and never tear, so the worst case is one
physics step using the previous value, and the alternative (deferring the write
into the physics callback) cannot be done without a new detour. No other
non-atomic mutable state is touched by both the callback and the main thread.

## World contact listener (instrumentation, added on main's request)

`WorldContactListener` (`src/WorldContactListener.{h,cpp}`) is registered on the
player's `hkpWorld` and runs on the physics thread. Its counters
(`collisionAdded_`, `pcActor_`, `pcObject_`, `actorActor_`, `other_`, `logged_`,
`lastLogMs_`) are all `std::atomic<uint64_t>`; the `WorldContactListener` object
itself is a static with no mutable non-atomic state. The bounded diagnostic is
emitted at most 20 times per session / once per 5 s, so the hot path never
allocates; `lastLogMs_`/`logged_` gate it with relaxed atomics. `g_lastWorld`
(registration cache) is touched on the main thread only. The listener reads no
PushAside shared structure other than config-free constants, so nothing else is
reachable from it.

Registration takes `bhkWorld::worldLock` via `BSWriteLockGuard` and calls the
RE'd `hkpCollisionCallbackUtil_requireCollisionCallbackUtil` /
`hkpWorld_addContactListener` (`src/Hooks/HavokUtil.{h,cpp}`), each null-guarded
with a one-shot error log. They are deliberately **not** in
`Hooks/HookTargets.def`/the committed table, because that table needs a prologue
hash generated from `SkyrimSE.exe` and no game binary is available in CI.

## Changes made

- `PushListener::owner_` is now `std::atomic<RE::hkpCharacterProxy*>`.
- The manifold scan's pair identity set and count are atomics (`PushModel.cpp`).
- Every other structure already used the discipline above; no further change was
  needed, and the table records the verdict so the next reader does not have to
  re-derive it.
