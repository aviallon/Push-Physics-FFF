# Skyrim-Push-Aside — design

A physics-based SKSE plugin for Skyrim AE 1.7.104 that lets the player push
NPCs and followers aside by walking into them. The push is produced by the
Havok character-controller solver, not by an animation, and without disabling
collision.

Companion documents:

| doc | what it establishes |
|---|---|
| [`engine-internals.md`](engine-internals.md) | verified AE ids, prologue hashes, vtable slots, struct offsets for 1.7.104 |
| [`build.md`](build.md) | how this machine builds/deploys a CommonLibSSE-NG plugin (Nix + xmake) |

The plugin's shape (an `ini`-driven `Config`, a committed and *content-verified*
target table, a `Health` verdict that is `DEGRADED` unless everything it touches
is named and verified, and MinHook only where a detour is actually required) is
taken from `/home/aviallon/Programing/Opensource/HeapSentinel`, which is the
reference implementation for this project. §6.1 says exactly what is reused
verbatim.

---

## 0. Recommendation in one paragraph

**Attach our own `hkpCharacterProxyListener` to the player's
`hkpCharacterProxy`** — no code detour at all — and get the proxy from
`PlayerCharacter::GetSingleton()->GetCharController()` →
`static_cast<bhkCharProxyController*>(...)` → `GetCharacterProxy()`. Havok
dispatches every listener's virtuals on the proxy it belongs to, so our
`CharacterInteractionCallback(a_proxy, a_otherProxy, a_contact)` is called with
`a_proxy` = **the player's proxy by construction** (no "is this the player?"
heuristic needed) whenever the player's capsule contacts another character; our
`ObjectInteractionCallback` receives the player's character-vs-rigid-body
contacts for the corpse/item half of the feature. In the callback we compute a
relative-velocity- and mass-scaled velocity and apply it to
`a_otherProxy->velocity`, which is the vector the target's own Havok solve
integrates; the target's manifold (walls, floor, the player) clips it, so the
engine keeps collision on and decides where the target can actually go.
MinHook is **optional, not required**: it is kept only as an escalation for the
cases the listener cannot cover (the character-interaction virtual not being
dispatched at all, NPC-pushes-player as a target, and a target-side pre-solve
injection point if a one-write-per-contact-frame proves too weak). Approach A —
tuning the proxy's own `characterMass` `+0xC0` / `characterStrength` `+0xBC` —
is folded in as two config fields rather than kept as a rival design: those two
members only affect the **rigid-body** path (`characterStrength` is documented
as the force cap on "moving objects", `characterMass` as a downward force on
bodies underfoot), so they can improve how the player shoves corpses and items
but cannot make the player win a character-vs-character contest. The riskiest
unknown is whether Skyrim's Havok dispatches `CharacterInteractionCallback` at
all; §3.7 and §9 handle that with a listener-side fallback and a one-session
instrumentation step before any physics is written.

---

## 1. What the engine actually gives us

### 1.1 Characters are phantoms, not rigid bodies

An actor's collision is an `hkpShapePhantom` (a capsule) driven by
`hkpCharacterProxy`, not an `hkpRigidBody`:

```
Actor* ──Actor::GetCharController()──▶ bhkCharacterController*
                                            │  (only the concrete subclass has the proxy)
                                            ▼
                                      bhkCharProxyController                     size 0x5B0
                                        +0x000  hkpCharacterProxyListener vptr   (vtable 240558)
                                        +0x010  bhkCharacterController           size 0x330
                                        +0x340  bhkCharacterProxy proxy
                                                   +0x010 hkRefPtr referencedObject ──▶ hkpCharacterProxy
                                        +0x5A0  ...
```

`hkpCharacterProxy*` is therefore `*reinterpret_cast<void**>(controller + 0x350)`,
which is exactly what `bhkCharProxyController::GetCharacterProxy()` returns
(`static_cast<hkpCharacterProxy*>(proxy.referencedObject.get())`). The
relationship is invertible, which matters for the target side of a push:
`controller = reinterpret_cast<std::uint8_t*>(proxy) - 0x350`, recoverable and
**verifiable** (§3.3). `bhkCharacterController` itself does **not** own or point
at the proxy (engine-internals §1.3).

The proxy moves by asking the simplex solver to fit `velocity` (offset `+0x60`)
into the free space described by `manifold` (offset `+0x20`). Contacts with
walls, floors **and other character phantoms** all land in the manifold. The
solver clips against all of them; it does not exchange momentum between two
character phantoms. That single sentence is the whole reason this plugin exists.

### 1.2 The two callbacks, and what each can move

`hkpCharacterProxy::integrate` runs the collision query (`UpdateManifold`, AE
**62647**), builds the simplex input, calls the listeners, solves, and moves the
phantom. There are exactly two listener callbacks that can change what happens:

| callback | listener slot | AE id | can move |
|---|---|---|---|
| `CharacterInteractionCallback(proxy, otherProxy, contact)` | 4 | **79134** | another **character**, by writing its proxy velocity |
| `ObjectInteractionCallback(proxy, event, result)` | 5 | **79133** | a **rigid body** — the callback writes `result.objectImpulse` / `result.impulsePosition`, which the solver applies to `event.body` |

This split is the design: Path 1 (character) and Path 2 (rigid body) are
different code paths with different units and different fail modes, and a
complete feature needs both.

`characterStrength` and `characterMass` act only in Path 2, because that is
where Havok clamps/scales the impulse applied to `event.body`. Nothing in the
character-vs-character path reads them.

### 1.3 `bhkCharProxyController` is the only listener the engine installs

`hkpCharacterProxyListener` has exactly one concrete implementor in 1.7.104:
`bhkCharProxyController` (there is no `bhkCharacterProxyListener`). The
controller *is* its own listener, at the same address as its
`hkpCharacterProxyListener` base, which means:

* our listener is a **peer** of the engine's, in the same array, called by the
  same dispatch loop;
* we do not have to replace or wrap the engine's behaviour — the engine's own
  push (whatever it does today) still runs, and ours adds to it.

The proxy's `listeners` array (`+0xC8`) is a plain
`hkArray<hkpCharacterProxyListener*>`; there is no
`addCharacterProxyListener`/`removeCharacterProxyListener` symbol, so we append
directly with `hkArray::push_back` (which exists and allocates through Havok's
heap allocator — hence main-thread-only, §3.5).

### 1.4 Verified ground truth (do not re-derive)

From `engine-internals.md`, verified by reading `SkyrimSE.exe`:

| symbol | AE id | kind | vtable / slot | RVA | prologue FNV-1a-64 |
|---|---|---|---|---|---|
| `bhkCharProxyController::characterInteractionCallback` | 79134 | kVtable | 240558 / 4 | `0x10981A0` | `0x06DDFF66FA2C2047` |
| `bhkCharProxyController::objectInteractionCallback` | 79133 | kVtable | 240558 / 5 | `0x1098180` | `0x9D1E885DC8E71243` |
| `bhkCharProxyController::processConstraintsCallback` | 79130 | kVtable | 240558 / 1 | `0x10976A0` | (generate) |
| `hkpCharacterProxy::UpdateManifold` | 62647 | kVtable | 230859 / 3 | `0xB9F720` | `0x3DDCC387306143DD` |
| `bhkCharacterController::SetLinearVelocityImpl` | 79146 | kVtable | 240560 / 7 | (generate) | (generate) |
| `bhkCharacterController::TryMoveTo` | 78260 | kRva | — | `0x1063E80` | `0xBF13EEDC2AA50D7E` |
| `bhkCharacterController::ProcessHurtfulBody` | 78302 | kRva | — | `0x1066C60` | `0x2C94B2F04299680F` |
| `hkpMotion::ApplyLinearImpulse` | 60970 | kVtable | 227961 / 0x13 | `0xB4F610` | `0xC0CB45878D4C5623` |
| `bhkCharProxyController` listener vtable | — | vtable | id **240558** | `0x1A89540` | — |
| `bhkCharProxyController` controller vtable | — | vtable | id **240560** | `0x1A89578` | — |

Struct offsets in `hkpCharacterProxy`: `velocity +0x60`, `shapePhantom +0x80`,
`dynamicFriction +0x88`, `staticFriction +0x8C`, `maxCharacterSpeedForSolver
+0xB8`, `characterStrength +0xBC`, `characterMass +0xC0`, `listeners +0xC8`.
In `bhkCharacterController`: `outVelocity +0x090`, `initialVelocity +0x0A0`,
`velocityMod +0x0B0`, `pushDelta +0x0E0`, `flags +0x218`,
`bumpedForce +0x2B8`, `bumpedBody +0x2C0`, `bumpedCharCollisionObject +0x2C8`.

`RELOCATION_ID(se, ae)` in these headers is 1.7.104 for its second argument
(`RUNTIME_SSE_LATEST_AE = (1,7,104,0)`), so no 1.6.x/1.7.104 ambiguity exists.

### 1.5 What is *not* verified (the honest list)

