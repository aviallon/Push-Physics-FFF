# The character-rigid-body step listener (`push ... steplisten`)

**Status: IMPLEMENTED AND MEASURED IN GAME. The mechanism works mechanically and
produces ZERO displacement. It DISPROVES the last physics lead: the NPC's
post-simulation `CharacterCallback` is not a phase at which a velocity write can
move a standing rigid-body-controller character.**

Date: 2026-09-24. Engine: Skyrim AE 1.7.104. Target: Whiterun Guard
`0x00078780`, a `bhkCharRigidBodyController` (motion type 7). DLL
`4779003f5443caf56a8352cd2be631d6c0f7bf2c43381e0ef133b24fa65a3857`.

## 1. Engine contract, read off `SkyrimSE.exe` (not assumed)

`hkpCharacterRigidBody` does **not** hold a listener list (unlike
`hkpCharacterProxy`'s `+0xC8` array). It holds a **single** pointer:

```
hkpCharacterRigidBody (RE/H/hkpCharacterRigidBody.h)
  +0x00  primary vtable (hkReferencedObject + CheckSupport/GetSupportInfo/GetGround)
  +0x10  hkpEntityListener vtable
  +0x18  hkpWorldPostSimulationListener vtable
  +0x20  hkpRigidBody* character
  +0x28  hkpCharacterRigidBodyListener* listener      <-- ONE pointer
```

* `setListener` is RVA `0x0B98880` (AE id 62449): `mov %rdx,0x28(%rcx)`; it
  addRefs the new listener but never releases the old one. `clearListener` is
  RVA `0x0B988C0` (AE 62450). The only callers are the controller's
  `LoadBinary`/ctor at RVA `0x109A720`.
* The callback is dispatched from the body's **post-simulation** vtable entry
  (`0x0B99910`, the `+0x18` subobject thunk):

  ```
  mov 0x10(%rcx),%rax   ; rax = *(body+0x28) = listener
  lea -0x18(%rcx),%r8   ; r8 = hkpCharacterRigidBody*
  mov (%rax),%r9
  jmp *0x18(%r9)        ; listener->CharacterCallback(world, body)
  ```

  So `CharacterCallback` runs in the world's POST-SIMULATION phase, once per
  step, **after** the solver. This is the crucial fact the whole test rests on.
* The controller that owns the listener is `bhkCharRigidBodyController`:
  primary (`bhkCharacterController`) vtable at offset 0 =
  `VTABLE_bhkCharRigidBodyController[0]` (AE 240580, RVA 0x1A89BF0); the chained
  listener lives at `+0x330` with `[1]` (AE 240583, RVA 0x1A89C98). The
  `hkpCharacterRigidBody*` is `charRigidBody.referencedObject` at controller
  `+0x350` (confirmed by the engine's own getter at RVA 0x109AA00).
* The engine feeds the step from `hkpCharacterRigidBody::SetLinearVelocity`
  (AE id 62456, RVA `0x0B99710`): it stores
  `acceleration = (newVel - motion.linearVelocity)/timestep * k` and calls
  `hkpMotion::SetLinearVelocity(newVel)`. **`bhkCharRigidBodyController`'s own
  `CharacterCallback` ends with exactly this call**, which is what made this
  listener a plausible place to inject the per-frame velocity.

`src/CharacterStepListener.cpp` therefore chains: it saves the existing listener
(the controller's `+0x330` subobject) and forwards all five non-`CharacterCallback`
virtuals to it, so replacing the single pointer cannot change normal
collision/slope/mass handling. It calls `hkpCharacterRigidBody::SetLinearVelocity`
**after** the engine's callback, and also writes `bhkCharacterController::outVelocity`.

## 2. Measurement

Run: `tools/ingame-harness.sh` (`pa-steplisten2-20260924-044749`), `trace on
every 1`, `watch 0x00078780`, 6 s baseline, `pushdry`, then
`push 0x00078780 120 steplisten`, 20 s recovery.

* **Attach/applied:** `steplisten applied=1 body=0x790A96C0 prev=0x793C3530`;
  `calls=1851 forwards=1851 writes=51` (`push` at ms 68497; 600 ms window).
  The engine callback really fired for this body (1851 times) and every call was
  forwarded to the controller.
* **Live velocity after the write:** `watch_ctrl_v` and `watch_rb_v` read exactly
  `(-119.51, -10.84, 0.00)` — the requested `dir*dv` = 120.00 u/s — for the
  window. `outVelocity` was the same and **persisted for the entire 20 s
  recovery** (`|out_vx|>1` in all 2141 post-window rows).
* **Displacement:** `0.00000 u` in the window, `0.00000 u` over the 20 s
  recovery, max per-frame position step `0.00000 u`. The guard's position is
  bit-identical before, during and after. Baseline stationarity was absolute
  (extent 0.0000 over 541 frames), so the zero is not noise hiding a small push.
* **Direction:** no displacement vector exists to compare against `dir`; the
  answer is zero along every axis, including the requested `(-0.9955,-0.0903,0)`.
* **Survival:** `game_running_at_collect=yes`, constraints advanced (7456),
  `step-listener` stats advanced (260→684→1108→1528→1945 calls), no new crash
  log, CrashLogger banner only, 270 saves byte-identical.

## 3. What this rules out

Prior measurements (see the shared store `pushaside-state-and-knock`) already
showed that writing `motion.linearVelocity` / `initialVelocity` / `velocityTime`
from the **main thread under `world->worldLock`** is inert. This test rules out
the remaining phase:

* The **post-simulation `CharacterCallback`** is a real, reachable phase (we were
  called 1851 times), and a write there reaches both
  `hkpCharacterRigidBody::SetLinearVelocity`'s targets
  (`motion.linearVelocity` + `acceleration`) and `bhkCharacterController::outVelocity`.
  Neither is integrated: the position is invariant.
* `outVelocity` is **not** the per-frame movement input for a
  `bhkCharRigidBodyController` NPC. It persists when written (here for 20 s) and
  nothing reads it back for movement. The earlier `state` result (fields persist,
  `outVelocity` stayed 0) and this result (outVelocity persists, nothing moves)
  are the two halves of the same negative.
* The engine's own `CharacterCallback` re-derives the character's velocity every
  frame and calls `SetLinearVelocity` itself; because it runs **after** the step,
  any externally written velocity is either the last write of the frame (read
  back by `GetLinearVelocityImpl`, as the trace shows) and then overwritten
  before the next step's integration, or simply never used by the character
  motion.

Conclusion: for a standing `bhkCharRigidBodyController` character, the engine
does not move the body from any velocity field an SKSE plugin can write, at any
phase we can reach (main thread under the world lock, or the character's own
post-simulation callback). The only in-engine displacement measured is the
directionless ~1.4 cm `KnockExplosion` stagger; a large directional push on this
path would require the ragdoll/character-state (animation) machinery, which is
explicitly out of scope. This is the honest limit of the velocity-write design.
