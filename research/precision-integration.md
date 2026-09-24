# Using Precision to make PushAside move NPCs — integration report

**Status:** research only. No repository, game install, mod manager, modlist or plugin file was
modified. Precision was cloned read-only to `/tmp/precision`.

**Source examined:** `https://github.com/ersh1/Precision`, default branch `main`, HEAD
**`9ef45e0d8d9e3cf8b2df444f05142c1e7bf41cd4`** ("Version 2.0.6", committed 2026-09-01).
Every `file:line` citation below is against that commit unless the line says otherwise; each
historical claim names the commit it was read at. Full history was fetched (`git fetch --unshallow
--tags`); the repo has **no git tags and no GitHub Releases** (`git tag -l` → empty, GitHub
`/releases` → `[]`), so "release" == a commit whose message is `Version X.Y.Z` and whose
`CMakeLists.txt` `project(... VERSION X.Y.Z)` matches.

Confidence tags are used: **[read]** = read directly in the source at the cited line,
**[inferred]** = a reasoning step over read facts, **[unverified]** = cannot be settled off-game.

---

## 1. Version reality

### 1.1 Latest release whose source is on GitHub

**2.0.6**, commit `9ef45e0d8d9e3cf8b2df444f05142c1e7bf41cd4`, dated 2026-09-01. [read]
- `CMakeLists.txt:5` → `VERSION 2.0.6`; `git log -1 --format=%s` → `Version 2.0.6`.
- `git ls-remote origin HEAD refs/heads/main` → `9ef45e0…` for both, so `main` (the GitHub default
  branch) *is* 2.0.6. There is no separate release branch and no tag to fetch.
- GitHub reported `"default_branch":"main"`, `"archived":false`, `"pushed_at":"2026-09-01T07:56:18Z"`.
- Nexus was not machine-readable (JS-only page), so "Nexus may ship something newer than GitHub" is
  **[unverified]**; nothing on GitHub is newer than 2.0.6. The user's 2.0.6 therefore matches
  GitHub HEAD exactly.

The whole released line, in order:
`1.0.0, 1.0.1, 1.0.2, 1.1.0 … 1.1.8, 2.0.0, 2.0.1, 2.0.2, 2.0.3, 2.0.4, <"fixed compilation
issues">, 2.0.5, 2.0.6`.

### 1.2 Interface versions and exactly which methods exist

The enum `PRECISION_API::InterfaceVersion` (`src/PrecisionAPI.h:12-19`) is **append-only across the
whole history**:

| release line | enum values | `IVPrecision…` present |
|---|---|---|
| 1.0.0–1.0.2 (`d8fe3e8`,`b44e696`,`687ba79`) | `V1` | 1 |
| 1.1.0–1.1.2 (`9473a42`,`5b58079`,`c83671a`) | `V1 V2` | 1–2 |
| 1.1.3–1.1.8 (`702428b`…`7164ef7`) | `V1 V2 V3` | 1–3 |
| **2.0.0–2.0.6** (`ecff7bb`…`9ef45e0`) | `V1 V2 V3 V4` | 1–4 |

The `InterfaceVersion` numeric values never changed: `V1=0, V2=1, V3=2, V4=3` (`src/PrecisionAPI.h:12`).

`IVPrecision1` (callbacks + `GetAttackCollisionCapsuleLength`), `IVPrecision2` (weapon collision
callbacks + **`ApplyHitImpulse`**), `IVPrecision3` (filter setup, contact listener,
`IsActorActive`/`IsActorActiveCollisionGroup`/`IsActorCharacterControllerHittable`/
`IsCharacterControllerHittable`/`IsCharacterControllerHittableCollisionGroup`, `ToggleDisableActor`),
`IVPrecision4` (`AddPrecisionLayerSetupCallback`/`Remove…`, `GetOriginalFromClone` ×2, and
**`ApplyHitImpulse2`**) — all read from `src/PrecisionAPI.h:141-365` at 2.0.6.

### 1.3 `ApplyHitImpulse2` in 2.0.6 — exact signature/tag

Exists. It is the **last** virtual of `IVPrecision4` (`src/PrecisionAPI.h:365`), declared exactly:

```cpp
virtual void ApplyHitImpulse2(
    RE::ActorHandle a_targetActorHandle,
    RE::ActorHandle a_sourceActorHandle,
    RE::hkpRigidBody* a_rigidBody,
    const RE::NiPoint3& a_hitVelocity,
    const RE::hkVector4& a_hitPosition,
    float a_impulseMult) noexcept = 0;
```

The old one is `IVPrecision2`, marked deprecated (`src/PrecisionAPI.h:242`):
`ApplyHitImpulse(ActorHandle a_actorHandle, hkpRigidBody*, const NiPoint3& hitVelocity, const
hkVector4& hitPosition, float impulseMult)` — note: **no source handle**, and it hard-codes
`bAttackerIsPlayer = true` (`src/ModAPI.cpp:119-122`).

`ApplyHitImpulse2` exists in **2.0.0 through 2.0.6** (counted `ApplyHitImpulse2` per commit:
`ecff7bb`, `567978e`, `7261007`, `017eda3`, `e094209`, `df3cd22`, `3b15cbb`, `9ef45e0` all = 2
occurrences; every 1.x commit = 0). So a build against the 2.0.6 header is ABI-correct against an
installed 2.0.0–2.0.6.

