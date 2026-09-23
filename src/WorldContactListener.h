#pragma once

#include <RE/H/hkpContactListener.h>

#include <atomic>
#include <cstdint>

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

			// Phantom-collidable control. NOTE THE ASYMMETRY: hkpCollisionEvent::bodies
			// is typed hkpRigidBody*[2] and a phantom is not a rigid body, so this
			// being NON-ZERO would prove that character phantoms do reach this channel,
			// while ZERO proves nothing at all (the channel may be rigid-body-only by
			// construction). It is kept because a single non-zero is decisive, never as
			// the basis for reading a zero.
			std::uint64_t bodyPhantom = 0;
			std::uint64_t bodyPhantomPlayer = 0;
			// Player identity by collision group (CFilter bits 16-31). Vacuous when
			// the phantom's group is 0, which is warned about once.
			std::uint64_t pcGroup = 0;
			// Superset tag, explicitly NOT a player signal: "exactly one side
			// unidentified, the other side an actor" also fires for every NPC-vs-world
			// pair. Kept only to show that bucket is large.
			std::uint64_t nullVsActor = 0;
			std::uint64_t logged = 0;  // bounded diagnostic lines emitted
		};

		[[nodiscard]] Stats Snapshot() const;

	private:
		// Shared budget for the bounded diagnostics, so the two callbacks cannot
		// double the log volume.
		[[nodiscard]] bool TryClaimLogSlot();

		std::atomic<std::uint64_t> collisionAdded_{ 0 };
		std::atomic<std::uint64_t> pcActor_{ 0 };
		std::atomic<std::uint64_t> pcObject_{ 0 };
		std::atomic<std::uint64_t> actorActor_{ 0 };
		std::atomic<std::uint64_t> other_{ 0 };
		std::atomic<std::uint64_t> bodyPhantom_{ 0 };
		std::atomic<std::uint64_t> bodyPhantomPlayer_{ 0 };
		std::atomic<std::uint64_t> pcGroup_{ 0 };
		std::atomic<std::uint64_t> nullVsActor_{ 0 };
		std::atomic<std::uint64_t> logged_{ 0 };
		std::atomic<std::uint64_t> lastLogMs_{ 0 };
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
	// (bhkCharacterController::bumpedBody / bumpedCharCollisionObject, plain fields
	// at +0x2C0/+0x2C8) and logs it when it changes. No hook, no vtable call: this
	// is the channel the character solver itself populates, and
	// bumpedCharCollisionObject is specifically the character collision object the
	// controller bumped.
	void ProbePlayerBumpRecord();
}
