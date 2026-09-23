#pragma once

#include <RE/H/hkpContactListener.h>

#include <atomic>
#include <cstdint>

namespace RE
{
	class Actor;
	class bhkCharacterController;
	class hkpCharacterProxy;
	class hkpRigidBody;
	class TESObjectREFR;
}

// A process-lifetime hkpContactListener registered on the player's Havok world,
// instrumentation only: it never pushes. It answers one question the manifold
// scan cannot - does the engine's world-contact channel deliver player<->actor
// collisions, and with what contact point/normal. Precision demonstrates the
// channel works and reaches actor bodies via hkpRigidBody::GetUserData().
//
// THREADING: CollisionAddedCallback/ContactPointCallback run during the physics
// step on the physics thread (the same 240/s channel as the character-proxy
// listener). Every counter here is a std::atomic; the bounded log is written at
// most kMaxLogs times per session, so nothing allocates on the hot path. The
// object is a static that outlives the world, and registration reuses it.

namespace pa
{
	class WorldContactListener final : public RE::hkpContactListener
	{
	public:
		// Once per new collision pair; classifies the two bodies into
		// player-character / actor / object / other buckets.
		void CollisionAddedCallback(const RE::hkpCollisionEvent& a_event) override;

		// Per contact point. Used only to emit the bounded player<->actor
		// diagnostic (contact point + separating normal); counts nothing.
		void ContactPointCallback(const RE::hkpContactPointEvent& a_event) override;

		struct Stats
		{
			std::uint64_t collisionAdded = 0;  // total new collision pairs seen
			std::uint64_t pcActor = 0;         // player <-> actor
			std::uint64_t pcObject = 0;        // player <-> non-actor object (userData != null)
			std::uint64_t actorActor = 0;      // actor <-> actor (neither is the player)
			std::uint64_t other = 0;           // anything with a null userData

			// Phantom-collidable control, with the asymmetry that makes it safe to
			// read: hkpCollisionEvent::bodies is typed hkpRigidBody*[2] and a phantom is
			// not a rigid body, so ZERO proves nothing at all (the channel may be
			// rigid-body-only by construction). A non-zero is only SUGGESTIVE, not proof:
			// it says a body's collidable matched a REGISTERED phantom's collidable,
			// which requires a live (non-stale) registry entry, and it assumes the
			// engine is willing to route a non-rigid-body through a hkpRigidBody* slot.
			std::uint64_t bodyPhantom = 0;
			std::uint64_t bodyPhantomPlayer = 0;
			// Player identity by collision group (CFilter bits 16-31). Vacuous when
			// the phantom's group is 0, which is warned about once.
			std::uint64_t pcGroup = 0;
			// Superset tag, explicitly NOT a player signal: "exactly one side
			// unidentified, the other side an actor" fires for every NPC-vs-world pair,
			// and also for player-vs-null, since the player IS an Actor. Kept only so
			// the log shows how large that bucket is.
			std::uint64_t nullVsActor = 0;
			// Contact-point diagnostic lines only: the collision-added and bump-record
			// lines have their own separate caps, so this does NOT total the log volume.
			std::uint64_t logged = 0;
		};

		[[nodiscard]] Stats Snapshot() const;

	private:
		// Separate budgets per diagnostic stream. A single shared budget let the
		// far-more-frequent contact-point callback take the window first and starve
		// the collision-added line to zero - the failure mode this probe exists to
		// avoid. Non-atomic check-then-act across callback threads can still
		// overshoot kMaxLogs by a line or two; that is bounded and acceptable.
		[[nodiscard]] static bool TryClaimLogSlot(std::atomic<std::uint64_t>& a_logged,
			std::atomic<std::uint64_t>& a_lastMs);