I diffed the full headers: `src/PrecisionAPI.h` 2.0.0 vs 2.0.6 differs **only** in whitespace and
in the inline `RequestPluginAPI` helper's module-handle API (`SKSE::WinAPI::GetModuleHandle` →
`REX::W32::GetModuleHandle`); `src/ModAPI.h` differs only in whitespace. **The `IVPrecision4` vtable
order and member layout are byte-identical across 2.0.0–2.0.6.** [read, from `git diff ecff7bb
9ef45e0 -- src/PrecisionAPI.h src/ModAPI.h`]

### 1.4 What requesting V4 does on an older install — this is the mismatch trap

The export `RequestPluginAPI` at `src/main.cpp:125-145` is a `switch` on the requested enum:

* **2.0.0–2.0.6** (`src/main.cpp:131-141`): `V1,V2,V3,V4` all fall through to
  `return PrecisionInterface::GetSingleton()` — the singleton *is* an `IVPrecision4`
  (`src/ModAPI.h:27`, `class PrecisionInterface : public InterfaceVersion4`). Requesting V4 returns
  a pointer whose `ApplyHitImpulse2` slot is valid.
* **1.1.x** (`git show 7164ef7:src/main.cpp`): only `V1,V2,V3` cases exist. `V4` (numeric 3) falls
  off the switch → `return nullptr`. **Requesting V4 against 1.1.8 fails closed, not by crashing.**
* **1.1.0–1.1.2** and **1.0.x**: same, only `V1,V2` / `V1` cases → `nullptr` for V4.

This is the concrete mechanism behind "default-branch header vs older install": an API written
against the 2.x header that blindly treats a non-null return as `IVPrecision4*` is safe on 2.x and
gets `nullptr` on 1.x. There is **no version query method** in `IVPrecision1..4` (I enumerated every
virtual in `src/PrecisionAPI.h:141-365`: no `GetVersion`/`GetInterfaceVersion`), so the *only*
runtime mismatch signal is whether `RequestPluginAPI(V4)` returns null. [read]

> Note on the previous attempt: the premise "targeted the default branch and mismatched a 2.0.0
> install" cannot be explained by an ABI difference — 2.0.0 and 2.0.6 are vtable-identical and both
> accept V4. The plausible mismatch was (a) requesting a version the *installed* DLL did not know
> (a 1.x install, which returns null), or (b) treating `RequestPluginAPI`'s return as the *value*
> of the requested version rather than the singleton. I could not reproduce the exact former
> failure from the source; treat the root cause of that specific incident as **[unverified]**.

### 1.5 Which interface our plugin should request for a 2.0.6 install, and forward-compat

* **Request `InterfaceVersion::V4` unconditionally.** It returns `ApplyHitImpulse2` on the user's
  2.0.6 and fails closed (`nullptr` → clean refusal) on any 1.x.
* On later updates: `InterfaceVersion` is append-only and `IVPrecision4` is a prefix of any future
  interface, so requesting V4 should keep working even when a `V5` appears. Re-vendoring is only
  needed if we want a *new* method (e.g. a future query). This is the standard mod-API contract and
  is **[inferred]** from the observed append-only history, not promised in-comment.
* Because the API cannot report its own version, report the **actual installed version from the
  DLL's PE version resource** (or the DLL bytes' SHA-256) alongside the requested interface. The
  current WIP (`src/PrecisionBridge.cpp:86`) stores `static_cast<int>(InterfaceVersion::V4)` = `3`
  in `PrecisionStatus::version`, which is the *requested* version number, not the installed one —
  it will happily print "V4" for any install that returns non-null. **That field is a reporting
  bug, not a safety bug** (the null check is what keeps it safe).

### 1.6 Exact acquisition and graceful failure

`src/PrecisionAPI.h:368-386` provides the sanctioned inline helper:

```cpp
typedef void* (*_RequestPluginAPI)(const InterfaceVersion);
inline void* RequestPluginAPI(InterfaceVersion v = V4) {
    auto h = REX::W32::GetModuleHandle("Precision.dll");
    auto f = (_RequestPluginAPI)REX::W32::GetProcAddress(h, "RequestPluginAPI");
    return f ? f(v) : nullptr;
}
```

The correct discipline (already the shape of the WIP `src/PrecisionBridge.cpp:27-59`):

1. `GetModuleHandle("Precision.dll")`; if null → reason `"Precision.dll is not loaded"`.
2. `GetProcAddress(h, "RequestPluginAPI")`; if null → reason `"export missing"`.
3. Call `f(InterfaceVersion::V4)`; if null → reason `"Precision refused InterfaceVersion::V4"`.
4. Cache the non-null pointer; **retry while null**, because Precision may load after PushAside
   (`src/PrecisionBridge.cpp:52`). Do not rely on `kMessage_PostLoad` alone — the vendored header
   itself warns it "seems to be unreliable for some users" (`src/PrecisionAPI.h:372-374`).