| id | unknown | mitigation |
|---|---|---|
| U1 | **Does Skyrim's Havok dispatch `CharacterInteractionCallback` at all?** The slot is populated (verified); dispatch is Havok-internal and only a live run can confirm it | instrumentation first (§7 step 2); fallback via the listener's `ProcessConstraintsCallback` manifold scan (§3.7); escalation to a MinHook detour (§3.8) |
| U2 | **Which thread** runs the callback — a main-thread write into another proxy's `velocity` is fine only if the target is not integrating concurrently | log the callback thread identity once; keep all game-world access (`Actor*`, mass, flags) in a main-thread snapshot (§3.4); if it is a job thread, restrict writes to proxies whose integrate we have already seen on the same thread this frame |
| U3 | **The ABI of `ProcessConstraintsCallback`.** CommonLibSSE-NG declares a leading `const hkpCharacterProxy*` that Havok's base class does not; one of the two is wrong for this build | that override is diagnostics-gated in v1 and enabled only if its invariant self-check passes (§3.7) |
| U4 | **Does the engine set `characterMass`/`characterStrength`?** Havok defaults are `0` and `FLT_MAX` | mass model prefers `characterMass > 0`, else `TESRace::data.baseMass`; observed values are logged once (§4.4) |
| U5 | **Is the player's controller always `bhkCharProxyController`?** Not while mounted or ragdolled | vtable identity check before every cast (§3.2), never a bare `static_cast` |
| U6 | **Is `hkArray::push_back` safe on a live `listeners` array?** The array is a plain `hkArray` and `push_back` exists; the risk is a concurrent reader | append only on the main thread, when the proxy is not being stepped (§3.5) |

---

## 2. Two candidate approaches

### Approach A — tune the existing proxy knobs

```cpp
playerProxy->characterStrength = fCharacterStrength;  // +0xBC, default FLT_MAX
playerProxy->characterMass     = fCharacterMass;      // +0xC0, default 0
```

What this buys, per Havok's own documentation of the members:

* `characterStrength` is the clamp on the constant force the character may
  impart **onto moving objects**. Raising it makes the player shove **corpses,
  loose items and ragdolls** harder. It is the direct, supported knob for the
  "like pushing an item/corpse" feel.
* `characterMass` is **only** a downward force applied to bodies underfoot. It
  does not participate in character-vs-character resolution.
* `maxCharacterSpeedForSolver` (`+0xB8`) is a stability clip for the "squeezed
  between two moving planes" case; useful to raise if our injections trip it,
  but it does not create push.

Cost: two float writes on one proxy. Risk: near zero. Coverage: **rigid bodies
only**.

### Approach B — own listener, computed velocity

Register our own `hkpCharacterProxyListener` on the player's proxy and add the
physics ourselves:

* Path 1: compute a velocity for the *other character's* proxy, scaled by
  relative closing speed and the mass ratio, and add it to
  `otherProxy->velocity`; the target's own solve bounds it.
* Path 2: keep the engine's impulse and scale (or override) it.

Cost: one static listener object, one fixed-size table, one registry. Risk:
moderate — it runs inside the physics dispatch for every contact the player has,
so the guards in §5 are the product, not boilerplate.

### 2.1 Verdict

| | A | A+B (listener) |
|---|---|---|
| NPC blocking a corridor moves aside | **no** (character path untouched) | yes |
| "player wins" against a standing NPC | **no** | yes, and tunable |
| sustained displacement rather than a one-frame nudge | no | yes (re-applied per contact frame; §3.6) |
| stagger the target on a hard shove | no | yes |
| heavy target (giant) resists by mass | no | yes (mass ratio) |
| corpse/item/dragon shove scales | yes (weak) | yes (A's knob **and** B's scale) |
| failure surface | nil | listener dispatch + thread + append — all gated |

**A alone does not suffice**, for two structural reasons rather than tuning
reasons:

1. The solver treats another character's phantom as a *surface*: it prevents
   penetration and cannot exchange momentum. No value of `characterStrength`
   changes that, because the character-vs-character path never reads it.
2. Even if it did, one float cannot express what the feature needs:
   relative-velocity scaling, a mass ratio, a decaying multi-frame push, a cap,
   and a stagger threshold with a cooldown.

So: **B is the mechanism; A is two config fields inside B's rigid-body path**
(`fCharacterStrength`, `fCharacterMass`, applied to the player's proxy on load
and on config change).

---

## 3. Attachment point and the object graph

### 3.1 The pointer path (exact, 1.7.104)

```cpp
// player -> its hkpCharacterProxy  (the only proxy we attach to)
RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
RE::bhkCharacterController* ctrl = player->GetCharController();     // inline via AIProcess
RE::bhkCharProxyController* pc = AsProxyController(ctrl);           // vtable check, §3.2
RE::hkpCharacterProxy* proxy = pc ? pc->GetCharacterProxy() : nullptr;

// inside our listener, the target side is inverted and verified
RE::bhkCharProxyController* target = ControllerOf(a_otherProxy);    // §3.3
```

### 3.2 Identifying the player's proxy (and refusing the rest)