		std::atomic<std::uint64_t> collisionAdded_{ 0 };
		std::atomic<std::uint64_t> pcActor_{ 0 };
		std::atomic<std::uint64_t> pcObject_{ 0 };
		std::atomic<std::uint64_t> actorActor_{ 0 };
		std::atomic<std::uint64_t> other_{ 0 };
		std::atomic<std::uint64_t> bodyPhantom_{ 0 };
		std::atomic<std::uint64_t> bodyPhantomPlayer_{ 0 };
		std::atomic<std::uint64_t> pcGroup_{ 0 };
		std::atomic<std::uint64_t> nullVsActor_{ 0 };
		std::atomic<std::uint64_t> logged_{ 0 };           // contact-point lines
		std::atomic<std::uint64_t> lastLogMs_{ 0 };
		std::atomic<std::uint64_t> collisionLogged_{ 0 };  // collision-added lines
		std::atomic<std::uint64_t> collisionLastMs_{ 0 };
	};

	[[nodiscard]] WorldContactListener& GetWorldContactListener();

	// Main thread, idempotent, cheap. Resolves the player's cell -> bhkWorld ->
	// hkpWorld and registers on each new world. Safe to call every frame.
	void EnsureWorldContactRegistration();

	// Main thread, kPreLoadGame: forget the last world so a reload re-registers.
	void ResetWorldContactRegistration();

	// Main thread, idempotent, cheap. Records the player's collision group (from the
	// registry's trusted controller -> char proxy -> shape phantom path) so the
	// callback can tag bodies by group, and reports once whether the ENGINE's
	// collidable->refr map resolves the player's phantom: hkpWorldObject::
	// GetUserData() is TESHavokUtilities::FindCollidableRef, an engine lookup, so a
	// null userData does NOT prove the body is not the player's.
	void EnsurePlayerBodyIdentity();

	// Main thread. Reads the engine's OWN record of the last bump
	// (bhkCharacterController::bumpedBody / bumpedCharCollisionObject) and logs it
	// when that pair changes, capped at kBumpLogMax lines.
	//
	// It also drives bump-record detection: when bumpedCharCollisionObject resolves
	// to a non-player Actor's character proxy, that proxy is published into a
	// single slot for the physics thread to consume. Resolution (GetUserData ->
	// Actor -> GetCharController) happens HERE, on the main thread; nothing on the
	// physics thread touches the game world to find the target.
	//
	// SEMANTICS ARE UNVERIFIED. Those fields appear in research/ as an offset list
	// only; nothing establishes that they are populated when the PLAYER bumps an
	// NPC (they may be for object or ragdoll bumps instead). This measures "are
	// these fields non-null on the player controller, and what do they resolve to";
	// only a live run can say what that means. Do NOT read its output as "the
	// player bumped an NPC".
	void ProbePlayerBumpRecord();

	// Physics thread. Consume the pending bump-detected target proxy. This is a
	// single-slot exchange(nullptr), so the value is applied at most once and a
	// null is never returned as a target. Returns nullptr when nothing is pending.
	[[nodiscard]] RE::hkpCharacterProxy* TakePendingBumpTarget();

	// Physics thread. Record that the character-vs-character model actually
	// changed this target's velocity through the bump path. Counts are cumulative;
	// the first occurrence is latched so the main thread can log it once with the
	// target's actor formID/name (which must not be resolved off-thread).
	void NoteBumpPushApplied(RE::hkpCharacterProxy* a_target, float a_dv);

	// Main thread: the engine's current bump record, resolved along the full
	// chain (charBody -> refr -> Actor -> controller -> proxy). Names are NOT
	// resolved here; the caller has the Actor*. Used by the `bump` command.
	struct BumpRecordView
	{
		RE::hkpRigidBody*           charBody = nullptr;
		RE::TESObjectREFR*          refr = nullptr;
		RE::Actor*                  actor = nullptr;
		RE::bhkCharacterController* ctrl = nullptr;
		RE::hkpCharacterProxy*      target = nullptr;
		float                       force = 0.0f;
	};
	[[nodiscard]] BumpRecordView ReadBumpRecord();

	// Distinct target proxies bump-record detection has resolved this session, and
	// pushes actually applied through this path. Main thread (stats line).
	[[nodiscard]] std::uint64_t BumpTargetCount();
	[[nodiscard]] std::uint64_t BumpPushAppliedCount();

	// Main thread. Emit the deferred one-shot "first push applied via the bump
	// path" line, resolving the target's actor formID/name here. Bounded to a
	// single line for the session.
	void ReportBumpDetection();
}