5. On refusal, do not write any Havok state; the other PushAside push modes remain available, so
   `precision` is a soft-fail mode, not a hard dependency.

---

## 2. Which call gives a directional, non-disruptive push

All paths from `ApplyHitImpulse2` were read end to end:

```
IVPrecision4::ApplyHitImpulse2                       ModAPI.cpp:218-222
  -> PrecisionHandler::ApplyHitImpulse(refHandle, rb, hitVel, hitPos,
                                       impulseMult, /*activeRagdoll=*/true,
                                       /*attackerIsPlayer=*/(source==0x100000))
```

Key finding up front: **`ApplyHitImpulse2` hard-codes `a_bIsActiveRagdoll = true`**
(`src/ModAPI.cpp:221`). That flag only controls (a) the feet-proximity attenuation inside
`CalculateHitImpulse` and (b) the fact that it is this path (not the raw one) that can add the
ragdoll. It does **not** mean "the target must already be ragdolled".

### 2.1 What `ApplyHitImpulse` does with the arguments

`PrecisionHandler::ApplyHitImpulse` (`src/PrecisionHandler.cpp:268-319`):

1. **Target refr/handle.** `a_refHandle.get()` must be non-null; if it is an `Actor` and
   `!IsRagdollAdded(actorHandle)`, it:
   - refuses if `actor->IsPlayerRef() && Utils::IsFirstPerson()` (`:280-282`);
   - inserts the handle into `ragdollsToAdd` (`:286-287`);
   - queues a `DeferredImpulseJob` onto the post-Havok-hit job list (`:290`);
   - **returns** — nothing is applied this frame. The push is inherently *deferred by at least one
     frame on first use* (`:291`).
2. **If the ragdoll exists** (or on the deferred re-entry with `a_bIsDeferred=true`), it walks the
   actor's ragdoll drivers (`Utils::ForEachRagdollDriver`, `:299`), and for each `ActiveRagdoll`:
   - sets `ragdoll->impulseTime = Settings::fRagdollImpulseTime` (**0.75 s default**,
     `src/Settings.h:264`), which is what keeps the ragdoll blend alive and is re-decremented each
     frame (`src/Hooks.cpp:1270-1272`);
   - queues a **linear** impulse at each body up to 3 ragdoll constraints from `a_rigidBody`, scaled
     by `fHitImpulseDecayMult1/2/3 = 0.225/0.125/0.075` (`src/PrecisionHandler.cpp:306-313`,
     `src/Settings.h:217-219`);
   - queues a **point** impulse on `a_rigidBody` itself at `a_hitPosition`
     (`src/PrecisionHandler.cpp:318`).
