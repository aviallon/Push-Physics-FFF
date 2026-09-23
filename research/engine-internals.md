# Engine internals — Skyrim AE 1.7.104 / CommonLibSSE-NG v8.0.0

Recon for a physics-based NPC-pushing SKSE plugin. Everything below is read
only: headers were read, the symbol map was grepped, and the id→RVA and vtable
slots were **verified by reading the actual `SkyrimSE.exe`** with HeapSentinel's
PE/AddressLibrary readers (`tools/gen-hooktable.py`). No game file was modified.

Sources:

| what | path |
|---|---|
| headers | `/home/aviallon/Projects/Skyrim-A-Pose-Fix/lib/commonlibsse-ng/include/RE` (v8.0.0-4-g98c8df05f) |
| symbol map | `/home/aviallon/.local/share/skyrim-crash-tools/skyrimae.rename` (42122 lines, `<AE-id> <symbol>`) |
| address library | `…/Skyrim Special Edition/Data/SKSE/Plugins/versionlib-1-7-104-0.bin` (Format 5, 565 759 ids) |
| binary | `…/Skyrim Special Edition/SkyrimSE.exe` (size 37 910 440, TimeDateStamp 1787588678, SizeOfImage 0x3929000) |
| verification tooling + tables | `/home/aviallon/Programing/Opensource/HeapSentinel/{tools/gen-hooktable.py,hooks/}` |

## 0. Runtime identification (important)

`include/SKSE/Version.h` in CommonLibSSE-NG v8.0.0 declares
`RUNTIME_SSE_n = (1,7,104,0)` and `RUNTIME_SSE_LATEST_AE = RUNTIME_SSE_n`.
Therefore the **second ("AE") argument of every `RELOCATION_ID(se, ae)` and the
AE element of every `REL::VariantID(se, ae, vr)` in these headers is the
1.7.104 id** — no guessing between 1.6.x and 1.7.104 is needed. This was
confirmed: `RELOCATION_ID(76421, 78260)` on `bhkCharacterController::TryMoveTo`
resolves to the same address that `skyrimae.rename` labels `78260
bhkCharacterController::sub_`, and `78302` likewise.

Format reminder used throughout:
* **RELOCATION_ID / Address-Library id** — resolvable with `REL::RelocationID(se, ae)`; RVA can be resolved from `versionlib-1-7-104-0.bin`.
* **VTABLE/RTTI id** — a separate Address-Library namespace (`Offsets_VTABLE.h`, `Offsets_RTTI.h`), used with `REL::RelocationID`.
* **struct offset** — byte offset in the given class.

---

## 1. Player → `bhkCharacterController` → `hkpCharacterProxy`

### 1.1 The API path (recommended)

```cpp
RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();      // Actor* (+TESObjectREFR)
RE::AIProcess*       proc   = player->GetActorRuntimeData().currentProcess;
RE::bhkCharacterController* ctrl = proc ? proc->GetCharController() : nullptr;

// Only the *proxy* variant owns an hkpCharacterProxy:
if (ctrl /* && it is bhkCharProxyController */) {
    auto* pc    = static_cast<RE::bhkCharProxyController*>(ctrl);
    RE::hkpCharacterProxy* proxy = pc->GetCharacterProxy();             // the real Havok object
}
```

* `Actor::GetCharController()` is **inline** in this version
  (`src/RE/A/Actor.cpp`):
  `return GetActorRuntimeData().currentProcess ? currentProcess->GetCharController() : nullptr;`
  — no RELOCATION_ID of its own.
* `AIProcess::GetCharController()` is also **inline** (`src/RE/A/AIProcess.cpp`):
  `return middleHigh ? middleHigh->charController.get() : nullptr;`
* `bhkCharProxyController::GetCharacterProxy()` (`src/RE/B/bhkCharProxyController.cpp`):
  `return static_cast<hkpCharacterProxy*>(proxy.referencedObject.get());`

### 1.2 The exact pointer path / offsets (1.7.104)

