# Prior art & Havok semantics - physics-based pushing of characters (Skyrim SE/AE)

*(Research compiled 2026-09-23 by a read-only research agent. Reddit was unreachable (network-security block) and Nexus search is JS-driven, so those two channels are only partially covered - see Gaps.)*

## Verdict
No mod, SKSE plugin or WIP was found that implements **physics-based character-vs-character pushing** (player shoves NPCs through Havok's character-controller pipeline). Everything that exists is one of: collision disable, animation/stagger push, collision-size tweak, or collision toggling. The Havok mechanism that *would* do it is documented but implemented by nobody publicly: `hkpCharacterProxyListener::characterInteractionCallback` (called when a character hits another character; empty by default) and its Skyrim implementation `bhkCharProxyController`.

## 1. Prior art
### 1.1 Collision disable ("walk through them")
- **I'm Walkin' Here** (Fudgyduff) - https://www.nexusmods.com/skyrimspecialedition/mods/27742. STEP forum: "Disables collision between player and NPCs so that they don't push the player or block narrow passages." and "Collision is disabled only between you (the player) and the related NPCs." (https://stepmodifications.org/forum/topic/16894-im-walkin-here-by-fudgyduff/).
- **Disable Follower Collision** (Felisky384) - older, per the STEP thread not updated for AE/1.6.x.

### 1.2 Animation / stagger 'push power'
- **Get Out Of My Way - Push NPCs** (Volek & elr0y7), v1.1, https://www.nexusmods.com/skyrimspecialedition/mods/61095. Tags: Gameplay, **Animation - New**, Quality of Life. Requirements: SkyUI (MCM) + SKSE64 (documented as being *for the stagger modifier*). Mechanism = animated shove + stagger, not a Havok impulse. Caveat: the description prose could not be read in full (Nexus behind JS), so this classification rests on the tags + requirements table.

### 1.3 Collision-size tweaks
- **Dynamic Collision Adjustment** (Ersh), https://www.nexusmods.com/skyrimspecialedition/mods/76783 - "Adjusts the character controller collision while sneaking and swimming so you can fit under objects. Fixes some races not being affected by Actor Scale."
- Nexus forum request thread "Skyrim SE - Mod Idea: Bigger Collisionbox for Humanoids" (2024-04-30): people enlarge the player radius as a hack and hit side effects ("Character starts ice skating", "NPC vs NPC combat is not affected"). https://forums.nexusmods.com/topic/13483065-skyrim-se-mod-idea-bigger-collisionbox-for-humanoids/

### 1.4 Adjacent SKSE plugins that touch actor collision/controllers
Actor Collision Manager (mods/160060), HIGGS (github.com/adamhynek/higgs), ActiveRagdoll (github.com/adamhynek/activeragdoll), Precision and DynamicCollisionAdjustment (github.com/ersh1/...), plus f4se/sfse/skse64 and TiltedEvolution. None pushes characters.

### 1.5 Code-index evidence (grep.app, 2026-09-23)
- `CharacterInteractionCallback`: 4 hits - 3 in Ryan-rsm-McKenzie/CommonLibSSE (2 declarations + an empty default body) and 1 declaration in llde/xOBSE. **No Skyrim plugin implements it.**
- `bhkCharProxyController`: 26 hits across CommonLibSSE, higgs, activeragdoll, DynamicCollisionAdjustment, Precision, f4se, sfse, skse64, TiltedEvolution.
- `maxCharacterSpeedForSolver`: 6 hits - CommonLibSSE header (Skyrim), Babylon.js, vircadia-world, deca. **No Skyrim mod tweaks it publicly.**

### 1.6 Gaps
- Reddit blocked ("You've been blocked by network security") - the relevant r/skyrimmods threads (1c6yszr, otqnx4, xzae43) were only visible as titles.
- Nexus search returns unfiltered results (JS app); no systematic sweep possible.
- Chinese/Japanese/Patreon/Discord-only: searched (e.g. 上古卷轴5 推开NPC mod 物理 碰撞 推动) - nothing found. Absence of evidence, not proof.
- Only push-adjacent WIP found: **Modern NPC Pathing** (mods/185413) - NPCs vault/step around obstacles (AI, not physics).

## 2. Havok semantics
### Sources
Havok 6.6.0 headers + demos vendored in https://github.com/nitaigao/engine-game-factions (`Source/Physics/Utilities/CharacterControl/CharacterProxy/`, `Demo/Demos/Physics/UseCase/CharacterControl/CharacterProxy/`), and CommonLibSSE-NG's RE'd copies (https://ng.commonlib.dev/hkp_character_proxy_8h_source.html).
**The implementation is not public**: the only public SDK-source mirror (sigmaco/havok-v5.1.0r1-b2007.09.19) has `.cpp` bodies replaced by "// Content removed automatically by executive order 129/2026'07'25, in response to a new requisition by Microsoft Corporation." (verified on `hkpCharacterProxy.cpp`).

### characterStrength (default HK_REAL_MAX)
> "The maximum constant force that the character controller can impart onto moving objects. By default this is HK_REAL_MAX, i.e. the character controller is infinitely strong." (hkpCharacterProxyCinfo.h)
Demo: "// This value will affect how much the character is able to push other objects around." `cpci.m_characterStrength = 5000;` (CharacterInteractionDemo.cpp).

### characterMass (default 0)
> "The mass of the character. This value is only used to apply an extra downward force to dynamic rigid bodies that the character is standing on. By default this value is 0 [...] It should only be set to a positive value if you do not apply gravity from your state machine when the character is on the ground."
Demo: "// This value will affect how much the character pushes down on objects it stands on." `cpci.m_characterMass = 100;`
Vertical-only. Not a lateral shove lever.

### maxCharacterSpeedForSolver (default 10)
> "This value is used to clip the character's velocity when it is being 'squeezed' by two moving planes. [...] If this velocity is exceeded by the character solver when solving parallel planes, the solver solves the planes independently. The result is that instead of moving at a high velocity, the character may penetrate one of the planes (based on plane priorities)."
Babylon.js reimplements the same field (`maxCharacterSpeedForSolver = 10.0`).

### Who gets pushed (simplex priorities)
CharacterPriorityDemo help string:
> "By default, surfaces are automatically given one of three priorities based on their motion type: 0 : Dynamic Motion types, 1 : Keyframed motion, 2 : Fixed motion. [...] we explicitly set each body's priority dynamically through the processConstraints() callback."
So: when the solver cannot satisfy all planes, **priority decides which plane is violated** (who 'loses'). A listener can rewrite `input.m_constraints[i].m_priority` and `m_velocity` before the solve.

**Character-vs-character is NOT resolved by that path.** Havok reports it via `characterInteractionCallback(proxy, otherProxy, contact)` - "Called when the character interacts with another character" - with an empty default body. Corroboration: `MultipleCharactersDemo.cpp` creates 100 proxies, adds **no** listener, and its update loop only does `setLinearVelocity` + `integrate`; there is no proxy-vs-proxy response anywhere.

### Object impulse path
`objectInteractionCallback` = "Called when the character interacts with another (non fixed or keyframed) rigid body." The event/result comments are quoted in the listener header: `m_objectImpulse` = "The magnitude of the impulse that will be applied if not overridden"; `m_projectedVelocity` = "The magnitude of the relative velocity along the normal"; `m_objectMassInv` = "Mass information for the object (projected along the normal)"; result `m_objectImpulse` = "The impulse that will be applied to object", `m_impulsePosition` = "The point in world space where the object impulse will be applied".

## 3. Listener callbacks
Havok 6.6 signatures (hkpCharacterProxyListener.h): the five virtuals above, with `processConstraintsCallback(const hkArray<hkpRootCdPoint>& manifold, hkpSimplexSolverInput& input)`.

**Skyrim difference (critical for us):** the RE'd copy passes the proxy as an extra first arg and const-qualifies it for the first three:
```cpp
virtual void ProcessConstraintsCallback(const hkpCharacterProxy*, const hkArray<hkpRootCdPoint>&, hkpSimplexSolverInput&);
virtual void ContactPointAddedCallback(const hkpCharacterProxy*, const hkpRootCdPoint&);
virtual void ContactPointRemovedCallback(const hkpCharacterProxy*, const hkpRootCdPoint&);
virtual void CharacterInteractionCallback(hkpCharacterProxy*, hkpCharacterProxy*, const hkContactPoint&);
virtual void ObjectInteractionCallback(hkpCharacterProxy*, const hkpCharacterObjectInteractionEvent&, hkpCharacterObjectInteractionResult&);
```
(vtable 01..05; an SKSE override must match these, not the SDK header.)

Typical implementations found:
- **ZeroPlanesCharacterInteractionListener** (CharacterInteractionDemo.cpp): in `processConstraintsCallback`, for each manifold point get `hkGetRigidBody(...)`; if it is a non-fixed/keyframed body, zero `input.m_constraints[i].m_velocity`, then zero the remaining slope planes. Comment: "This listener can be used to prevent objects from moving the character at all. It should only be used if the character strength has been set to REAL_MAX." and "WARNING: This only works when the character is not in a moving environment, as the velocities are zeroed." (In the demo it is disabled by default.)
- **MyCharacterPriorityListener** (CharacterPriorityDemo.cpp): writes body property values into `input.m_constraints[i].m_priority`.
- No implementation of `characterInteractionCallback` (i.e. a real push response) was found anywhere - only the empty default in CommonLibSSE.

## 4. The Skyrim class
`RE::bhkCharProxyController : public hkpCharacterProxyListener (0x000), public bhkCharacterController (0x010)` - it overrides all five listener callbacks (vtable 01-05) plus `GetLinearVelocityImpl`/`SetLinearVelocityImpl`; has `bhkCharacterProxy proxy` at **0x340**, `hkpCharacterProxy* GetCharacterProxy() const`, `sizeof == 0x5B0`. Sources: https://raw.githubusercontent.com/alandtse/CommonLibSSE-NG/ng/include/RE/B/bhkCharProxyController.h, https://raw.githubusercontent.com/alandtse/CommonLibSSE-NG/ng/include/RE/B/bhkCharacterProxy.h, grep.app hit in CommonLibSSE.
`RE::bhkCharacterProxy` wraps the `hkpCharacterProxy` (GetWorld1 dereferences it) and contains `bhkCharacterPointCollector ignoredCollisionStartCollector` at 0x020 - i.e. the wrapper already carries collision-filtering machinery.
Runtime confirmation: SkyRP crash log (SkyrimSE 1.5.97) shows `bhkCharProxyController*` at `Actor::InitHavokImpl` and `ahkpCharacterProxy*` in registers - https://forum.skyrp.ru/threads/no41-krash.127/
**Unknown:** what vanilla `bhkCharProxyController::CharacterInteractionCallback` does. Behavioural evidence that NPC-vs-NPC/player-vs-NPC pushing happens in vanilla: STEP thread - "Any other NPC walking into you won't push you, they'll go through you, but the same NPC walking into Balgruuf will push him"; plus the common complaint that NPCs shove the player aside. Reversing that one function is the highest-value next step.

## 5. SKSE / CommonLibSSE-NG discussions
- GitHub issue search `hkpCharacterProxy repo:alandtse/CommonLibSSE-NG` -> **0 results**. No discussion of characterMass/characterStrength/proxy listeners.
- grep.app: no Skyrim plugin reads or writes characterStrength / characterMass / maxCharacterSpeedForSolver; only CommonLibSSE declares them (floats at 0xB8/0xBC/0xC0; `listeners` hkArray at 0xC8).
- Plenty of plugins already duplicate the `bhkCharProxyController` struct (HIGGS, ActiveRagdoll, Precision, DynamicCollisionAdjustment) - there is no shared API, which means writing `proxy->characterStrength` etc. from a plugin is trivially possible but untrodden.

## 6. Exact levers for 'player physically pushes NPCs'
1. **Character-vs-character callback (the only true lever).** Implement `CharacterInteractionCallback` - either by overriding `bhkCharProxyController`'s vtable slot 04 or (cleaner) by adding our own `hkpCharacterProxyListener` to the player's proxy via `hkpCharacterProxy::addCharacterProxyListener`/`m_listeners`. Havok hands us `(proxy, otherProxy, contact)` and does nothing itself; the push must be produced by us writing velocity/displacement onto `otherProxy` (or into its owning controller). **No existing mod does this.**
2. **`processConstraintsCallback`** - zero/scale surface `m_velocity` and rewrite `m_priority` (defaults: dynamic 0 < keyframed 1 < fixed 2) so the pusher is not stopped by the pushee when planes are unsatisfiable.
3. **`characterStrength`** - the constant-force cap on the rigid-body push path (HK_REAL_MAX by default; demo uses 5000). Only matters for non-proxy bodies.
4. **`characterMass`** - vertical only; not for shoving.
5. **`maxCharacterSpeedForSolver`** (10) - avoids the squeeze fallback where the character penetrates a plane; a tuning knob, not the push mechanism.
6. Fallback that is *not* character-controller physics: apply an impulse/displacement to the NPC's rigid body when the player's proxy contacts it (scripted force - same family as the stagger mods).