3. The impulse jobs are `PointImpulseJob` / `LinearImpulseJob` (`src/PrecisionHandler.h:296-385`).
   Each checks: ref handle still valid, `Utils::FindRigidBody(refr->Get3D(), rigidBody)` (**the body
   must be findable under the actor's 3D scene graph**), `Utils::IsMoveableEntity(rigidBody)`
   (**motion type must be dynamic/sphere/box/thin-box**, `src/Utils.h:245-253`), then
   `hkpEntity_Activate(rigidBody)` and `CalculateHitImpulse` → `ApplyLinearImpulse`/
   `ApplyPointImpulse`. If the target has hitstop, the linear job returns `false` and is retried
   after the hitstop ends (`src/PrecisionHandler.h:352-366`), and the impulse timer is refreshed.

### 2.2 What `a_hitVelocity`, `a_hitPosition`, `a_impulseMult` do

`PrecisionHandler::CalculateHitImpulse` (`src/PrecisionHandler.cpp:383-437`) — [read], with the
last step **[inferred]** from standard Havok impulse→velocity identity:

- Rejects `motion.type == kKeyframed` outright (`:385-387`). **A Precision clone body
  (`kPrecisionBody`, clone motion forced to `kKeyframed` at `src/Hooks.cpp:1991-1997`) is
  therefore inert as an impulse target.** So is the NPC character-controller body, which is
  `kCharacter` (established context; `IsMotionTypeMoveable` excludes it, `src/Utils.h:245-253`).
- `mass = 1 / inertiaAndMassInv.w` (`:389-390`).
- `impulseStrength = clamp(fHitImpulseBaseStrength + fHitImpulseProportionalStrength * pow(mass,
  fHitImpulseMassExponent), fHitImpulseMinStrength, fHitImpulseMaxStrength)` =
  `clamp(1 - 0.15·√mass, 0.2, 1.0)` with the defaults (`src/Settings.h:210-214`). Heavier bones
  attenuate the impulse.
- `a_outImpulse = a_hitVelocity; impulseSpeed = a_outImpulse.Unitize();` — so **`a_hitVelocity` is
  a direction × speed vector and `impulseSpeed` is its magnitude** (Havok `unitize` returns the
  original length; **[inferred]** — this exact CLNG revision's `hkVector4::Unitize` was not found in
  the vendored CLNG, so this rests on Havok semantics, not a line I read).
- If `a_bIsActiveRagdoll` and the body's owner is an `ActorCharacter`, it attenuates the speed when
  the hit node's world AABB bottom is near the actor's feet (`:399-414`, threshold 20 units,
  `src/Settings.h:277`) — a feet hit only nudges.
- `impulseSpeed = min(impulseSpeed, fHitImpulseMaxVelocity / timeMult)`,
  `fHitImpulseMaxVelocity = 1500` Skyrim units/s (`:427`, `src/Settings.h:215`).
- `outImpulse *= impulseSpeed * g_worldScale * mass; *= impulseStrength; *= a_impulseMult;`
  (`:428-431`). The `* g_worldScale * mass` step is exactly the conversion from a velocity to an
  impulse, so **[inferred]** the resulting body velocity change is
  `Δv ≈ |a_hitVelocity| · impulseStrength · a_impulseMult` (in the same units as
  `a_hitVelocity`), with the direction of `a_hitVelocity`. Off-centre torque comes from
  `ApplyPointImpulse` at `a_hitPosition`.
- Downward component is halved (`fHitImpulseDownwardsMultiplier=0.5`, `:434`).

**Consequences for our arguments:**
- `a_hitVelocity` = `dir · dv`, where `dv` is our model's desired velocity change in **Skyrim
  units/s**. This maps directly onto our model; `impulseStrength` will shave it (0.2–1.0), so the
  realised Δv is ≤ our requested `dv`.
- `a_impulseMult` = our strength/gate scale (`Config::pushScale`, default 1.0). A **normal weapon
  hit uses `impulseMult = fHitImpulseBaseMult = 1.0`** (`src/PendingHit.cpp:277`); block `0.6`,
  power attack `2.0`, kill `1.0` (`src/Settings.h:204-208`). So **sane range 0.1–1.0 for a shove**,
  with `dv` in the ~50–500 u/s band for a humanoid; there is a hard 1500 u/s input clamp.
- `a_hitPosition` is a Havok-space point in the same units as `rb->motion.motionState.transform.
  translation`; it only sets the torque of the point impulse. Using the body's own translation is
  legal but produces a centre hit; a torso point would look more like a shove.

### 2.3 Which rigid body to pass

**A moveable ragdoll body of the target — the engine bump body is not usable.**
- `PointImpulseJob`/`LinearImpulseJob` require `FindRigidBody(refr->Get3D(), rb)` and
  `IsMoveableEntity(rb)` (`src/PrecisionHandler.h:352-360`, `:368-376`).
- The body named by the engine's bump record is the target's `bhkCharRigidBodyController` body
  (`ctrl->GetRigidBody()`), whose motion type is `kCharacter` — not "moveable" — and which is not a
  node of the ragdoll root. It would be skipped by every job.
- The correct choice is a body from `driver->ragdoll->rigidBodies` (`hkaRagdollInstance`, CLNG
  `include/RE/H/hkaRagdollInstance.h:19`). The WIP's `ParallelRagdollRootBody`
  (`src/PrecisionBridge.cpp:66-87`) returns `driver->ragdoll->rigidBodies[0]` (pelvis) — correct
  and sufficient. Before the first push these bodies are keyframed, but `AddRagdollToWorld` +
  `ModifyConstraints` set every ragdoll body to `kDynamic` (`src/Hooks.cpp:1117-1132`), and the
  deferred job runs *after* the add, so by impulse time they are moveable. [read+inferred]
- Better-looking option **[unverified, game-only]:** pick the ragdoll body whose world position is
  nearest the contact/bump point, or a fixed torso bone, so the point impulse torques the chest
  rather than the pelvis. Functionally the root works.

### 2.4 Does the target need to be an "active actor"? Does it activate a ragdoll?

- **No active-actor requirement.** `ApplyHitImpulse` never calls `IsActorActive` or
  `IsActorCharacterControllerHittable`; those are about the *clone-skeleton* map (`activeActors`,
  `src/PrecisionHandler.cpp:1890-1896`), a different subsystem. What it needs is for the target to
  be **processable by Precision's Havok update loop**: `IsRagdollAdded` (the ragdoll was added) or
  the ability to enqueue it. The ragdoll add path requires `CanAddToWorld`
  (`src/Hooks.cpp:844-878`): a live animation graph manager, `kProcessMe`, and a non-null
  `ragdollDriver->ragdoll`; plus the update loop's `bShouldAddToWorld` (alive, not disabled, not
  player-disabled, position within `fActiveActorDistance = 4000` units of the player,
  `src/Hooks.cpp:411-416`, `src/Settings.h:222`). [read]
- **It activates the ragdoll itself.** This is the whole point: `ApplyHitImpulse` queues
  `ragdollsToAdd` and `AddRagdollToWorld` (`src/Hooks.cpp:467-474`, `:948-1017`) does
  `graph->AddRagdollToWorld()`, `ModifyConstraints`, `SetRagdollConstraintsFromBhkConstraints`.
  We do not need to prepare anything; we only need to pass a valid ragdoll body. [read]
- `IsActorActive(handle)` is nevertheless a useful **pre-flight readiness probe**
  (`src/ModAPI.cpp` → `PrecisionHandler.cpp:1890`): true ⇒ the actor is near/alive and *has* a
  ragdoll interface, which is the same precondition the ragdoll add needs. It is about the clone,
  so a false is not a guarantee of failure, but it is a cheap gate. [inferred]