```
Actor*
  +0xE8   ACTOR_RUNTIME_DATA  (base moved 0xE0→0xE8 at 1.6.629;
                              RUNTIME_DATA_ACCESSOR_VERSIONED_EX(Actor.h:778))
  +0xF8   AIProcess* currentProcess   (runtime-data + 0x10; header comment says
                                       /* 0F0 */ but that is the PRE-1.6.629
                                       absolute offset — see note below)
AIProcess*
  +0x008  MiddleHighProcessData* middleHigh      (AIProcess.h:189)
MiddleHighProcessData*
  +0x250  NiPointer<bhkCharacterController> charController
                                                  (MiddleHighProcessData.h:184)
bhkCharacterController*            <-- returned by GetCharController()
```

**Offset caveat.** The member comments inside `RUNTIME_DATA_CONTENT`
(`/* 0E0 */`, `currentProcess /* 0F0 */`, …) are the *older* (pre-1.6.629)
absolute offsets; `RelocateMemberIfNewer` moves the whole block to `+0xE8` on
1.7.104. So `currentProcess` is `Actor + 0xE8 + 0x10 = Actor + 0xF8`. Confidence
**medium-high** (derived from the header macros); prefer the accessor
`GetActorRuntimeData()` and treat the raw `0xF8` as a verification TODO in game.

### 1.3 `bhkCharacterController` does NOT own the proxy

`bhkCharacterController` (`RE/B/bhkCharacterController.h`, size `0x330`) is
abstract (`GetPositionImpl`, `GetLinearVelocityImpl`, …) and contains **no**
`hkpCharacterProxy` member and no back-pointer to one. The concrete subclasses
are the only place the proxy exists:

`RE/B/bhkCharProxyController.h`:

```cpp
class bhkCharProxyController :
      public hkpCharacterProxyListener,   // 0x000  (vptr only, size 8)
      public bhkCharacterController       // 0x010  (size 0x330)
{
    hkpCharacterProxy* GetCharacterProxy() const;   // proxy.referencedObject
    bhkCharacterProxy  proxy;    // 0x340   (size 0x260)
    void*              unk5A0;   // 0x5A0
    std::uint64_t      unk5A8;   // 0x5A8
};
static_assert(sizeof(bhkCharProxyController) == 0x5B0);   // holds
```

Manual proxy extraction (no function call):

```
bhkCharProxyController*
  +0x340  bhkCharacterProxy proxy
              bhkCharacterProxy : bhkSerializable : bhkRefObject
  +0x000  vptr
  +0x010  hkRefPtr<hkReferencedObject> referencedObject   <-- the hkpCharacterProxy*
  +0x018  void* cinfo
  +0x020  bhkCharacterPointCollector ignoredCollisionStartCollector (size 0x240)
```

So **`hkpCharacterProxy* = *reinterpret_cast<void**>((uint8_t*)pc + 0x350)`
(= `pc + 0x340 + 0x10`)**. This is confirmed twice: `bhkCharacterProxy::GetWorld1()`
in the header does `auto proxy = (hkpCharacterProxy*)referencedObject.get();`,
and `bhkCharProxyController::GetCharacterProxy()` is exactly that static_cast.

### 1.4 `hkpCharacterProxy` layout (RE/H/hkpCharacterProxy.h, size 0xF0)

```
hkpCharacterProxy : hkReferencedObject, hkpEntityListener, hkpPhantomListener
 +0x20  hkArray<hkpRootCdPoint>             manifold
 +0x30  hkArray<hkpRigidBody*>              bodies
 +0x40  hkArray<hkpPhantom*>                phantoms
 +0x50  hkArray<hkpTriggerVolume*>          overlappingTriggerVolumes
 +0x60  hkVector4                           velocity
 +0x70  hkVector4                           oldDisplacement
 +0x80  hkpShapePhantom*                    shapePhantom
 +0x88  float dynamicFriction       +0x8C float staticFriction
 +0x90  hkVector4 up               +0xA0 extraUpStaticFriction +0xA4 extraDownStaticFriction
 +0xA8  float keepDistance         +0xAC keepContactTolerance +0xB0 contactAngleSensitivity
 +0xB4  int32 userPlanes           +0xB8 maxCharacterSpeedForSolver
 +0xBC  float characterStrength    +0xC0 characterMass
 +0xC8  hkArray<hkpCharacterProxyListener*> listeners      <-- append point
 +0xD8  float maxSlopeCosine       +0xDC penetrationRecoverySpeed
 +0xE0  int32 maxCastIterations    +0xE4 bool refreshManifoldInCheckSupport
```

