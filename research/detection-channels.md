# Detecting "the player is touching a character" in Skyrim AE 1.7.104

**Verdict: three of the four candidate channels are structurally dead for the player, and the
fourth is an engine field nobody documents.** This file records which, why, and the evidence, so
that nobody repeats the four in-game test runs that established it.

The one-line explanation: **the player's collision is an `hkpCharacterProxy` phantom.** Phantoms
produce no world contacts and are not the character *proxies* the manifold references, so every
physics-contact channel is blind to the player. The engine nevertheless *records* the player's bump
against another character in `bhkCharacterController`, and that field is the only working signal.

| # | Channel | Verdict | Evidence |
|---|---|---|---|
| 1 | `hkpCharacterProxyListener::CharacterInteractionCallback` (vtable slot 4, AE id 79134) | **Never dispatched** for the player | `character=0` in four separate runs while colliding with Lydia. The listener *was* attached (`listener attached to player proxy 0x… (listeners now 2)`) and other slots on the same listener do fire (`object=…`, `constraints=…`). |
| 2 | `ProcessConstraintsCallback` (slot 1, AE id 79130) + manifold scan | Fires (~240/s) but the **manifold never contains a character** | `constraints=3977` while `pairs=0`. Composition probe: `manifold probe: points=1 collidables=2 registered=1 self=1 other=0` (and `points=2 … self=2`). Every point pairs the player's *own* collidable with an unregistered one — world geometry, or possibly a Precision clone body (a rigid body, which cannot match a registered character **phantom** collidable). |
| 3 | Havok world contact listener (`hkpContactListener` on `hkpWorld::contactListeners`) | Fires, reaches actor bodies, **never the player** | `contactAdded=186`, `actorActor=93`, `contactOther=93`, but `pcActor=0 pcObject=0 pcGroup=0 bodyPhantom=0 bodyPhantomPlayer=0` for the whole run — with viable controls (`groupProbeViable=true engineMapResolves=true`). |
| 4 | Engine bump record on the player's `bhkCharacterController` | **WORKS** | `player bump record: bumpedBody=0x0 … bumpedCharCollisionObject=0x865F2E00 (refr=0x000A2C94 'Lydia') bumpedForce=0.00`, repeated while the player pressed into her. |

Channel 3's zeros are worth stating precisely, because two of them *looked* like evidence and were
not: `engineMapResolves=true (engineMapRef=0x5BC9A080)` proves the engine's own collidable→refr map
*does* know the player's phantom, so a refr-based counter could have fired for the player and simply
never did. Those are real negatives, not broken probes.

## Why `bumpedForce = 0.00` is the whole story

The engine detects the contact and applies **no response**. That is why walking into an NPC in
Skyrim moves nothing: the collision is computed, recorded, and deliberately not acted on. The
missing half of the interaction is exactly what this plugin supplies.

```
                    engine                        PushAside
  contact ---------> bump record ----------------> read on the main thread
                    (detection, already there)     resolve target, publish
                                                   apply velocity on the physics thread
                                                   (the response the engine omits)
```

## The working signal

```cpp
// bhkCharacterController, plain fields
supportBody               // 0x2B0   hkRefPtr<hkpRigidBody>   (NOT in design.md's verified table)
bumpedForce               // 0x2B8   float                    (observed 0.00)
bumpedBody                // 0x2C0   hkRefPtr<hkpRigidBody>   (objects)
bumpedCharCollisionObject // 0x2C8   hkRefPtr<hkpRigidBody>   (the bumped CHARACTER)
```

`bumpedCharCollisionObject->GetUserData()` resolves to the bumped character's `TESObjectREFR`.

### Detection → handoff → application

1. **Main thread** (`ProbePlayerBumpRecord`, `src/WorldContactListener.cpp`): read the field via the
   vtable-verified `PlayerController()`; resolve `GetUserData()` → `TESObjectREFR` → `As<Actor>()`
   (reject null and the player) → `AsProxyController(actor->GetCharController())->GetCharacterProxy()`.
   Publish into a single-slot (`src/BumpSlot.h`, latest wins); `nullptr` clears it.
2. **Physics thread** (`PushListener::ProcessConstraintsCallback`): exchange-clear the slot and call
   the unchanged `PushModel::OnCharacterContact(a_proxy, target, nullptr)`. A null contact makes
   `ComputePushDirection` fall back to the two capsule positions — the shove axis.
3. **Never write proxy velocities from the main thread.** `hkpCharacterProxy::velocity` is a 16-byte
   `hkVector4` racing the physics step; the write belongs inside the solver callback.

### Proximity guard

The engine can leave the bump record set after the contact breaks (observed: the same value across
~20 s), and `PushModel`'s gates test direction and closing speed but **never distance** — so a stale
target lying in the direction of travel could be shoved across the room. The publish is therefore
gated on 200 world units (~2.9 m; 1 unit ≈ 1.4 cm) between capsule centres, deliberately generous
because a false withhold is a missed push while the failure it prevents is a shove at range. Missing
data counts as *out* of range. A withheld target is neither published nor counted as a detection,
and is logged once per distinct target — which is also the measurement of whether the record really
is sticky.

## Open questions

- **Are the bump fields real contacts, or something else?** The offsets are verified; the *semantics*
  are not. Only a run where the field names an actor the player actually touched confirms it.
- **Is the record sticky?** Unknown; the guard plus its withhold log answers this.
- **Does the player get cloned bodies from Precision?** Read from Precision's source (see
  `precision-interop.md`), not yet measured in game. If yes, the manifold's unregistered collidable
  may be resolvable to an actor, which would give contact *geometry* as well as identity.

## Also recorded

- The player's collision **group is not stable**: it changed `0x0009` → `0x0496` across a proxy
  rebuild. Identity-by-group is therefore fragile and must not be relied on.
- `nullVsActor` is a **superset tag, not a player signal**: every NPC-vs-world pair lands in it, and
  so does player-vs-null, because the player is itself an `Actor`. The observed events were all
  actor-vs-static (`filterA=0x045A001E` character layer, `filterB=0x00010011` world).

Store entries: `research/skyrim-push-aside-detection-channels`, and for the probe discipline that
this cost, `conventions/skse-ingame-probe-guardrails`.