### 2.5 Damage, hitstop, knock state

- **No damage.** `ApplyHitImpulse2`/`ApplyHitImpulse` do not touch health, `HitData`, or the hit
  pipeline. Damage in Precision lives in `PendingHit.cpp` and is applied by the game's own hit
  before `ApplyHitImpulse` is reached (`src/PendingHit.cpp:303`). The API path is impulse only.
  [read]
- **No hitstop.** `ApplyHitImpulse` never calls `AddHitstop`; it only *defers* when a hitstop is
  already active (`src/PrecisionHandler.h:352-366`). [read]
- **No knock state is set.** Precision only *reads* `GetKnockState()`
  (`src/Hooks.cpp:1277`, `src/Hooks.cpp:1250`) and never calls any `SetKnock*`/`SetRagdollState`
  (grep over `src/` for `SetKnockState|SetKnock|SetRagdollState|SetInRagdoll` → zero hits). [read]
- **It is self-reversing for a standing actor.** `PreDriveToPose` computes
  `bActorInRagdollState = actor->IsInRagdollState()` (`src/Hooks.cpp:1330`); while the engine state
  is *not* ragdoll, once `impulseTime <= 0` it starts `kRagdollToAnim` over
  `fBlendOutTime = 0.05 s` and drops back to `kBlendOut` (`src/Hooks.cpp:1332-1340`). The
  anim→ragdoll entry blend is `fBlendInTime = 0.05 s` (`src/Hooks.cpp:979`, `src/Settings.h:245`).
  So a push on a standing, in-combat follower should be a short ragdoll overlay that returns to
  animation, *not* a knockdown that leaves them prone. [read mechanism; the "follower keeps
  following" consequence is **[inferred]/[unverified]**]
- **Counter-example we must respect:** the ordinary hit path deliberately **skips** alive targets
  already in ragdoll state — `if (!bIsInRagdollState || bIsDead)` with the comment "don't apply
  impulse to ragdolled alive targets because they won't be able to get up when regularly hit"
  (`src/PendingHit.cpp:292-303`). Since `ApplyHitImpulse2` cannot set `bIsActiveRagdoll=false`, the
  same rule must be enforced by **our** gate: do not call it on a target that
  `IsInRagdollState()` or `IsDead()`. [read]

### 2.6 Version differences in behaviour

None found between 2.0.0 and 2.0.6 for this call: `ApplyHitImpulse`, `CalculateHitImpulse`,
`AddRagdollToWorld`, `PreDriveToPose` and the default constants are unchanged in the range
(spot-checked `git show ecff7bb:src/PrecisionHandler.cpp`). Pre-2.0.0 has no `ApplyHitImpulse2` at
all, so there is nothing to compare. [read]

---

## 3. Detection options using Precision

Precision's relevant collision machinery:

* **Clone bodies:** every actor within `fActiveActorDistance` gets its skeleton cloned
  (`CloneSkeleton`, `src/Hooks.cpp:1969-2035`); all clone collisions are forced to
  `MotionType::kKeyframed` (`:1991-1997`), the clone root gets `clone->SetUserData(actor)`
  (`:1997`), and it is added to the world with the actor's collision group but on
  `CollisionLayer::kPrecisionBody = 57` (`src/PCH.h:123`, `src/Hooks.cpp:2012`).
* **Body layer filter:** `iPrecisionBodyLayerBitfield = 0x100000040000000` → bits **{30, 56}**
  (computed), i.e. clone bodies can collide only with `kCharController` (30) and Precision's own
  attack layer (56) (`src/Settings.h:292-293`). Note the extra rule in Precision's own filter
  comparison: char-controller–vs–clone-body is **forced to `Ignore` unless the actor's group is in
  `ragdollCollisionGroups`** (race has `kAllowRagdollCollision`) (`src/Hooks.cpp:1955-1964`) —
  so whether the *player's* clone body even generates contacts against an NPC is actor-dependent.
* **Contact listener:** Precision installs its own `hkpContactListener` on
  `world->GetWorld2()->contactListeners` (`src/Hooks.cpp:353-366`) — the **same** channel PushAside
  already measured blind to the player's phantom. `ContactListener::ContactPointCallback` calls
  third-party callbacks (`PrecisionHandler::RunContactListenerCallbacks`, `src/Havok/ContactListener.cpp:35`)
  *before* its own layer filter, then **returns immediately unless a body is on the attack/recoil
  layer** (`:49-51`). So `AddContactListenerCallback` does receive *every* contact point event,
  including body-layer ones — but the event is a rigid-body event.
* **PrePhysicsStep:** `RunPrePhysicsStepCallbacks(a_world)` runs inside `HavokHooks::PrePhysicsStep`
  (`src/Hooks.cpp:1811-1827`), which is effectively the same phase PushAside's
  `ProcessConstraintsCallback` already runs in.
* **Queries:** `IsActorActive` (clone present), `IsActorCharacterControllerHittable` (char
  controller in `hittableCharControllerGroups`, i.e. actors with no ragdoll instance, `:1904-1917`).

**Assessment.**

| channel | can it see the player pressing an NPC? | can it name the actor? | verdict |
|---|---|---|---|
| engine bump record `bumpedCharCollisionObject +0x2C8` (ours) | **yes — measured** | yes, `GetUserData()`→Actor | **keep** |
| `AddContactListenerCallback` | the player is a phantom; the list PushAside measured on this same world-listener channel never saw `pcActor/pcObject/bodyPhantomPlayer` | n/a if unseen | **not a detection channel for us** |
| player's clone body vs NPC char controller, via the contact listener | possibly — both are rigid bodies — but the filter *forces Ignore* unless the NPC race allows ragdoll collision, and the clone is keyframed/contact-disabled | `GetUserData()` resolves via `TESHavokUtilities::FindCollidableRef` | **candidate only; [unverified]** |
| `AddPrePhysicsStepCallback` manifold scan | the "other collidable" in the player's phantom manifold may be a clone body, resolvable by `FindCollidableRef` | only if the clone body resolves | **candidate, gives geometry not identity; [unverified]** |
| `IsActorActive` / `IsActorCharacterControllerHittable` | detection no | gate yes | **use as gates** |

**Recommendation: keep detection on the engine bump record.** It is the only channel measured to
name the touched character, it is independent of whether Precision is installed (so the diagnostic
modes and the plugin still work without Precision), and it avoids depending on clone bodies that
Precision may not create, may keyframe, and may filter out. Precision's contact/manifold channels
*can* add contact geometry and its `IsActorActive` query *can* act as a readiness gate, but neither
adds identity that the bump record lacks. The most useful thing Precision gives us here is the
*response*, not the detection. [inferred from the above read facts + the project's measured
negatives]

---

## 4. Integration design

### 4.1 Thread and phase

* **Detection:** main thread, unchanged. Read `bhkCharacterController::bumpedCharCollisionObject`
  (+0x2C8) via the vtable-verified player controller, `GetUserData()`→`TESObjectREFR`→`Actor`,
  reject null/player, apply the 200-unit proximity guard, publish to `BumpSlot`. This is the
  existing pipeline; Precision changes nothing here.
* **Application:** **main thread, under `world->worldLock`**, in the same place the other modes
  apply (`ProxyRegistry::MainThreadTick` → `PushRequest::ApplyPendingPushRequest`). This is both
  the project's established safe point (writing from the player's solver callback hangs the engine;
  see `research/engine-limits-and-gates.md`) and sufficient for Precision, because
  `ApplyHitImpulse2` performs **no direct Havok write** when the ragdoll is absent — it only locks
  its own containers (under its own locks) and queues jobs. Precision then runs those jobs in its
  own phases:
  - first use → `PostHavokHitJob` (`DeferredImpulseJob`) + `ragdollsToAdd`, both drained in
    `HavokHooks::ProcessHavokHitJobs` (`src/Hooks.cpp:467-474`, `:558`);
  - subsequent/steady state → `PrePhysicsStepJob`s, drained by `ProcessPrePhysicsStepJobs()` right
    after external `PrePhysicsStep` callbacks (`src/PrecisionHandler.cpp:2014-2025`, `:786-797`).