### 1.5 Distinguishing proxy vs rigid-body characters

`Actor::GetCharController()` may also return a `bhkCharRigidBodyController`
(RE/B/bhkCharRigidBodyController.h; AE vtable ids **240580 / 240583**, RTTI id
398677) which uses an `hkpCharacterRigidBody`, not an `hkpCharacterProxy`. The
game setting `gIni_bUseCharacterRB_HAVOK` selects it for some actors. **Guard
the cast**: compare `ctrl->GetRTTI()` with `RTTI_bhkCharProxyController`
(AE 398671) or compare the vtable pointer with
`VTABLE_bhkCharProxyController` (AE ids 240558/240560). Player/NPC humanoids use
the proxy path (`bhkCharProxyController`).

---

## 2. All `hkpCharacterProxyListener` implementors in the headers

`rg "hkpCharacterProxyListener"` in `include/RE` yields exactly **one** concrete
subclass — there is no `bhkCharacterProxyListener` in this version (that name
does not exist). (`hkpCharacterRigidBodyListener`, RE/H/hkpCharacterRigidBodyListener.h,
is a different interface used only by the rigid-body controller path.)

| class | file | base offset | RTTI (AE) | VTABLE (AE) | Address-Library id in 1.7.104 |
|---|---|---|---|---|---|
| `hkpCharacterProxyListener` (interface) | RE/H/hkpCharacterProxyListener.h | — | 398668 | 240549 | vtable @ RVA 0x1A89318 |
| **`bhkCharProxyController`** | RE/B/bhkCharProxyController.h | `hkpCharacterProxyListener` @ 0x000, `bhkCharacterController` @ 0x010 | 398671 | 240558 (listener vtable), 240560 (controller vtable) | vtables @ RVA 0x1A89540 / 0x1A89578 |

### 2.1 Verified vtable for `bhkCharProxyController[0]` (AE vtable id 240558, RVA 0x1A89540)

Read directly from `SkyrimSE.exe`; the slots match the header's declared order
exactly:

| slot | name (skyrimae.rename) | AE id | RVA |
|---|---|---|---|
| 0 | `bhkCharProxyController::dtor` | 79161 | 0x1099420 |
| 1 | `bhkCharProxyController::processConstraintsCallback` | **79130** | 0x10976A0 |
| 2 | `bhkCharProxyController::contactPointAddedCallback` | **79131** | 0x10980A0 |
| 3 | `bhkCharProxyController::contactPointRemovedCallback` | **79132** | 0x1098140 |
| 4 | `bhkCharProxyController::characterInteractionCallback` | **79134** | 0x10981A0 |
| 5 | `bhkCharProxyController::objectInteractionCallback` | **79133** | 0x1098180 |

This confirms the header's comments `// 04` (CharacterInteraction) and `// 05`
(ObjectInteraction). Is the engine's own listener installed on player/NPC
proxies? `bhkCharProxyController` *is* the listener the engine installs for the
character it owns; the base subobject is at the same address as the
`bhkCharProxyController`. The proxy's `listeners` array (`+0xC8`) is a plain
`hkArray<hkpCharacterProxyListener*>` and is safe to **append** to
(`push_back` exists, `RE/H/hkArray.h:129`) as long as the listener outlives the
proxy and is not left dangling. There is **no** `addCharacterProxyListener` /
`removeCharacterProxyListener` engine symbol in these headers; mutate the array
directly. (Confidence: high on layout, medium on "engine always installs itself";
verify in game.)

---

## 3. Movement / character-vs-character collision functions