The player predicate is *structurally* satisfied by the listener design: the
listener is attached to one proxy, and Havok only calls it on that proxy, so
`a_proxy` **is** the player's proxy. We still verify it, because an orphaned
attach (the player's controller is rebuilt on load and the old proxy survives)
must never push on behalf of a proxy that is no longer the player's:

```cpp
namespace
{
	// vtable identity: 240560 is the bhkCharacterController subobject vtable
	RE::bhkCharProxyController* AsProxyController(RE::bhkCharacterController* a_ctrl)
	{
		if (!a_ctrl) {
			return nullptr;
		}
		static REL::Relocation<std::uintptr_t> kControllerVtable{ RE::VTABLE_bhkCharProxyController[1] };
		if (*reinterpret_cast<const void* const*>(a_ctrl) !=
			reinterpret_cast<const void*>(kControllerVtable.address())) {
			return nullptr;  // bhkCharRigidBodyController, or mounted / detached / ragdolled
		}
		return static_cast<RE::bhkCharProxyController*>(a_ctrl);
	}
}

// the player's proxy, or nullptr when the player has none right now (mounted,
// ragdolled, mid-load)
RE::hkpCharacterProxy* PlayerProxy()
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* pc = player ? AsProxyController(player->GetCharController()) : nullptr;
	return pc ? pc->GetCharacterProxy() : nullptr;
}
```

And inside every callback, the orphan check:

```cpp
if (a_proxy != m_owner) {
	return;   // this listener was left on a proxy that is no longer the player's
}
```

This is the discriminator the naive "compare with
`PlayerCharacter::GetCharController()`" would need on every call, obtained for
free from where the listener is registered — but still enforced.

### 3.3 Recovering the target's controller (and verifying it)

The target of a push is a proxy, and we need its controller for
`flags & kNotPushable`, its `Actor*` (for ragdoll/dead/combat), and its
stagger. The layout in §1.1 inverts exactly:

```cpp
// proxy = *(hkpCharacterProxy**)(controller + 0x350)   =>   controller = proxy - 0x350
RE::bhkCharProxyController* ControllerOf(RE::hkpCharacterProxy* a_proxy)
{
	auto* c = reinterpret_cast<RE::bhkCharProxyController*>(
		reinterpret_cast<std::uint8_t*>(a_proxy) - 0x350);

	// 1. the candidate must be a bhkCharProxyController (listener vtable 240558)
	static REL::Relocation<std::uintptr_t> kListenerVtable{ RE::VTABLE_bhkCharProxyController[0] };
	if (*reinterpret_cast<const void* const*>(c) !=
		reinterpret_cast<const void*>(kListenerVtable.address())) {
		return nullptr;
	}

	// 2. and it must point back at exactly this proxy (definitive, free)
	return c->GetCharacterProxy() == a_proxy ? c : nullptr;
}
```

Step 2 makes a wild `- 0x350` unrepresentable: it would have to name an object
whose first qword is the engine's `bhkCharProxyController` vtable *and* whose
`referencedObject` is our proxy. This is what lets the design rely on offset
arithmetic without a reverse map in the hot path.

A `ProxyRegistry` (proxy pointer → `{Actor*, mass, flags}`), rebuilt on the main
thread, is still needed for everything the proxy itself does not carry: the
`Actor*` for the stagger, and a *stable* snapshot of ragdoll/dead/combat/
swimming state (§3.4). It is also the fallback for the target's controller if
`ControllerOf` ever refuses (registry miss ⇒ conservative scaling, §5.16).

### 3.4 Why a snapshot instead of live `Actor*` reads

The callback runs inside the Havok character step. Reading `actor->IsInRagdollState()`
or `actor->GetRace()` there is a game-world access in a physics callback, which
is exactly the kind of thing that works in a single-threaded test and crashes
under a job queue. So:

* `ProxyRegistry` is refreshed on the **main thread** through
  `SKSE::GetTaskInterface()->AddTask(...)` every `fRegistryRefreshSec`
  (default 1.0 s), and immediately on `kPostLoadGame` / `kNewGame`;
* each entry is published behind a seqlock-style even/odd counter, so a reader
  on any thread sees a whole entry or falls back to defaults;
* the stagger is *deferred*: the callback pushes `(proxy, dv, dir)` into a fixed
  SPSC ring and the main-thread pump fires the animation event.

So the physics callback touches exactly two things: `hkVector4` fields of Havok
objects, and our own lock-free tables.

### 3.5 Registration, lifetime and the load cycle

```cpp
class PushListener final : public RE::hkpCharacterProxyListener
{
public:
	// listener vtable slot 1 - diagnostics/fallback only in v1 (U3, §3.7)
	void ProcessConstraintsCallback(const RE::hkpCharacterProxy* a_proxy,
		const RE::hkArray<RE::hkpRootCdPoint>& a_manifold,
		RE::hkpSimplexSolverInput& a_input) override;

	// slot 4 - Path 1, the player pushes a character
	void CharacterInteractionCallback(RE::hkpCharacterProxy* a_proxy,
		RE::hkpCharacterProxy* a_otherProxy, const RE::hkContactPoint& a_contact) override;

	// slot 5 - Path 2, the player pushes a corpse/item/ragdoll
	void ObjectInteractionCallback(RE::hkpCharacterProxy* a_proxy,
		const RE::hkpCharacterObjectInteractionEvent& a_input,
		RE::hkpCharacterObjectInteractionResult& a_output) override;

	void AttachTo(RE::hkpCharacterProxy* a_proxy);   // main thread only
	void Detach();                                   // main thread only
	[[nodiscard]] RE::hkpCharacterProxy* Owner() const { return owner_; }

private:
	RE::hkpCharacterProxy* owner_ = nullptr;
};
```

`AttachTo` is idempotent, records `owner_` **before** appending, and appends only
if our pointer is not already in the array (a re-attach after a controller swap
must not double-register, or the push would be applied twice per frame):

```cpp
void PushListener::AttachTo(RE::hkpCharacterProxy* a_proxy)
{
	if (!a_proxy || owner_ == a_proxy) {
		return;
	}
	owner_ = a_proxy;
	auto& listeners = a_proxy->listeners;
	for (std::int32_t i = 0; i < listeners.size(); ++i) {
		if (listeners[i] == this) {
			return;
		}
	}
	listeners.push_back(this);   // hkArray::push_back -> Havok heap; main thread only
	logger::info("listener attached to player proxy 0x{:X} (listeners now {})",
		reinterpret_cast<std::uintptr_t>(a_proxy), listeners.size());
}
```

Lifetime is trivial — the listener is a function-local static, so it outlives
every proxy — but the *owner* is not, so the load cycle is handled explicitly:

| event | action |
|---|---|
| `kPostLoadGame`, `kNewGame` | `ProxyRegistry::RebuildNow()`, `AttachTo(PlayerProxy())`, `PushModel::ApplyProxyTuning(player proxy)` (Approach A) |
| `kPreLoadGame` | `PushRegistry::Clear()`, `ProxyRegistry::Invalidate()`, listener `Detach()` (drop `owner_`); the old proxy's array, if it is ever stepped again, calls us and the orphan check in §3.2 returns immediately |
| main-thread pump (1 Hz) | if `PlayerProxy() != owner_`, re-attach: the player's controller is rebuilt on death/ragdoll/mount transitions |

Death and ragdoll deserve a note: while the player is ragdolled the controller
is replaced/disabled, so `PlayerProxy()` returns null and we simply stop
pushing (correct — a limp body shoving people would be a bug). Nothing needs to
be torn down.

### 3.6 The push is a per-contact-frame action, not a one-shot

This is the single most important consequence of moving from a detour to a
listener, so it is worth being exact.

`CharacterInteractionCallback` is called on the *player's* proxy, once per
manifold contact with another character proxy, on **every frame the contact
exists**. A persistent contact therefore gives us one call per frame for as long
as the player keeps walking into the NPC. The baseline mechanism is:

> On each contact frame, **add** the computed velocity to
> `a_otherProxy->velocity`. The engine's own solve, on the target's integrate,
> uses that value; the target's movement code replaces it at the start of the
> next frame, and our next contact frame adds to it again.

That produces exactly the intended "keep walking and they keep moving"
behaviour without any per-frame hook, and it stops when the player stops
pushing (or when the target has moved far enough that the capsule no longer
touches). Two honest limitations and their escapers:

| limitation | escaper |
|---|---|
| the write is a one-frame impulse unless the contact repeats; a target against a wall can consume it and the player sees no displacement | the injected **buffer** is kept per target with a decay (`PushRegistry`), and re-applied on each contact frame (§4.1 step 6); the wall case is inherent and desirable (the target cannot be pushed into geometry) |
| if the target's integrate has already run this frame, our write lands next frame (one frame of latency, ≈16 ms) | unavoidable without a target-side point; imperceptible |
| after the contact breaks, the target stops almost immediately (no coast) | `bTargetSideInjection` (§3.8, MinHook on `processConstraintsCallback`), or a target-side listener appended to the pushed proxy, re-injects the decaying buffer at the target's own pre-solve point |

### 3.7 Fallback detection inside the listener (no detour)

If U1 resolves the wrong way and `CharacterInteractionCallback` is never
dispatched, the listener is not dead: `ProcessConstraintsCallback` (slot 1) is
called before **every** solve and receives the manifold. The manifold contains
one `hkpRootCdPoint` per contact, each with `rootCollidableA` /
`rootCollidableB` (`hkpCollidable*`, offset `0x20` / `0x30`), and the other
character's phantom is registered in `ProxyRegistry`, so the same pair can be
reconstructed:

```cpp
for (std::int32_t i = 0; i < a_manifold.size(); ++i) {
	const auto& p = a_manifold[i];
	if (const auto* other = ProxyRegistry::Get().ProxyForCollidable(p.rootCollidableB)) {
		PushModel::OnCharacterContact(a_proxy, other, &p.contact);
	} else if (const auto* other2 = ProxyRegistry::Get().ProxyForCollidable(p.rootCollidableA)) {
		PushModel::OnCharacterContact(a_proxy, other2, &p.contact);
	}
}
```

Because NG adds a leading `const hkpCharacterProxy*` to that override that
Havok's base class does not have (U3), this path is **diagnostics-gated in v1**:
the override exists, verifies that the pointer it receives equals its own
`owner_` and that any plausible frame time is in `(0, 0.2]`, and only then
enables the scan. If the invariant fails repeatedly, the scan stays off, the
plugin reports `DEGRADED`, and Path 1 relies on slot 4 alone. This is
"verify the semantics, not just the bytes" applied to a virtual we override
rather than a function we patch.

### 3.8 MinHook is optional (escalations, in order of preference)

Nothing below is required for v1. Each is a config-gated escalation behind the
same committed verification table and the same runtime invariant discipline as
HeapSentinel.

| escalation | target | solves |
|---|---|---|
| E1 | `processConstraintsCallback` **79130** (vtable 240558 / 1) | target-side pre-solve injection point: a decaying push that continues after contact, with no reliance on slot 4 dispatch |
| E2 | `hkpCharacterProxy::UpdateManifold` **62647** (vtable 230859 / 3) | the guaranteed per-step entry point if slot 1's ABI is unusable: the manifold is built here, and the callback fires for every proxy |
| E3 | `bhkCharacterController::SetLinearVelocityImpl` **79146** (vtable 240560 / 7) | signature-clean injection into any controller (it feeds the movement state machine, so it is the least clean option physically) |
| E4 | `bhkCharacterController::ProcessHurtfulBody` **78302** | engine-native stagger instead of an animation-graph event, if the behaviour-graph event proves unreliable |
| E5 | `hkpMotion::ApplyLinearImpulse` **60970** (vtable 227961 / 0x13) | a true impulse on a rigid body (corpse/ragdoll) instead of scaling the engine's `objectImpulse` |

The chosen default (no detour) means the *committed target table can be empty in
v1*: nothing is patched, so nothing needs verifying. That is a genuinely
smaller attack surface than the detour design, and it makes the verification
machinery (kept in the tree, wired, and exercised by CI with the escalation
targets listed above) a *precondition* for turning any escalation on rather
than a cost paid on every start.

---

## 4. The model

All quantities are in **engine (Havok) units**; every constant below is a
unitless ratio, a time, or an engine-unit speed that is calibrated in §4.7.
There is no unit conversion anywhere.

### 4.1 Path 1 — character vs character

Per interaction event:

| symbol | meaning | source |
|---|---|---|
| `p_s`, `p_o` | pusher / other position (Z up) | `ControllerOf(proxy)->GetPosition()`; `other->shapePhantom->motionState.transform.translation` |
| `v_s`, `v_o` | pusher / other velocity | `proxy->velocity` (`+0x60`) |
| `n_c` | contact normal | `contact.separatingNormal` |
| `m_s`, `m_o` | masses | §4.4 |
| `d` | push direction | computed below |

```
1.  d = horizontalNormalize(p_o - p_s)
      if |p_o - p_s| < eps:  d = horizontalNormalize(n_c)      // oriented p_s -> p_o
      if still degenerate:   return                            // nothing to push along

2.  v_rel   = v_s - v_o
    v_close = max(0, dot(d, v_rel))          // closing speed along the push direction
    v_close = v_close - fMinRelSpeed
      if v_close <= 0: return                // standing still, or walking away: no push

3.  mu  = clamp( 2 * m_s / (m_s + m_o), 0, fMassRatioMax )
    dv  = (1 + fRestitution) * mu * v_close * fPushScale        // engine units/s

4.  heavy gate:
      if m_o > fHeavyMassRatio * m_s:
          if v_close < fHeavyMinSpeed: return
          dv *= fHeavyScale                                     // default 0.25
      dv = min(dv, fMaxDeltaV)

5.  state scales:
      dv *= targetInCombat ? fCombatScale : fOutOfCombatScale
      dv *= targetSwimming ? fSwimScale : 1.0
      dv *= targetAirborne ? fAirborneScale : 1.0
      dv *= targetKnown    ? 1.0 : fUnknownTargetScale          // registry miss

6.  buffer write (cooldown-gated, take-max - never accumulate):
      e = buffer[otherProxy]                                    // PushRegistry
      if now - e.lastPushAt >= fPushCooldownMs:
          e.v = d * dv;  e.lastPushAt = now
      else:
          e.v = d * max(|e.v|, dv)
      e.dir = d;  e.magnitude = dv
      e.v = clampLength(e.v, fMaxInjectedSpeed)
      e.expiresAt = now + fMaxPushDurationMs

7.  apply (this frame, to the target's own solve) **at most once per frame**:
      if e.appliedFrame != currentFrame:      // a manifold can hold SEVERAL contact
          otherProxy->velocity += e.v         // points against the same proxy; without
          e.appliedFrame = currentFrame       // this guard the push is multiplied
      if fReactionOnPusher > 0:
          proxy->velocity -= e.v * fReactionOnPusher

8.  stagger:
      need = fStaggerDeltaV * pow(m_o / m_s, fStaggerMassExponent)
      if dv >= need and (targetInCombat or fStaggerOutOfCombat)
         and now - e.lastStaggerAt >= fStaggerCooldownMs:
          e.lastStaggerAt = now
          StaggerQueue::Push(otherProxy, dv, d)                 // drained on the main thread
```

Step 7 is the whole push: `otherProxy->velocity` is the vector
`hkpCharacterProxy::integrate` builds its simplex input from, so the target's
own solve decides how much of it survives the wall it is standing against.

**Injected velocity vs impulse — be precise about this.** What we write is a
*velocity*; the momentum that corresponds to it is `J = m_o * dv`. It is not an
impulse handed to the constraint solver, so two consequences are stated rather
than hidden:

* it is not momentum-conserving. The pusher loses nothing unless
  `fReactionOnPusher > 0`. The honest fixes available are `fPushScale < 1`, the
  `fMaxDeltaV` / `fMaxInjectedSpeed` caps, and `fReactionOnPusher` (default `0`
  for the player so the player is never slowed; `0.15` if `bNpcVsNpc` is ever
  enabled);
* the solver may absorb part of it (pushing the target into a wall), in which
  case the target stops and no interpenetration occurs. That is the desired
  behaviour, not a bug, and it is the reason this is "real character-controller
  physics" rather than a teleport.

### 4.2 Path 2 — character vs rigid body

The engine's own impulse is the correct one; our first job is to stop it being
clamped, and our second to let it be scaled. Because our listener is on the
player's proxy, `ObjectInteractionCallback` fires only for the player's
contacts, so no "is this the player?" test is needed here at all.

```cpp
// the engine's own controller is a peer listener and has already run (or will);
// we do not call it, we adjust the result it shares through a_output
if (!a_input.body || a_input.objectMassInv <= 0.0f) {
	return;                                            // static/keyframed, or nothing to move
}
if (IsHeavyBody(a_input, m_player)) {                   // 1/objectMassInv > fHeavyMassRatio * m_player
	return;                                            // dragons, mammoths: not launchable by walking
}

if (Config::Get().objectCustomImpulse) {
	const float m_char     = MassOf(a_proxy);
	const float m_char_inv = m_char > 0.0f ? 1.0f / m_char : 0.0f;
	const float v_close    = -a_input.projectedVelocity;    // sign measured in step 8 of §7
	const float j = (1.0f + fObjectRestitution) * std::max(0.0f, v_close - fMinRelSpeed) /
	                (a_input.objectMassInv + m_char_inv);
	const float capped = fMaxObjectImpulse > 0.0f ? std::min(j, fMaxObjectImpulse) : j;
	a_output.objectImpulse    = Scale(Horizontal(normalTowardBody(a_input)), capped * fObjectShoveScale);
	a_output.impulsePosition  = a_input.position;           // at contact height -> torque -> tumble
} else {
	a_output.objectImpulse    = Scale(a_output.objectImpulse, fObjectShoveScale);
	a_output.impulsePosition  = a_input.position;
}
```

`objectCustomImpulse` defaults to **0**: scaling the engine's verified impulse
is safer than re-deriving it, and it composes with `fCharacterStrength`, which
raises the clamp the engine applies before we see the value. The custom formula
is the Havok default impulse with our own cap, for when the engine's value turns
out not to respond to `characterStrength` at all.

Whether the engine's own listener has already run when ours does is
unspecified (both are entries in the same `listeners` array, and array order is
whatever the engine created), so the scale-only mode must be read as "scale
whatever `objectImpulse` currently holds" and the value is logged once to see
which order actually happens. `normalTowardBody` is the one thing that must be
measured rather than assumed: log
`dot(a_input.normal, a_output.objectImpulse)` for one vanilla call (it is ±1),
then pin the sign. Getting it backwards launches the corpse into the player.

### 4.3 Avoiding the "squash"

| artefact | control |
|---|---|
| target accelerated through a wall | we write `velocity`, never a position, and never `pushDelta`; the target's own manifold (including the wall) is solved against its own velocity, so it stops. We do not bypass the solver |
| target vibrates inside a doorway | per-target cooldown (200 ms) + take-max semantics: at most one new push per window |
| sustained jitter while the player keeps walking into them | the buffer decays, and the push is only renewed while `v_close > 0`; when the player stops, `v_close` → 0 and there is nothing left to repeat |
| runaway speed when several contacts fire in one frame | one buffer entry per target; `fMaxInjectedSpeed` clamp; `uMaxInteractionsPerFrame` budget |
| player pushed into geometry by an NPC | our listener is on the player's proxy only, so we never inject into the player. Vanilla behaviour for "an NPC walks into the player" is untouched (`bPlayerAsTarget` is about the escalation path, not v1) |
| target lifted off the ground / stuck in the air | injected direction is horizontal (`z = 0`), plus optional `fLift` (default 0) |
| target "glides" without a walk animation for too long | the push is renewed only while the player is pushing; `fMaxPushDurationMs` (600 ms) bounds the buffer even if a contact somehow persists |
| double-applied push | `AttachTo` refuses to register twice; the buffer is keyed by proxy, is take-max, and is applied **at most once per frame** (`e.appliedFrame`) because one manifold can hold several contact points against the same proxy |

### 4.4 Mass model

Preference order, evaluated on the main thread and cached in `ProxyRegistry`:

1. `proxy->characterMass` (`+0xC0`) if `> 0` — the engine's own value, which is
   exactly what using the proxy rather than inventing a mass model means.
   Havok's default is 0, so this may never hit (U4).
2. `Actor::GetRace()->data.baseMass` (`TESRace.h`, `+0x34`) × `GetScale()³` —
   a real per-race mass that exists in these headers; giants, mammoths and
   dragons get genuinely large values.
3. `fDefaultCharacterMass` (config, default 80) — last resort.

The player is forced to `fPlayerMass` (default 100) so the player/NPC mass ratio
is a single tunable. This is a *ratio* model: only `mu` and the heavy gate use
the numbers, so absolute correctness does not matter, only ordering.

### 4.5 Heavy targets (giants, mammoths, dragons)

Three separate mechanisms, so that "heavy" is not one magic number:

1. **Mass ratio** (`mu`, step 3): a 400-unit giant against a 100-unit player
   gets `mu = 0.4` instead of `1.0`, so the same walk produces 40 % of the
   displacement.
2. **Heavy gate** (step 4): above `fHeavyMassRatio` (2.5) the push additionally
   requires `fHeavyMinSpeed` and is scaled by `fHeavyScale` (0.25). A giant
   therefore only moves when the player is genuinely sprinting into it, and then
   only a little.
3. **Stagger threshold scales with mass** (step 8): a giant needs a much larger
   `dv` to stagger, so walking into one never staggers it and sprinting into one
   produces at most a small flinch.

Dragons in flight and mounted/mammoth-style actors run
`bhkCharRigidBodyController`, so their proxies do not exist, `CharacterInteractionCallback`
never fires for them, and Path 1 is structurally unreachable; if the player bumps
their rigid body, Path 2's `IsHeavyBody` gate refuses to launch them. This is
the desirable asymmetry: the feature is for *humans blocking a doorway*, and it
should not scale to monsters.

### 4.6 Scaling by player facing direction

The push direction is `horizontalNormalize(p_o - p_s)` — the vector from the
player's capsule centre to the target's, projected to the horizontal plane —
**not** the raw contact normal. Reasons:

* the contact normal can be a slide plane (a wall between them, a staircase
  normal) and pushing along it reads as being deflected rather than shoving;
* `p_o - p_s` is by construction "away from me", which is what the player is
  asking for when they walk into someone;
* the closing-speed projection (`dot(d, v_rel)`) then measures exactly how hard
  the player is moving *into* that line, so strafing past someone at speed
  produces a glancing push and walking straight into them produces the full one.

The contact normal is used only as the fallback when the two capsule centres
coincide (rare, but it happens at zero distance during penetration recovery).

### 4.7 Calibration

Every engine-unit constant in §4.1 is anchored to the player's own walk speed,
which the plugin measures and logs at startup:

```
[info] calibration: player proxy velocity while walking = 141.7 u/s,
       characterMass=0.00 characterStrength=inf, target#0 mass=80.0,
       listener vtable=240558 owner=0x1F4A2C30, callback thread=12345 (main=12345)
```

`fMinRelSpeed` (20) and `fStaggerDeltaV` (120) are then anchored to that number
rather than to a guess, and the log line is the evidence. A `bDebugPushLog` mode
prints one bounded line per accepted push (`dv=… mu=… mass=… cooldown hit/miss`)
for tuning; it is off by default and capped at `fDebugLogMaxPerSec` lines.

---

## 5. Failsafes, jitter and state gates

Every gate is evaluated *before* an entry is written, and every gate has a
config switch so the test matrix can assert the gate itself (a guard that has
never been observed refusing anything is not a guard).

| # | gate | where | default |
|---|---|---|---|
| 1 | master switch; when off, nothing is attached and no target is patched | `main.cpp` | on |
| 2 | **orphan check**: `a_proxy != owner_` ⇒ return | every callback | always |
| 3 | **controller identity check**: `ControllerOf()` must prove `vtable == 240558` *and* `GetCharacterProxy() == proxy` | target side | always |
| 4 | **invariant self-check** on the slot-1 fallback; N consecutive violations disable it and mark the plugin `DEGRADED` | slot 1 | always |
| 5 | `fMinRelSpeed` minimum closing speed | model step 2 | 20 |
| 6 | per-target cooldown `fPushCooldownMs` + take-max | model step 6 | 200 ms |
| 7 | hard `fMaxPushDurationMs` per buffer entry | buffer | 600 ms |
| 8 | `fMaxDeltaV` per event and `fMaxInjectedSpeed` per entry | steps 4/6 | 250 / 350 |
| 9 | exponential damping `fPushDamping` on the buffer | buffer | 6 /s |
| 10 | `Actor::IsInRagdollState()` ⇒ no Path-1 entry (the ragdoll body is Path 2's job) | model entry | on |
| 11 | `Actor::IsDead()` ⇒ no Path-1 entry | model entry | on |
| 12 | target `flags & kNotPushable` or `kNotPushablePermanent` ⇒ no entry; `kNoCharacterCollisions` ⇒ nothing to do | model entry | on |
| 13 | target `IsInKillMove()` / `IsInBleedout()` ⇒ no entry | model entry | on |
| 14 | in combat: `fCombatScale` (0.5), stagger allowed; out of combat: `fOutOfCombatScale` (1.0), `bStaggerOutOfCombat=0` | steps 5/8 | — |
| 15 | `DialogueMenu` open ⇒ the player does not push (the *I'm Walkin' Here* case, solved without disabling collision) | model entry | on |
| 16 | swimming `fSwimScale` (0.35); airborne `fAirborneScale` (0.5) | step 5 | — |
| 17 | registry miss ⇒ push anyway at `fUnknownTargetScale` (0.5), no stagger, early expiry | step 5 | 0.5 |
| 18 | `uMaxInteractionsPerFrame` (32) budget; excess dropped, logged once per second | model entry | 32 |
| 19 | global `bDisableInCombat`, `bDisableWhileSwimming` kill switches | model entry | off |
| 20 | if neither slot 4 nor the slot-1 fallback can be trusted, Path 1 is inert and reported `DEGRADED` — silence must never look like success | `main.cpp` + `Health` | always |

Gate 12 deserves a note: `kNotPushable` lives on the *target's*
`bhkCharacterController::flags` (`+0x218`), which is reachable exactly because
`ControllerOf()` succeeds. When it does not, the flag cannot be evaluated, which
is why gate 17 exists and is conservative.

Jitter control is three-layered on purpose: the cooldown stops *re-pushing*, the
damping stops *sliding forever*, and the expiry stops *a buffer entry outliving
the contact*. Any one alone leaks in a case the other two cover (standing in a
crowd, a target against a wall, a target that dies mid-push).

---

## 6. Skeleton

### 6.1 Repository layout (adapted from HeapSentinel)

```
Skyrim-Push-Aside/
├── flake.nix                        # from build.md
├── xmake.lua                        # HeapSentinel's shape (MinHook optional, §3.8)
├── config/PushAside.ini             # §6.5
├── hooks/                           # committed, content-verified escalation targets
│   ├── skyrimse-1.7.104.0-<sha>.json
│   ├── addresslibrary-1.7.104.0.json
│   └── README.md
├── tools/{gen-hooktable.py,check-hooktable.py}   # reused verbatim
└── src/
    ├── PCH.h
    ├── main.cpp                     # §6.4
    ├── Config.{h,cpp}               # §6.6 (HeapSentinel's Config + new fields)
    ├── Core/Health.{h,cpp}          # reused
    ├── Hooks/
    │   ├── Hooks.h                  # §6.3  the listener + attach API
    │   ├── Hooks.cpp                # §6.3  listener, controllers, tuning
    │   ├── HookTargets.def          # escalation targets only, §6.2
    │   ├── HookTargets.h            # reused unchanged
    │   ├── HookTable.{h,cpp}        # reused unchanged
    │   ├── HookTableData.gen.h      # generated
    │   └── HookVerifier.{h,cpp}     # reused unchanged
    └── Physics/
        ├── HkMath.h                 # §6.7.0
        ├── PushListener.{h,cpp}     # alternative home for §6.3 if Hooks/ stays detour-only
        ├── ProxyRegistry.{h,cpp}    # proxy -> {Actor*, mass, flags} snapshot
        ├── PushRegistry.{h,cpp}     # §6.7.1
        └── PushModel.{h,cpp}        # §6.7.2
```

The `Hooks/` module is kept even though v1 installs no detour, because §3.8's
escalations are the planned v1.1 work and the verification machinery must be
wired (and CI-exercised) *before* the first escalation is turned on. The
listener itself is `Hooks.h`'s public API, as the task asked.

### 6.2 `src/Hooks/HookTargets.def` — escalations only

```c
// PushAside escalation targets. v1 installs NO detour: the feature is a listener
// attached to the player's proxy (see design.md sec 3). These entries exist so the
// committed verification table and the CI completeness check are live from day one,
// and so turning an escalation on is a config change that CI already proves.
//
// HOOK_TARGET(<enum id>, "<display name>", <kind>, <se id>, <ae id>, <vtable id>, <vtable slot>)
HOOK_TARGET(kProcessConstraints, "bhkCharProxyController::processConstraintsCallback", kVtable, 0, 79130, 240558, 1)   // E1
HOOK_TARGET(kUpdateManifold,     "hkpCharacterProxy::UpdateManifold",                   kVtable, 0, 62647, 230859, 3)   // E2
HOOK_TARGET(kSetLinearVelocity,  "bhkCharProxyController::SetLinearVelocityImpl",       kVtable, 0, 79146, 240560, 7)   // E3
HOOK_TARGET(kProcessHurtfulBody, "bhkCharacterController::ProcessHurtfulBody",          kRva,    0, 78302, 0, 0)        // E4
HOOK_TARGET(kApplyLinearImpulse, "hkpMotion::ApplyLinearImpulse",                       kVtable, 0, 60970, 227961, 19)  // E5 (slot 0x13)
```

`kVtable` is exactly HeapSentinel's kind: the Address Library id resolves to the
**function**, and the verifier additionally reads `vtable[slot]` and requires it
to hold that function. That check is not ceremony — it is the guard against
CommonLibSSE-NG's declared slot order being wrong (HeapSentinel's `DESIGN.md`
§3.3 records a case where the header's order did not match the engine's
vtable). It is also why the *listener* design is safer: a wrong slot order
cannot hurt us when we do not patch a slot.

### 6.3 `src/Hooks/Hooks.h` and `Hooks.cpp`

```cpp
// Hooks.h
#pragma once

namespace ha
{
	// The player's own proxy listener. One static instance; it is registered in
	// the player proxy's `listeners` array and receives CharacterInteractionCallback
	// (Path 1) and ObjectInteractionCallback (Path 2) for the player only.
	class PushListener final : public RE::hkpCharacterProxyListener
	{
	public:
		// listener vtable 240558 slot 1 - diagnostics/fallback only in v1 (U3, design.md 3.7)
		void ProcessConstraintsCallback(const RE::hkpCharacterProxy* a_proxy,
			const RE::hkArray<RE::hkpRootCdPoint>& a_manifold,
			RE::hkpSimplexSolverInput& a_input) override;

		// slot 4 - Path 1: the player's capsule contacts another character's proxy
		void CharacterInteractionCallback(RE::hkpCharacterProxy* a_proxy,
			RE::hkpCharacterProxy* a_otherProxy, const RE::hkContactPoint& a_contact) override;

		// slot 5 - Path 2: the player's capsule contacts a rigid body
		void ObjectInteractionCallback(RE::hkpCharacterProxy* a_proxy,
			const RE::hkpCharacterObjectInteractionEvent& a_input,
			RE::hkpCharacterObjectInteractionResult& a_output) override;

		void AttachTo(RE::hkpCharacterProxy* a_proxy);   // main thread only, idempotent
		void Detach();                                   // main thread only
		[[nodiscard]] RE::hkpCharacterProxy* Owner() const { return owner_; }

	private:
		RE::hkpCharacterProxy* owner_ = nullptr;
	};

	[[nodiscard]] RE::hkpCharacterProxy* PlayerProxy();
	[[nodiscard]] RE::bhkCharProxyController* AsProxyController(RE::bhkCharacterController* a_ctrl);
	[[nodiscard]] RE::bhkCharProxyController* ControllerOf(RE::hkpCharacterProxy* a_proxy);
	[[nodiscard]] PushListener& GetPushListener();

	// Full setup: resolve the player proxy and attach. Returns false when there is
	// no player proxy yet (pre-load), which is not a failure - the pump retries.
	bool AttachListener();
	void DetachListener();

	// Only used by the escalations of design.md 3.8; empty in v1.
	bool InstallEscalationHooks();
	void RemoveEscalationHooks();
}
```

```cpp
// Hooks.cpp (the parts that carry the design; includes elided)
namespace ha
{
	PushListener& GetPushListener()
	{
		static PushListener s_listener;   // outlives every proxy; never deleted
		return s_listener;
	}

	void PushListener::CharacterInteractionCallback(RE::hkpCharacterProxy* a_proxy,
		RE::hkpCharacterProxy* a_otherProxy, const RE::hkContactPoint& a_contact)
	{
		if (a_proxy != owner_ || !a_otherProxy) {
			return;   // orphan check (gate 2) - see design.md 3.2
		}
		if (!Config::Get().useCharacterInteraction) {
			return;
		}
		PushModel::OnCharacterContact(a_proxy, a_otherProxy, &a_contact);
	}

	void PushListener::ObjectInteractionCallback(RE::hkpCharacterProxy* a_proxy,
		const RE::hkpCharacterObjectInteractionEvent& a_input,
		RE::hkpCharacterObjectInteractionResult& a_output)
	{
		if (a_proxy != owner_) {
			return;
		}
		if (Config::Get().useObjectInteraction) {
			PushModel::OnObjectContact(a_proxy, &a_input, &a_output);
		}
	}

	void PushListener::ProcessConstraintsCallback(const RE::hkpCharacterProxy* a_proxy,
		const RE::hkArray<RE::hkpRootCdPoint>& a_manifold,
		RE::hkpSimplexSolverInput& a_input)
	{
		// U3: NG's leading proxy parameter is unverified. Prove the ABI on live data
		// before trusting a single byte of the manifold.
		static Invariant inv;
		const bool plausible = a_proxy == owner_ &&
			std::isfinite(a_input.deltaTime) && a_input.deltaTime > 0.0f && a_input.deltaTime <= 0.2f;
		if (!inv.Ok("processConstraintsCallback", "proxy == owner && plausible frame time", plausible)) {
			return;   // never dereference what we cannot vouch for
		}
		if (Config::Get().useManifoldScan) {
			PushModel::ScanManifold(const_cast<RE::hkpCharacterProxy*>(a_proxy), a_manifold);
		}
	}

	void PushListener::AttachTo(RE::hkpCharacterProxy* a_proxy)
	{
		// ... as in design.md 3.5: owner_ first, then append once if absent.
	}

	bool AttachListener()
	{
		auto* proxy = PlayerProxy();
		if (!proxy) {
			return false;
		}
		GetPushListener().AttachTo(proxy);
		PushModel::ApplyProxyTuning(proxy);   // Approach A: characterMass / characterStrength
		return true;
	}
}
```

### 6.4 `src/main.cpp`

```cpp
#include "PCH.h"
#include "Config.h"
#include "Core/BuildInfo.h"
#include "Core/Health.h"
#include "Hooks/Hooks.h"
#include "Physics/ProxyRegistry.h"
#include "Physics/PushRegistry.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			ha::ProxyRegistry::Get().RebuildNow();      // main thread
			ha::PushRegistry::Get().Clear();
			ha::AttachListener();
			ha::PushModel::LogCalibrationOnce();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			ha::DetachListener();                       // never keep a proxy across a load
			ha::PushRegistry::Get().Clear();
			ha::ProxyRegistry::Get().Invalidate();
			break;
		default:
			break;
		}
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	ha::SetupLog();

	logger::info("PushAside v" PA_VERSION " (Skyrim AE 1.7.104, CommonLibSSE-NG, build " PA_BUILD_ID ") loading");

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("SKSE", OnMessage);
	}

	ha::Config::Get().Load();
	if (!ha::Config::Get().enabled) {
		logger::info("disabled in PushAside.ini - attaching nothing");
		ha::Health::Off("disabled in PushAside.ini");
		return true;
	}

	logger::info("config summary: {}", ha::Config::Get().Summary());
	ha::ProxyRegistry::Get().Init(ha::Config::Get().registryCapacity);
	ha::PushRegistry::Get().Init(ha::Config::Get().registryCapacity);

	// No detour by default. Attaching needs a player proxy, which does not exist
	// until a game is loaded; the main-thread pump (below) attaches on load and
	// re-attaches whenever the player's controller is rebuilt.
	if (ha::Config::Get().useEscalationHooks && !ha::InstallEscalationHooks()) {
		logger::error("escalation hooks requested but none could be verified; continuing listener-only");
	}

	// Main-thread pump: registry refresh, player-proxy attach/re-attach, deferred
	// staggers, buffer sweep, and the bounded stats line.
	ha::ProxyRegistry::Get().StartMainThreadPump();

	logger::info("health: {}", ha::Health::Line());
	return true;
}
```

### 6.5 `config/PushAside.ini` (schema; every value is the compiled-in default)

```ini
; PushAside - Data/SKSE/Plugins/PushAside.ini
; Delete this file to use the compiled-in defaults.

[General]
bEnabled=1

[Listener]
; Path 1 detection: our listener's CharacterInteractionCallback on the player proxy.
bUseCharacterInteraction=1
; Path 2: our listener's ObjectInteractionCallback (corpses, items, ragdolls).
bUseObjectInteraction=1
; Fallback detection inside the listener's ProcessConstraintsCallback (manifold
; scan). Off by default: its ABI is an unverified NG reconstruction (design.md
; 3.7) and it is only enabled after the invariant self-check passes on live data.
bUseManifoldScan=0
; Escalations (design.md 3.8): MinHook detours, each verified against hooks/ first.
; 0 = no detour is installed at all, which is the default and the supported mode.
bUseEscalationHooks=0
; E1: target-side pre-solve injection, so a push coasts after the contact breaks.
bTargetSideInjection=0

[Physics]
; --- Approach A: the proxy's own knobs (rigid bodies only) ---
; characterStrength (+0xBC): max constant force imparted onto moving objects.
; -1 leaves the engine value (FLT_MAX). Try 8000 to make corpses/items fly further.
fCharacterStrength=-1
; characterMass (+0xC0): downward force on bodies underfoot only. -1 leaves vanilla.
fCharacterMass=-1

; --- mass model (ratios matter, absolute values do not) ---
fPlayerMass=100
fDefaultCharacterMass=80
fMassRatioMax=2.0
fHeavyMassRatio=2.5
fHeavyScale=0.25
fHeavyMinSpeed=60

; --- Path 1: character vs character ---
fMinRelSpeed=20
fRestitution=0.15
fPushScale=1.0
fMaxDeltaV=250
fMaxInjectedSpeed=350
; 1/s decay of the per-target buffer. 0 = never decays (do not use).
fPushDamping=6
uPushCooldownMs=200
uMaxPushDurationMs=600
; Fraction of the push the pusher loses. 0 for the player, 0.15 for NPC vs NPC.
fReactionOnPusher=0

; --- state scales ---
fCombatScale=0.5
fOutOfCombatScale=1.0
fSwimScale=0.35
fAirborneScale=0.5
fUnknownTargetScale=0.5
fLift=0
uStaggerCooldownMs=1500
fStaggerDeltaV=120
fStaggerMassExponent=0.5
bStaggerOutOfCombat=0

; --- Path 2: character vs rigid body ---
; 0 = scale the engine's own impulse (safe). 1 = recompute it from relative
; velocity and mass (needs the normal sign pinned; see design.md 4.2).
bObjectCustomImpulse=0
fObjectShoveScale=1.0
fObjectRestitution=0.2
; 0 = uncapped (characterStrength still clamps).
fMaxObjectImpulse=0

[Policy]
; Only meaningful if escalation hooks are enabled: v1's listener cannot see
; NPC-vs-NPC or NPC-vs-player contacts at all.
bNpcVsNpc=0
bPlayerAsTarget=0
bDisableDuringDialogue=1

[Budget]
uMaxInteractionsPerFrame=32
uRegistryCapacity=512
fRegistryRefreshSec=1.0

[Diagnostics]
bDebugPushLog=0
uDebugLogMaxPerSec=32
fCalibrationLogAfterSec=5
```

### 6.6 `src/Config.h` (fields added to HeapSentinel's `Config`)

```cpp
struct Config
{
    // [Listener]
    bool useCharacterInteraction = true;
    bool useObjectInteraction = true;
    bool useManifoldScan = false;
    bool useEscalationHooks = false;
    bool targetSideInjection = false;

    // [Physics] - Approach A inputs (tuning the proxy, not a rival design)
    float characterStrength = -1.0f;   // <0: leave vanilla (FLT_MAX)
    float characterMass = -1.0f;       // <0: leave vanilla (0)

    // mass model
    float playerMass = 100.0f;
    float defaultCharacterMass = 80.0f;
    float massRatioMax = 2.0f;
    float heavyMassRatio = 2.5f;
    float heavyScale = 0.25f;
    float heavyMinSpeed = 60.0f;

    // Path 1
    float minRelSpeed = 20.0f;
    float restitution = 0.15f;
    float pushScale = 1.0f;
    float maxDeltaV = 250.0f;
    float maxInjectedSpeed = 350.0f;
    float pushDamping = 6.0f;
    std::uint32_t pushCooldownMs = 200;
    std::uint32_t maxPushDurationMs = 600;
    float reactionOnPusher = 0.0f;

    // state scales / gates
    float combatScale = 0.5f;
    float outOfCombatScale = 1.0f;
    float swimScale = 0.35f;
    float airborneScale = 0.5f;
    float unknownTargetScale = 0.5f;
    float lift = 0.0f;
    std::uint32_t staggerCooldownMs = 1500;
    float staggerDeltaV = 120.0f;
    float staggerMassExponent = 0.5f;
    bool  staggerOutOfCombat = false;

    // Path 2
    bool  objectCustomImpulse = false;
    float objectShoveScale = 1.0f;
    float objectRestitution = 0.2f;
    float maxObjectImpulse = 0.0f;

    // Policy
    bool npcVsNpc = false;
    bool playerAsTarget = false;
    bool disableDuringDialogue = true;

    // Budget
    std::uint32_t maxInteractionsPerFrame = 32;
    std::uint32_t registryCapacity = 512;
    float registryRefreshSec = 1.0f;

    // Diagnostics
    bool debugPushLog = false;
    std::uint32_t debugLogMaxPerSec = 32;
    float calibrationLogAfterSec = 5.0f;
    // ... (the rest is HeapSentinel's Config, unchanged)
};
```

### 6.7 Physics

#### 6.7.0 `Physics/HkMath.h`

```cpp
#pragma once

namespace ha
{
    inline float Hx(const RE::hkVector4& v) { return _mm_cvtss_f32(v.quad); }
    inline float Hy(const RE::hkVector4& v) { return _mm_cvtss_f32(_mm_shuffle_ps(v.quad, v.quad, _MM_SHUFFLE(1, 1, 1, 1))); }
    inline float Hz(const RE::hkVector4& v) { return _mm_cvtss_f32(_mm_shuffle_ps(v.quad, v.quad, _MM_SHUFFLE(2, 2, 2, 2))); }

    inline RE::hkVector4 Hk(float x, float y, float z) { return RE::hkVector4(x, y, z, 0.0f); }
    inline RE::hkVector4 Scale(const RE::hkVector4& v, float s) { return v * RE::hkVector4(s); }
    inline RE::hkVector4 Add(const RE::hkVector4& a, const RE::hkVector4& b) { return a + b; }
    inline RE::hkVector4 Sub(const RE::hkVector4& a, const RE::hkVector4& b) { return a - b; }

    // Skyrim is Z-up: "horizontal" drops Z.
    inline RE::hkVector4 Horizontal(const RE::hkVector4& v) { return Hk(Hx(v), Hy(v), 0.0f); }

    inline RE::hkVector4 HorizontalNormalize(const RE::hkVector4& v)
    {
        const auto  h = Horizontal(v);
        const float len = h.Length3();
        return len > 1e-4f ? Scale(h, 1.0f / len) : Hk(0.0f, 0.0f, 0.0f);
    }

    inline RE::hkVector4 ClampLength(const RE::hkVector4& v, float maxLen)
    {
        const float len = v.Length3();
        return (maxLen > 0.0f && len > maxLen) ? Scale(v, maxLen / len) : v;
    }
}
```

#### 6.7.1 `Physics/PushRegistry.h`

```cpp
#pragma once

#include "Physics/HkMath.h"

namespace ha
{
    // One pending push per target proxy. Open addressing, fixed capacity, no
    // allocation on the hot path. A stale slot is erased by the main-thread
    // sweeper, not by a destructor, so a proxy freed by the engine cannot leave a
    // dangling entry past one push duration.
    struct PushEntry
    {
        RE::hkpCharacterProxy* proxy = nullptr;
        RE::hkVector4          velocity{};      // injected this frame (engine u/s)
        RE::hkVector4          dir{};           // last push direction (horizontal)
        float                  magnitude = 0.0f;
        std::uint32_t          appliedFrame = 0;  // a manifold can hold several contacts vs one proxy
        std::uint64_t          lastPushAt = 0;
        std::uint64_t          lastStaggerAt = 0;
        std::uint64_t          expiresAt = 0;
    };

    class PushRegistry
    {
    public:
        static PushRegistry& Get();

        void Init(std::uint32_t a_capacity);
        void Clear();

        [[nodiscard]] PushEntry* Find(RE::hkpCharacterProxy* a_proxy);
        PushEntry&               GetOrCreate(RE::hkpCharacterProxy* a_proxy);
        void                     Scale(RE::hkpCharacterProxy* a_proxy, float a_factor);
        void                     Erase(RE::hkpCharacterProxy* a_proxy);
        void                     Sweep(std::uint64_t a_nowMs);
        [[nodiscard]] std::size_t Size() const;

    private:
        [[nodiscard]] std::size_t Hash(RE::hkpCharacterProxy* a_proxy) const
        {
            return (reinterpret_cast<std::uintptr_t>(a_proxy) >> 4) % capacity_;
        }

        std::vector<PushEntry> slots_;
        std::size_t            capacity_ = 0;
        std::size_t            size_ = 0;
    };
}
```

#### 6.7.2 `Physics/PushModel.h/.cpp` (the model of §4)

```cpp
#pragma once

namespace ha
{
    class PushModel
    {
    public:
        // Path 1. Called from the listener's CharacterInteractionCallback and from
        // ScanManifold; idempotent within a frame thanks to the cooldown + take-max.
        static void OnCharacterContact(RE::hkpCharacterProxy* a_self,
                                       RE::hkpCharacterProxy* a_other,
                                       const RE::hkContactPoint* a_contact);

        // Path 2. Called from the listener's ObjectInteractionCallback.
        static void OnObjectContact(RE::hkpCharacterProxy* a_self,
                                    const RE::hkpCharacterObjectInteractionEvent* a_input,
                                    RE::hkpCharacterObjectInteractionResult* a_output);

        // Fallback detection (design.md 3.7), driven by the slot-1 override.
        static void ScanManifold(RE::hkpCharacterProxy* a_self,
                                 const RE::hkArray<RE::hkpRootCdPoint>& a_manifold);

        // Approach A, applied to the player's proxy on load / config change.
        static void ApplyProxyTuning(RE::hkpCharacterProxy* a_playerProxy);

        static void TickMainThread(float a_deltaSec);   // staggers, sweep, stats
        static void LogCalibrationOnce();
    };
}
```

The core of `OnCharacterContact` is the §4.1 pseudocode written out:

```cpp
void PushModel::OnCharacterContact(RE::hkpCharacterProxy* a_self,
                                   RE::hkpCharacterProxy* a_other,
                                   const RE::hkContactPoint* a_contact)
{
    const auto& cfg = Config::Get();
    auto&       buffer = PushRegistry::Get();
    auto&       proxies = ProxyRegistry::Get();

    const RE::hkVector4 p_s = proxies.PositionOf(a_self);
    const RE::hkVector4 p_o = proxies.PositionOf(a_other);

    RE::hkVector4 d = HorizontalNormalize(Sub(p_o, p_s));
    if (d.SqrLength3() < 1e-8f) {
        d = HorizontalNormalize(a_contact->separatingNormal);
        if (d.Dot3(Sub(Horizontal(p_o), Horizontal(p_s))) < 0.0f) {
            d = Scale(d, -1.0f);
        }
    }
    if (d.SqrLength3() < 1e-8f) {
        return;  // coincident centres and a vertical contact: nothing to push along
    }

    const RE::hkVector4 v_rel = Sub(a_self->velocity, a_other->velocity);

    float v_close = std::max(0.0f, d.Dot3(v_rel)) - cfg.minRelSpeed;
    if (v_close <= 0.0f) {
        return;  // gate 5
    }

    const float m_s = proxies.MassOf(a_self, cfg.playerMass);
    const float m_o = proxies.MassOf(a_other, cfg.defaultCharacterMass);

    const float mu = std::clamp(2.0f * m_s / (m_s + m_o), 0.0f, cfg.massRatioMax);
    float       dv = (1.0f + cfg.restitution) * mu * v_close * cfg.pushScale;

    if (m_o > cfg.heavyMassRatio * m_s) {
        if (v_close < cfg.heavyMinSpeed) {
            return;
        }
        dv *= cfg.heavyScale;
    }
    dv = std::min(dv, cfg.maxDeltaV);

    const auto state = proxies.StateOf(a_other);
    dv *= state.inCombat ? cfg.combatScale : cfg.outOfCombatScale;
    dv *= state.swimming ? cfg.swimScale : 1.0f;
    dv *= state.airborne ? cfg.airborneScale : 1.0f;
    if (!state.known) {
        dv *= cfg.unknownTargetScale;   // gate 17
    }

    const std::uint64_t now = FrameClock::NowMs();
    auto&               e = buffer.GetOrCreate(a_other);
    if (now - e.lastPushAt >= cfg.pushCooldownMs) {
        e.velocity  = Scale(d, dv);
        e.lastPushAt = now;
    } else {
        e.velocity = Scale(d, std::max(e.velocity.Length3(), dv));   // take-max, never add
    }
    e.dir       = d;
    e.magnitude = dv;
    e.velocity  = ClampLength(e.velocity, cfg.maxInjectedSpeed);
    e.expiresAt = now + cfg.maxPushDurationMs;

    // step 7: the push itself, at most once per frame. A manifold can hold several
    // contact points against the SAME proxy; without this guard the push is
    // multiplied by the contact count. a_other is a character proxy, so this
    // reaches the target's own solve. Never a position, never pushDelta.
    if (e.appliedFrame != FrameClock::CurrentFrame()) {
        e.appliedFrame    = FrameClock::CurrentFrame();
        a_other->velocity = Add(a_other->velocity, e.velocity);
        if (cfg.reactionOnPusher > 0.0f) {
            a_self->velocity = Sub(a_self->velocity, Scale(e.velocity, cfg.reactionOnPusher));
        }
    }

    const float need = cfg.staggerDeltaV * std::pow(m_o / m_s, cfg.staggerMassExponent);
    if (dv >= need && (state.inCombat || cfg.staggerOutOfCombat) &&
        now - e.lastStaggerAt >= cfg.staggerCooldownMs) {
        e.lastStaggerAt = now;
        StaggerQueue::Get().Push(a_other, dv, d);   // drained on the main thread
    }
}
```

### 6.8 Tooling delta

`tools/gen-hooktable.py` and `tools/check-hooktable.py` are reused from
HeapSentinel **unchanged**: the generator already resolves an AE id to an RVA,
checks that the vtable slot holds that function, computes the `.pdata` extent
and the FNV-1a prologue hash, and emits `HookTableData.gen.h`; the checker
already fails the build when a `HookTargets.def` entry has no matching record.
In v1 the escalation table can be generated but unused; CI still passes because
completeness is about the `.def` list, not about which hooks the config enables.
`hooks/README.md` and the "no byte of SkyrimSE.exe is committed" policy carry
over verbatim.

---

## 7. Implementation plan (ordered, each step independently shippable)

0. **Scaffold** from HeapSentinel (`xmake.lua`, `flake.nix`, `PCH.h`, `Config`,
   `Health`, `Hooks/HookTable*`, `HookVerifier*`, `tools/`), rename the plugin,
   and confirm it builds and loads with no listener and no detour (per
   `build.md`).
1. **Config + INI + diagnostics** (§6.5/6.6), including `Summary()` and a
   `PushAside.log` header. Predicate: deleting the INI yields the defaults; a
   malformed value logs and keeps the default.
2. **Attach and observe, no physics.** `PlayerProxy()`, `AsProxyController()`,
   `AttachTo()`, and a callback that only counts calls and logs, once per
   second: calls to slots 4/5/1, the callback thread id vs the main thread id,
   `characterMass`/`characterStrength` for the player and three NPCs, and the
   player's walking velocity. **This step resolves U1, U2, U3 and U4 with
   evidence and is a hard gate: do not write physics until the log answers
   them.** Predicate: the log shows a non-zero slot-4 count while the player
   walks into a follower. If slot 4 is zero but slot 1 is non-zero, enable
   `bUseManifoldScan` and re-test.
3. **`ProxyRegistry`** on the main thread (actors → proxies → collidables, mass,
   flags) with the seqlock snapshot and `Invalidate()` on `kPreLoadGame`.
4. **`PushRegistry` + the apply step**, driven by a debug trigger (a hotkey that
   pushes the nearest proxy once) so the write-to-`otherProxy->velocity`
   mechanism is proven independently of detection.
5. **Path 1 end to end** (§4.1), with cooldown, damping, expiry, caps.
   Deliverable behaviour: walking into a follower in a corridor moves them
   aside, and they stop when the player stops.
6. **Tuning pass** against the §4.7 calibration line: `fMinRelSpeed`,
   `fPushScale`, `fMaxDeltaV`, `fPushDamping`, `fStaggerDeltaV`.
7. **Stagger**, queued and drained on the main thread, with the
   `IsStaggering()` guard.
8. **Path 2** (§4.2): scale-only first, pinning the normal sign from the log,
   then `bObjectCustomImpulse`.
9. **Approach A knobs** (`ApplyProxyTuning`) and their interaction with Path 2.
10. **Policy gates** (§5) and the negative tests of §8.
11. **Escalations, only if §2's evidence asks for them** (E1 first). Each is a
    config flag turned on after its committed table entry verifies *and* its
    invariant self-check passes on live data.
12. **Soak + packaging**: FOMOD, INI under `SKSE/Plugins/PushAside/`, and the
    long-session stats line (calls/frame, buffer size, invariant failures,
    dropped pushes) so a regression is visible without a debugger.

---

## 8. Test matrix

Every row states the *number that must appear in the log*, not just the
behaviour. "It felt right" is not a result.

| # | scenario | expected | evidence in the log |
|---|---|---|---|
| 1 | walk into a follower in a corridor | follower displaced along `p_o - p_s`, then stops; the player is not stuck | slot-4 calls > 0; ≥1 push with `dv > 0` and `mu ≈ 1.0`; the buffer entry expires ≤600 ms after contact |
| 2 | sprint into the same NPC | larger displacement, capped | `dv` rises with `v_close`, clamps at `fMaxDeltaV` / `fMaxInjectedSpeed` |
| 3 | walk into a giant / mammoth | little or no movement, no stagger | `mu <= 0.4`, `heavyScale` applied, zero stagger lines |
| 4 | crowd of ≥5 NPCs in a doorway | at most one new push per target per 200 ms; frame time unaffected | `interactions/frame <= uMaxInteractionsPerFrame`; buffer size bounded and sweeping |
| 5 | target goes ragdolled mid-push | Path-1 pushes stop; the ragdoll body still reacts via Path 2 | gate-10 skip count increments; no buffer entry survives the ragdoll |
| 6 | both in water | reduced push; no launch onto the shore | gate-16 count increments; `dv` scaled by `fSwimScale` |
| 7 | player in dialogue | no push | gate-15 count increments; zero push lines while `DialogueMenu` is open |
| 8 | NPC walks into the player | vanilla behaviour unchanged | zero pushes with the player as target; the player's own `velocity` is never written (`fReactionOnPusher=0`) |
| 9 | shove a corpse / loose item | corpse slides and tumbles further than vanilla | Path-2 lines with `objectShoveScale ≠ 1`; `dot(normal, objectImpulse)` shows the expected sign |
| 10 | target pinned against a wall | target stops; no interpenetration, no jitter | the injected velocity is written but the target's displacement ≈ 0; no extra pushes beyond the cooldown |
| 11 | save, load, save, load, and die/ragdoll the player | no stale pointer, no crash, pushes work after each load; no push while ragdolled | `listener attached to player proxy 0x…` after each load; orphan-check count > 0 for the retired proxy; `PlayerProxy()` null while ragdolled |
| 12 | attacher idempotence and once-per-frame application | exactly one registration; a push is never applied twice per frame even when the target's manifold holds several contact points | `listeners now N` increments by 1, not 2, across a re-attach; per-frame application count per target == 1 with a 4-point manifold |
| 13 | mounted player / horse | no misidentification, no push | `AsProxyController()` returns null while mounted; slot-4 calls attributed to the player = 0 |
| 14 | dragon in flight | no Path 1; Path 2 refuses | `IsHeavyBody` refusal count increments |
| 15 | **negative test of each gate** — set `fMinRelSpeed=1e6`, `uPushCooldownMs=10^9`, `uMaxPushDurationMs=0`, `uMaxInteractionsPerFrame=0`, `bEnabled=0`, `bUseCharacterInteraction=0` | zero pushes and zero crashes in each case | the matching gate counter increments and the push count stays at zero |
| 16 | **ABI/invariant test** — with a debug build, force the slot-1 override to see a bogus `deltaTime`/proxy | the invariant fires 8×, the manifold scan stays off, the plugin reports `DEGRADED` | `invariant 'proxy == owner && plausible frame time' failed 8 times in a row` |
| 17 | **orphan test** — force a re-attach without detaching from the old proxy, then walk into an NPC | the retired proxy's calls are ignored; no double push | orphan-check count increments; total pushes for one contact unchanged |
| 18 | thread check | if the callback is on a job thread, no game-world access happens there | the one-shot `callback thread=… (main=…)` line; a debug assertion that `ProxyRegistry` is only *read* off the main thread |

Rows 15–18 matter most: per `silent-pass-verification`, a green walk-through
proves the feature works in one case, not that the guards work at all. Each
guard needs a run where it is the *only* thing that could have stopped the push.

---

## 9. Riskiest unknown (and the answer if it goes wrong)

**Does Skyrim's Havok dispatch `CharacterInteractionCallback` at all?** The
vtable slot is populated (verified) and the function exists (verified), but
dispatch is a Havok-internal behaviour that only a live run confirms
(`engine-internals.md` open item 3). This is the one thing that decides whether
the elegant no-detour design is enough.

The answer is built in, twice:

1. `ProcessConstraintsCallback` on the *same listener* runs before every solve
   and receives the manifold, so `ScanManifold` + `ProxyRegistry::ProxyForCollidable`
   reconstructs the same character pair with no detour (§3.7). Its only
   weakness is the unverified NG ABI (U3), which the invariant self-check turns
   into a loud, safe refusal rather than corruption.
2. If both listener paths are unusable, escalation E2
   (`hkpCharacterProxy::UpdateManifold`, AE 62647) is the guaranteed per-step
   entry point and MinHook becomes the mechanism instead of the escalation — the
   committed table and the invariant discipline are already in place for it.

That is why §7 step 2 attaches, instruments and *stops*: the first session
answers U1–U4 with log lines instead of guesses, and the rest of the design is
downstream of those five numbers.

Secondary unknown, in risk order: the callback thread (U2, measured, §3.4), the
slot-1 ABI (U3, self-checked), `characterMass` never being set by the engine
(U4, fallback to `TESRace::data.baseMass`), whether `hkArray::push_back` on a
live `listeners` array is observed safely (U6, main-thread-only + idempotent
attach), and the `ObjectInteractionCallback` normal sign (§4.2, measured).

---

## 10. References

* `research/engine-internals.md` — verified ids, RVAs, vtable slots, prologue
  hashes and struct offsets for 1.7.104 (this repo).
* `research/build.md` — build/deploy on NixOS with xmake + CommonLibSSE-NG.
* `/home/aviallon/Programing/Opensource/HeapSentinel/{DESIGN.md,src/Hooks/*,tools/*}`
  — the plugin architecture this design copies: MinHook where a detour is
  needed, a committed per-build verification table, an ini config, and a
  `Health` verdict that is `DEGRADED` unless everything touched is named and
  verified.
* `Skyrim-A-Pose-Fix/lib/commonlibsse-ng/include/RE/B/bhkCharProxyController.h`,
  `RE/B/bhkCharacterController.h`, `RE/H/hkpCharacterProxy.h`,
  `RE/H/hkpCharacterProxyListener.h`, `RE/H/hkpSimplexSolver.h`,
  `RE/H/hkpRootCdPoint.h`, `RE/T/TESRace.h` — the layouts and APIs used above.
* Havok's own documentation for `hkpCharacterProxyCinfo` (`characterMass`,
  `characterStrength`, `maxCharacterSpeedForSolver`) and
  `hkpCharacterProxyListener` (the two callbacks), reproduced in
  `/tmp/hkpCharacterProxyCinfo.h` and `/tmp/hkpCharacterProxyListener.h`.
* Prior art, for the record: *I'm Walkin' Here* (disables collision between the
  player and allies — the thing this design deliberately does **not** do),
  *Disable Follower Collision*, and Papyrus-level `PushActorAway` gadgets. None
  of them keeps collision on and moves the NPC with the character-controller
  solver.