* **Do not** call `ApplyHitImpulse2` from inside a Precision callback/job:
  `ProcessPrePhysicsStepJobs` holds `prePhysicsStepJobsLock` while running jobs
  (`src/PrecisionHandler.cpp:786-790`), so a job that calls back into `QueuePrePhysicsJob` would
  self-deadlock on a non-recursive lock. Calling it from our own `AddPrePhysicsStepCallback` would
  run *before* `ProcessPrePhysicsStepJobs` (not inside it), but there is no benefit. Main thread is
  the answer.

### 4.2 Call chain (diagram in words)

```
main thread, world->worldLock
  ProxyRegistry::MainThreadTick / PushRequest::ApplyPendingPushRequest
    BumpSlot (from bumpedCharCollisionObject +0x2C8)      <- kept
      target Actor, source = PlayerCharacter
      our gates: cooldownMs, distance, direction, closing speed,
                 !IsDead, !IsInRagdollState, friendly/teammate rule
      our strength/mass model -> dir (unit), dv (u/s), scale     <- kept
      body selection: ctrl->GetRigidBody()
                      if kCharacter or not a ragdoll node ->
                         driver->ragdoll->rigidBodies[0] (pelvis)
      PRECISION_API::IVPrecision4::ApplyHitImpulse2(
          target->GetHandle(), player->GetHandle(), rb,
          NiPoint3{dir*dv}, rb->motion.motionState.transform.translation,
          scale)
        |
        +-- ragdoll not added: ragdollsToAdd.insert(handle);
        |                      QueuePostHavokHitJob<DeferredImpulseJob>; return
        |                         |
        |       HavokHooks::ProcessHavokHitJobs (next frame, main thread)
        |         AddRagdollToWorld: graph->AddRagdollToWorld(),
        |           ModifyConstraints (ragdoll bodies -> kDynamic),
        |           SetRagdollConstraintsFromBhkConstraints,
        |           state = kBlendIn (anim->ragdoll, 0.05 s)
        |         ProcessPostHavokHitJobs -> DeferredImpulseJob re-enters
        |           ApplyHitImpulse(..., bIsDeferred=true)
        |
        +-- ragdoll added: ragdoll->impulseTime = 0.75 s
                           QueuePrePhysicsJob<LinearImpulseJob>  (x bodies<=3 decay)
                           QueuePrePhysicsJob<PointImpulseJob>   (rb at hitPos)
                              |
                    HavokHooks::PrePhysicsStep -> ProcessPrePhysicsStepJobs
                       each job: FindRigidBody + IsMoveableEntity
                                 hkpEntity_Activate(rb)
                                 CalculateHitImpulse(...)
                                 motion.ApplyLinearImpulse / ApplyPointImpulse
                              |
                    PreDriveToPose: impulseTime-- ; when <=0 and actor not in
                       engine ragdoll state -> kRagdollToAnim (0.05 s) -> anim
```