| function | header | RELOCATION_ID (se, **ae**) | AE RVA | notes |
|---|---|---|---|---|
| `bhkCharacterController::TryMoveTo` | RE/B/bhkCharacterController.h | (76421, **78260**) | 0x1063E80 | hkpWorld LinearCast sweep + position/velocity setters; prologue FNV 0xBF13EEDC2AA50D7E |
| `bhkCharacterController::ProcessHurtfulBody` | RE/B/bhkCharacterController.h | (76460, **78302**) | 0x1066C60 | engine's own character-hit reaction; prologue FNV 0x2C94B2F04299680F |
| `bhkCharacterController::IsHurtfulBody` | RE/B/bhkCharacterController.h | (76456, **78298**) | 0x1066… | **flag:** map labels 78298 `hkpRigidBody::sub_`, header groups it under the controller. Verify before hooking. |
| `hkpCharacterProxy::UpdateManifold` | RE/H/hkpCharacterProxy.h | **62647** (`Func3_`) | 0xB9F720 | vtable slot 3; called during the character step, fills `manifold`/`bodies`/`phantoms`, then dispatches listener callbacks |
| `hkpCharacterProxy::ExtractSurfaceConstraintInfo` | RE/H/hkpCharacterProxy.h | **62649** (`Func4_`) | 0xBA04D0 | vtable slot 4 |
| `hkpCharacterProxy::"Func2_"` | RE/H/hkpCharacterProxy.h | 62659 | 0xBA0E90 | vtable slot 2 (listener/`CalcContentStatistics`-adjacent; unnamed) |
| `hkpCharacterProxy` dtor | — | 62667 | 0xBA15C0 | vtable slot 0 |
| `bhkCharProxyController::characterInteractionCallback` | RE/B/bhkCharProxyController.h | **79134** | 0x10981A0 | listener slot 4; **the character-vs-character callback** |
| `bhkCharProxyController::objectInteractionCallback` | RE/B/bhkCharProxyController.h | **79133** | 0x1098180 | listener slot 5; character-vs-rigid-body callback |
| `bhkCharacterController::dtor` | — | 78342 | 0x1069680 | |
| `bhkCharProxyController::sub_` | — | 79129 | 0x1097430 | `LoadBinary`/`LinkObject`-adjacent constructor work |
| `bhkCharProxyController::dtor` | — | 79161 | 0x1099420 | |

`hkpCharacterProxy` vtable ids: **230859 / 230861 / 230863** (RVA 0x19FAE40 /
0x19FAE70 / 0x19FAEA8). `VTABLE_hkpCharacterProxyListener` AE id **240549**
(RVA 0x1A89318). `VTABLE_bhkCharacterProxy` AE id **240553** (RVA 0x1A89370).
`VTABLE_hkpMotion` AE id **227961** (RVA 0x19EB450).

**`bhkCharacterController::Update`** — there is **no** symbol by that name in the
1.7.104 map. The per-frame character step is driven through
`hkpCharacterContext` / `hkpCharacterStateManager` and the listener callbacks;
non-virtual update code is emitted as unnamed thunks (`78258`, `78261`–`78294`,
`78305`–`78310`). Do not assume a named `Update` to hook.

### 3.1 Impulse path (Havok side)

`hkpRigidBody::ApplyLinearImpulse` is **header-inline**
(`src/RE/H/hkpRigidBody.cpp`): `Activate(); motion.ApplyLinearImpulse(...)`. There is
therefore no `hkpRigidBody` RELOCATION_ID for it (note: id **74607** is
`NiMeshParticleSystem::UpdateWorldBound`, **not** an impulse function).
The real dispatch target is the virtual **`hkpMotion::ApplyLinearImpulse`,
vtable slot 0x13** (header RE/H/hkpMotion.h), AE AddressLibrary id **60970**,
RVA **0xB4F610**, vtable id 227961. Sibling virtuals:
`SetLinearVelocity` slot 0x10 (AE 60967, 0xB4F5B0),
`SetAngularVelocity` slot 0x11 (AE 60968, 0xB4F5D0),
`ApplyPointImpulse` slot 0x14 (AE 60969, 0xB4F5F0).
Alternative (no Havok hook): write `hkpRigidBody`'s `hkpMotion` directly, or use
`hkpCharacterProxy::velocity` (`+0x60`).

---

## 4. Actor / TESObjectREFR APIs for reacting physically

| API | header | RELOCATION_ID (se, **ae**) | AE RVA / notes |
|---|---|---|---|
| `Actor::IsInRagdollState()` | RE/A/Actor.h:648 | (36492, **37491**) | 0x687C20 |
| `Actor::IsStaggering()` | RE/A/Actor.h:663 | inline — reads graph var `"IsStaggering"`, else `AsActorState()->IsStaggered()` | no id |
| `Actor::PotentiallyFixRagdollState()` | RE/A/Actor.h:391 | **virtual slot 0x0B0** | `src/RE/A/Actor.cpp:1789` uses `RelocateVirtual<…>(0x0B0, 0x0B2, this)` |
| `Actor::UpdateCharacterControllerSimulationSettings(bhkCharacterController&)` | RE/A/Actor.h:390 | **virtual slot 0x0AF** | |
| `Actor::GetCharacterController` | map symbol | **37258** | 0x675310 (may be the engine's non-inlined variant) |
| `ActorProcess::GetCharacterController` | map symbol | **39856** | 0x722110 |
| `AIProcess::KnockExplosion(Actor*, const NiPoint3&, float)` | RE/A/AIProcess.h:171 | (38858, **39895**) | 0x7237C0 |
| `AIProcess::KnockParalyze(Actor*)` | RE/A/AIProcess.h:172 | (38857, **39894**) | 0x723620 |
| `Actor::Update` | map symbol | **37348** | per-actor update |
| `TESObjectREFR::SetMotionType(MotionType, bool)` | RE/T/TESObjectREFR.h:483 | inline (`src/RE/T/TESObjectREFR.cpp:1055`) → `Get3D()->SetMotionType(..., true,false,allowActivate)` | no id |
| `Actor::SetPosition(const NiPoint3&, bool)` | RE/A/Actor.h:384 | virtual slot 0x0A9 | |
| `Actor::DetachCharController` / `RemoveCharController` | RE/A/Actor.h:382–383 | virtual slots 0x0A7 / 0x0A8 | |
| `TESObjectREFR::MoveTo(TESObjectREFR*)` | RE/T/TESObjectREFR.h:468 | — | kinematic teleport, not physical |
| `Actor::StopMoving(float)` | RE/A/Actor.h | — | |

Ragdoll/stagger state is also directly readable/writable through `ActorState`
(RE/A/ActorState.h, `Actor+0xC0` on 1.7.104 via `AsActorState()`):
`actorState1.knockState` (`KNOCK_STATE_ENUM`: `kOut`/`kQueued`/`kDown`/`kGetUp`…),
`actorState2.staggered` (1 bit). `ActorState::GetKnockState()` / `IsStaggered()`.

Not found in these headers: `PushRagdoll`, `SetRagdoll`, `Stagger`, `KnockDown`,
`ApplyVelocity` as public methods on `Actor`/`TESObjectREFR` (the engine uses the
`Knock*` AIProcess helpers and the ragdoll-graph path above).

---

## 5. `characterMass` / `characterStrength` writability

Both are **plain public `float` members** of `hkpCharacterProxy`
(`RE/H/hkpCharacterProxy.h`): `characterStrength` at **+0xBC**,
`characterMass` at **+0xC0**. They are directly writable at runtime through the
header once you hold the proxy pointer; there is no setter needed.
`hkpCharacterProxyCinfo` is **not present** in the v8.0.0 headers (only named in
the map), so tuning must go through the live proxy (or a Havok class the plugin
declares itself).

**Code in the repos that already touches the proxy: none.** `rg` over
`Skyrim-A-Pose-Fix/src` and `HeapSentinel/src` + DESIGN.md for
`CharacterProxy|CharController|hkpCharacter` returned no hits. This is greenfield.

---

## 6. Rename-map matches for the requested substrings

`grep` of `/home/aviallon/.local/share/skyrim-crash-tools/skyrimae.rename`:

* **`CharacterProxy`** (54 hits) — `hkpCharacterProxy::Func3_` **62647**,
  `Func4_` **62649**, `Func2_` **62659**, `dtor_` **62667**, plus ~25
  `hkpCharacterProxy::sub_*` and ~14 `ahkpCharacterProxy::sub_*`;
  `hkpCharacterProxyCinfo::dtor_` **59840**; `bhkCharacterProxy::dtor_` **79163**,
  `bhkCharacterProxyCinfo::dtor_` **79164**, `hkpCharacterProxyListener::dtor_`
  **79165**, `bhkCharacterProxy::GetRTTI_` **79168**, `bhkCharacterProxy::Func44_`
  **79169**, `bhkCharacterProxy::LoadBinary_/LinkObject_/RegisterStreamables_/SaveBinary_`
  **80529–80532**, `CreateClone_` **80535**.
* **`CharProxy`** (15 hits) — all `bhkCharProxyController` /
  `bhkCharProxyControllerCinfo`: callbacks **79130–79134**, ctor-ish
  **79129/79136**, `dtor_` **79161**, cinfo vtables **79122–79127 / 79162**.
* **`hkpCharacter`** (82 hits) — `hkpCharacterMotion::Func5_…Func24_`
  **61743–61754**, `hkpCharacterContext::GetCharacterState_` **62427**,
  `hkpCharacterStateManager::sub_` **62444/62445**, `hkpCharacterRigidBody::Func3_…Func5_`
  **62452–62454**, `hkpCharacterRigidBody` ctor/dtor, `hkpCharacterProxy*` (above),
  `hkpCharacterRigidBodyCinfo::dtor_` **62581**, `hkpCharacterControllerCinfo::dtor_`
  **59839**, `hkpCharacterProxyCinfo::dtor_` **59840**, `hkpCharacterState*` state classes.
* **`bhkCharacter`** (110 hits) — `bhkCharacterController` ctor/sub family
  **78253, 78258, 78260, 78261–78294, 78302, 78305–78310**, `dtor_` **78342**;
  `bhkCharacterStateClimbing::sub_` **78345**; `bhkCharacterCollisionHandler::Func0_`
  **41660**; `bhkCharacterPointCollector::sub_` **60336**;
  `BSTEventSink_bhkCharacterMoveFinishEvent_::Handle_` **37998** and dtors
  **38196/38212/40284/40938**.
* **`PushCharacter`** — **0 hits.**
* **`Simplex`** — **0 hits.** (The Havok simplex solver types —
  `hkpSimplexSolverInput` etc. — exist only as forward declarations in
  `hkpCharacterProxyListener.h`; no engine symbols carry the name in the map.)

---

## 7. Recommended hook point (and how to tell the two proxies apart)

**Best hook: attach a custom `hkpCharacterProxyListener` to the *player's*
proxy — no code detour at all.** Append your listener to
`playerProxy->listeners` (`hkpCharacterProxy+0xC8`) and implement
`CharacterInteractionCallback(a_proxy, a_otherProxy, contact)` (slot 4) and,
if desired, `ObjectInteractionCallback` (slot 5). This is exactly the interface
the engine's own `bhkCharProxyController` implements, so it is the intended
extension point.

**Identifying the player's proxy inside the callback.** The listener array
belongs to `a_proxy`, so if you installed your listener on the player's proxy
then **`a_proxy` is always the player's proxy** and `a_otherProxy` is the
collided character (the NPC) — this is the clean discriminator.

If instead you **vtable-hook** `bhkCharProxyController` slot 4 (AE vtable id
**240558**, target AE id **79134**, RVA 0x10981A0), the hook fires for *every*
character (player and NPC), and `this` is the `bhkCharProxyController` base
(== listener subobject). Test ownership by comparing `a_proxy` (the proxy being
updated) with the player's:

```cpp
auto* playerCtrl = RE::PlayerCharacter::GetSingleton()->GetCharController();
const bool isPlayer = playerCtrl &&
    playerCtrl->GetRTTI() == RE::RTTI_bhkCharProxyController &&
    static_cast<RE::bhkCharProxyController*>(playerCtrl)->GetCharacterProxy() == a_proxy;
```

The proxy object itself carries **no** `userData` back-pointer of its own
(`hkpCharacterProxy` derives `hkReferencedObject`, not `hkpWorldObject`); its
`shapePhantom` (`+0x80`) is an `hkpWorldObject` with `userData` at `+0x18`,
whose contents (`bhkWorldObject*`?) are unverified — do not rely on it as the
discriminator. The only supported route from a proxy to its `Actor` is your own
proxy→Actor registry, or comparing against `PlayerCharacter::GetSingleton()`.

**Also consider hooking** `bhkCharacterController::ProcessHurtfulBody`
(AE **78302**, RVA 0x1066C60) or `AIProcess::KnockExplosion` (AE **39895**) if
you want the reaction to reuse the engine's own debris/knock behaviour.

**Separately, the main-thread tick is a function-entry detour on the leaf
`RE::Main::Update` calls as its last instruction before its epilogue**
(AE id **107306**, RVA **0x154AF70** on 1.7.104). It is *not* a push hook and not
a push mechanism: it just drives `ProxyRegistry` refresh, listener
attach/re-attach, the deferred stagger drain and the stats heartbeat once per
frame. It is verified against `hooks/skyrimse-1.7.104.0-846efccf.json` before
MinHook patches anything.

The tick deliberately does **not** hook `RE::Main::Update` (AE 36564, RVA
0x658870) itself: HDT-SMP installs a function-entry detour there, so the
committed prologue no longer matches and that hook is refused (crash
2026-09-23). Disassembling 36564 shows `call 0x14154AF70` at
`Main::Update+0xC49`, right before the epilogue; a `.text`-wide scan for the
`E8 rel32` to `0x14154AF70` finds exactly one caller, that instruction. The leaf
is `incl 0xc8(%rcx); ret`. CommunityShaders patches around
`Main::Update+0x160` and SKSE dispatches from `Main::Update+0x9A`, so those
regions are avoided. Do **not** reintroduce the first implementation's
`SKSE::GetTaskInterface()->AddTask` self-re-adding loop (see research/design.md
§3.4.1): it froze the game at the main menu.

Also do **not** call `Actor::IsInBleedout()`: it is `RELOCATION_ID(48461, 0)` and
the AE id is 0, so on 1.7.104 it resolves to a null address and the call jumps to
0 (the same 2026-09-23 crash). Read `ActorState::GetLifeState()` /
`IsBleedingOut()` instead.

### Verification hashes (HeapSentinel convention: AE target + vtable id/slot)

| target | AE id | kind | vtable id | slot | RVA | pdataExtent | prologueLen | FNV-1a-64 |
|---|---|---|---|---|---|---|---|---|
| `bhkCharProxyController::characterInteractionCallback` | 79134 | kVtable | 240558 | 4 | 0x10981A0 | 102 | 32 | 0x06DDFF66FA2C2047 |
| `bhkCharProxyController::objectInteractionCallback` | 79133 | kVtable | 240558 | 5 | 0x1098180 | null | 32 | 0x9D1E885DC8E71243 |
| `hkpCharacterProxy::UpdateManifold` | 62647 | kVtable | 230859 | 3 | 0xB9F720 | 18 | 18 | 0x3DDCC387306143DD |
| `hkpCharacterProxy::ExtractSurfaceConstraintInfo` | 62649 | kVtable | 230859 | 4 | 0xBA04D0 | null | 32 | 0xFA3B849549DBA2EA |
| `bhkCharacterController::TryMoveTo` | 78260 | kRva | 0 | 0 | 0x1063E80 | 1770 | 32 | 0xBF13EEDC2AA50D7E |
| `bhkCharacterController::ProcessHurtfulBody` | 78302 | kRva | 0 | 0 | 0x1066C60 | 745 | 32 | 0x2C94B2F04299680F |
| `hkpMotion::ApplyLinearImpulse` | 60970 | kVtable | 227961 | 0x13 | 0xB4F610 | null | 32 | 0xC0CB45878D4C5623 |
| `RE::Main::Update` | 36564 | kRva | 0 | 0 | 0x658870 | 3183 | 32 | 0xB8E269AA60C8D8A4 |
| `Main::Update` frame-tail leaf (main-thread tick) | 107306 | kRva | 0 | 0 | 0x154AF70 | null | 32 | 0x30DE54828E299079 |

(Slot 0x13 for `hkpMotion` was verified by reading the vtable: slot 19 holds
RVA 0xB4F610; slots 5–10, 18, 20 are `_purecall`, id 109686.)

---

## 8. Open items / TODO

1. Confirm `currentProcess` absolute offset `0xF8` for 1.7.104 empirically
   (derived from `RUNTIME_DATA_ACCESSOR_VERSIONED_EX(...,0xE0,0xE8)` + struct offset 0x10).
2. Confirm the engine installs `bhkCharProxyController` as an entry in the
   proxy's `listeners` array (and whether the array can be appended to live).
3. Confirm `hkpCharacterProxy::manifold` is populated after each `TryMoveTo` and
   is valid to read on the physics thread; the callback path (`UpdateManifold`,
   AE 62647) strongly suggests yes.
4. Resolve the `IsHurtfulBody` AE id discrepancy (header says 78298; map labels
   78298 `hkpRigidBody::sub_`).
5. Determine `hkpShapePhantom::userData` (`hkpWorldObject+0x18`) contents, if it
   is ever a usable proxy→owner back-pointer.