### 4.3 Values that must come from the engine

| value | source | notes |
|---|---|---|
| target `RE::ActorHandle` | `bumpedCharCollisionObject->GetUserData()` → `TESObjectREFR` → `Actor::GetHandle()` | the detection identity; never the player |
| source `RE::ActorHandle` | `PlayerCharacter::GetSingleton()->GetHandle()` | only used for `bAttackerIsPlayer` (`native_handle()==0x100000`) and the player time multiplier (`src/ModAPI.cpp:220`) |
| `a_rigidBody` | the target's ragdoll body: `driver->ragdoll->rigidBodies[0]` (or a torso bone) | must be dynamic after `ModifyConstraints`; **not** the `kCharacter` controller body, **not** a clone body |
| `a_hitPosition` | `rb->motion.motionState.transform.translation` (Havok units), or a contact point | torque only |
| `a_hitVelocity` | our model: `dir · dv`, Skyrim u/s | clamped to 1500 u/s inside Precision |
| `a_impulseMult` | our model: `Config::pushScale` (or a mass-ratio-derived factor) | 1.0 == a normal weapon hit |

### 4.4 Cooldowns, limits, gates

* `pushCooldownMs` (200 ms default) — do not re-issue per frame. Note Precision's own effect lasts
  `fRagdollImpulseTime = 0.75 s`; our `maxPushDurationMs` (600 ms) does **not** control it. Either
  accept re-pushing as additive or raise our cooldown above ~0.75 s for a single clean shove.
  **[inferred]**
* Absolute gates to add specifically because of `ApplyHitImpulse2`: target `!IsDead()` and
  `!IsInRagdollState()` (mirrors `src/PendingHit.cpp:292`), and target is not the player.
* Readiness gate: `IsActorActive(handle)` (clone present ⇒ near, alive, driven by Precision's loop)
  — optional, cheap.
* Keep every existing gate: proximity (~200 u), direction/closing speed, teammate/faction, player
  alive/in control, no loading screen (`SimGuard`/`GameState`).
* Distance ceiling: Precision will silently not add a ragdoll beyond 4000 u, but our 200 u guard is
  far tighter, so this only matters as an explanation if a call appears to do nothing.

### 4.5 What the WIP already gets right and what to change

`src/PushRequest.cpp:291-321` + `src/PrecisionBridge.{h,cpp}` are already the right shape:
`RequestPluginAPI(V4)` with a null check (`PrecisionBridge.cpp:35-45`), the
`kCharacter`→ragdoll-root body substitution (`PushRequest.cpp:302-307`), and `dir*dv` as the hit
velocity. Changes to make:

1. **Do not report `applied = true`** when the ragdoll was not yet added. `ApplyHitImpulse2` either
   applies or queues; the WIP sets `res.precisionApplied = true` unconditionally on a non-null API
   (`PrecisionBridge.cpp:126-128`). Distinguish "queued (ragdoll was absent)" from "impulse jobs
   queued", e.g. via `IsRagdollAdded`-equivalent state or by reporting only that the call returned.
   Displacement is the only real proof (the harness already measures it).
2. **`PrecisionStatus::version`** should be the *installed* DLL version (PE resource), not the
   requested enum (`PrecisionBridge.cpp:86`).
3. Add the dead/ragdolled/teammate gates before the call.
4. Consider a better `a_hitPosition` (torso) than the root body translation. **[unverified]**

---

## 5. Risks, unknowns, and how to test each

| # | risk / unknown | confidence | how to test (in-game) |
|---|---|---|---|
| 1 | Does the impulse need a specific phase? | **[inferred: no]** — `ApplyHitImpulse2` queues into Precision's phases; nothing is written synchronously | call from main thread under `worldLock`; observe any displacement. A single push command, 6 s baseline / 20 s recovery, the existing harness `tools/ingame-harness.sh` |
| 2 | Does a follower keep following, and stay standing? | **[inferred yes]** from `PreDriveToPose`'s auto `kRagdollToAnim` when `!IsInRagdollState` | push a follower mid-follow; record displacement, whether it stays upright, whether `IsInRagdollState()` ever becomes true, whether it resumes following/combat |
| 3 | First push is deferred ≥1 frame and can no-op | **[read]** — `ragdollsToAdd` + `DeferredImpulseJob` return path | compare a single push vs two pushes; log whether the second call finds the ragdoll; confirm displacement appears 1–2 frames after the command |
| 4 | Target is not processable (not high-process / no ragdoll interface, e.g. some creatures) | **[read: condition exists]** | push an actor with no ragdoll interface (and the player) and confirm clean refusal / no displacement, no crash |
| 5 | Collision-layer interference: our own bodies/step-listener vs Precision's cloned bodies | **[unverified]** — clone bodies are `kKeyframed` on layer 57 and are force-ignored vs char controllers unless the race allows ragdoll collision (`Hooks.cpp:1955-1964`) | re-run the existing manifold probe with Precision installed and identify the "other collidable" (`FindCollidableRef`); confirm no new unexpected contacts; confirm `StepListen` on a `kCharacter` body still attaches |
| 6 | Precision absent or a different version | **[read]** — null return on 1.x; identical vtable on 2.x | run with Precision removed (expect clean `precision` refusal with reason); if a 1.1.8 DLL is available, confirm null return; run with 2.0.6 (expect the call) |
| 7 | Target has hitstop → impulse deferred | **[read]** — `LinearImpulseJob` returns false and retries | not a failure; verify the push still lands after hitstop |
| 8 | `a_hitPosition`/body choice produces spinning or foot-level hits | **[unverified]** | visual A/B: pelvis vs torso point; compare rotation and fall behaviour |
| 9 | Killing/ragdoll state: repeated pushes on a downed actor | **[read]** — ordinary path explicitly avoids ragdolled-alive targets | our gate should prevent it; verify a downed follower is never pushed |
| 10 | Slopes/stairs/furniture: ragdoll blend can look wrong or clip | **[unverified]** | push on stairs, a slope, and while the target uses furniture (Precision refuses to remove a ragdoll while `GetOccupiedFurniture()`, `Hooks.cpp:1099-1103`) |
| 11 | `pushScale`/`dv` magnitude calibration | **[inferred]** from `impulseStrength` (0.2–1.0) and the 1500 u/s clamp | sweep `dv ∈ {50,150,300,500}` and `pushScale ∈ {0.25,0.5,1.0}`; record displacement to find the knee |

**Only settleable in-game:** whether the impulse actually displaces a standing NPC (the entire
premise), whether a follower keeps following and stays upright, the collision-layer side effects,
and the torque/magnitude calibration. Everything else in this report is source-read.

---

## 6. What we keep vs what Precision does

**Precision does for us**
* Ragdoll activation (adding the ragdoll to the world, `AddRagdollToWorld`, `ModifyConstraints`,
  `SetRagdollConstraintsFromBhkConstraints`).
* The reversible anim↔ragdoll blend (`Blender`, `fBlendInTime`/`fBlendOutTime` = 0.05 s).
* The impulse-time window (`fRagdollImpulseTime` = 0.75 s) and its decay across the constraint
  graph (`fHitImpulseDecayMult1/2/3`).
* The mass-dependent impulse strength, the max-velocity clamp, the downward-component damping, and
  the feet-proximity attenuation (`CalculateHitImpulse`).
* The job scheduling that applies impulses in the correct Havok phase
  (`PrePhysicsStepJob`/`PostHavokHitJob`).
* Eased constraints (`hkpEaseConstraintsAction`) and the world-from-model fade in/out.

**Stays ours**
* Detection (the engine bump record and its resolution to an `Actor`).
* The strength/mass model (`Config::playerMass`, `defaultCharacterMass`, mass-ratio clamps,
  `pushScale`) → `dir`, `dv`, `scale`.
* Safety gates: proximity, direction, closing speed, cooldown, dead/ragdolled/teammate rules,
  ready/in-control.
* The body-selection adapter (engine bump body → ragdoll body) and the version/compat layer.
* The live command channel (`PushAside.cmd` → `PushAside.out`, `PushAside.trace`) and the
  unattended in-game harness.
* All other push modes and diagnostics, which must keep working with Precision absent.

---

## Appendix: confidence summary and open questions

* **[read]** version map, enum values, `ApplyHitImpulse2` existence/signature, `RequestPluginAPI`
  switch behaviour on 1.x vs 2.x, `ApplyHitImpulse` structure, ragdoll add/remove, blend-back, no
  damage/hitstop/knock-set, layer bitfields, clone keyframing, contact-listener early return.
* **[inferred]** `Unitize` returns the length (⇒ `a_hitVelocity` maps ~linearly to Δv); the
  main-thread call is phase-safe because Precision only queues; `IsActorActive` is a good readiness
  probe; follower keeps following.
* **[unverified]** Nexus version vs GitHub; the exact 2.0.0-mismatch root cause; whether clone-body
  contacts are visible; whether a visible, non-disruptive push actually results; everything in the
  "only in-game" list above.

**Open questions for the next in-game run:**
1. Does one `ApplyHitImpulse2` on a stationary guard produce net displacement > 0, and how large?
2. Does the guard's `IsInRagdollState()` stay false throughout, and does he stay upright?
3. Does a follower resume following immediately? Does combat state change?
4. With Precision installed, does the existing manifold probe now resolve the "unregistered
   collidable" to a Precision clone body via `TESHavokUtilities::FindCollidableRef`?