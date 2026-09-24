#pragma once

#include <RE/H/hkpCharacterRigidBody.h>
#include <RE/H/hkpCharacterRigidBodyListener.h>

#include <atomic>
#include <cstdint>

// PushAside's character-rigid-body listener: the mirror of PushListener, but for
// the NPC's own hkpCharacterRigidBody.
//
// Unlike hkpCharacterProxy, hkpCharacterRigidBody holds a SINGLE listener
// pointer (hkpCharacterRigidBody+0x28), not a list, and the engine installs the
// owning bhkCharRigidBodyController's listener subobject there. We therefore
// *chain*: we save the existing listener and forward every virtual to it, then
// apply our write in the CharacterCallback.
//
// Where is the callback invoked from? Engine fact, established by reading
// SkyrimSE.exe 1.7.104 (this is the whole reason to try it):
//   hkpCharacterRigidBody vtable[+0x18] (post-simulation subobject) ->
//     thunk RVA 0x0B99910:
//       mov 0x10(%rcx),%rax   ; rax = *(body+0x28) = the listener
//       lea -0x18(%rcx),%r8   ; r8 = the hkpCharacterRigidBody*
//       mov (%rax),%r9
//       jmp *0x18(%r9)        ; listener->CharacterCallback(world, body)
// so CharacterCallback runs in the world's POST-SIMULATION phase, once per
// step, after the solver. Writing the per-frame velocity there is the one phase
// the main-thread `state`/`ctrl`/`rb` writes never reached.
//
// THREADING (same discipline as PushListener): the attachment table and the
// arm payload are written by the main thread while holding bhkWorld's
// worldLock (ApplyPendingPushRequest), and read by the Havok thread inside the
// step, which holds the same lock. They are published through the armed_ atomic
// with acquire/release so a callback that observes armed_==true sees a complete
// payload. Counters and last-observed values are atomics. Nothing here logs.

namespace RE
{
	class bhkCharacterController;
}

namespace pa
{
	class CharacterStepListener final : public RE::hkpCharacterRigidBodyListener
	{
	public:
		~CharacterStepListener() override = default;

		// hkpCharacterRigidBodyListener overrides. CharacterCallback is ours;
		// every other virtual is forwarded verbatim to the saved engine listener
		// so replacing the single pointer cannot change the character's normal
		// collision/slope/mass handling.
		void CharacterCallback(RE::hkpWorld* a_world, RE::hkpCharacterRigidBody* a_characterRB) override;
		void ProcessActualPoints(const RE::hkpWorld* a_world, RE::hkpCharacterRigidBody* a_characterRB,
			const RE::hkpLinkedCollidable::CollisionEntry& a_entry, RE::hkpSimpleConstraintContactMgr* a_mgr,
			RE::hkArray<std::uint16_t>& a_contactPointIds) override;
		void UnweldContactPoints(RE::hkpCharacterRigidBody* a_characterRB,
			const RE::hkpLinkedCollidable::CollisionEntry& a_entry, RE::hkpSimpleConstraintContactMgr* a_mgr,
			const RE::hkArray<std::uint16_t>& a_contactPointIds) override;
		void ConsiderCollisionEntryForSlope(const RE::hkpWorld* a_world, RE::hkpCharacterRigidBody* a_characterRB,
			const RE::hkpLinkedCollidable::CollisionEntry& a_entry, RE::hkpSimpleConstraintContactMgr* a_mgr,
			RE::hkArray<std::uint16_t>& a_contactPointIds) override;
		void ConsiderCollisionEntryForMassModification(const RE::hkpWorld* a_world,
			RE::hkpCharacterRigidBody* a_characterRB, const RE::hkpLinkedCollidable::CollisionEntry& a_entry,
			RE::hkpSimpleConstraintContactMgr* a_mgr, const RE::hkArray<std::uint16_t>& a_contactPointIds) override;

		// Main thread, under the Havok world write lock. Idempotent per body.
		// Returns false when the attachment table is full (then nothing changed).
		[[nodiscard]] bool Attach(RE::hkpCharacterRigidBody* a_body, RE::bhkCharacterController* a_ctrl);

		// Main thread, under the world write lock. Publish a fresh push payload
		// for a_body and arm its application for the window ending at a_endMs.
		void Arm(RE::hkpCharacterRigidBody* a_body, const float a_dir[3], float a_dv,
			std::uint32_t a_formId, std::uint64_t a_endMs);

		// Main thread. Stop applying writes immediately (the attachment itself is
		// left in place; the engine's listener chain is never disturbed twice).
		void Disarm();

		[[nodiscard]] bool Armed() const { return armed_.load(std::memory_order_acquire); }

		// Snapshot of the observable state, safe to call from either thread.
		struct Snapshot
		{
			std::uintptr_t body = 0;
			std::uintptr_t prev = 0;
			std::uintptr_t ctrl = 0;
			bool           armed = false;
			float          dir[3]{};
			float          dv = 0.0f;
			std::uint64_t  endMs = 0;
			std::uint64_t  callbacks = 0;
			std::uint64_t  forwards = 0;
			std::uint64_t  writes = 0;
			std::uint64_t  lastWriteMs = 0;
			float          lastAccel[3]{};   // hkpCharacterRigidBody::acceleration after the write
			float          lastRbVel[3]{};   // character motion.linearVelocity after the write
			float          lastOutBefore[3]{};  // bhkCharacterController::outVelocity before
			float          lastOutAfter[3]{};   // bhkCharacterController::outVelocity after
			float          lastPos[3]{};     // character motion position after the write
		};
		[[nodiscard]] Snapshot Read() const;

	private:
		struct Attachment
		{
			RE::hkpCharacterRigidBody*          body = nullptr;
			RE::hkpCharacterRigidBodyListener*  prev = nullptr;
			RE::bhkCharacterController*         ctrl = nullptr;
		};

		static constexpr std::size_t kMaxAttachments = 8;

		// Main-thread writes (under the world lock), Havok-thread reads (inside the
		// step, which holds the world lock). Bounded and never evicted: an attach
		// that cannot be recorded is refused rather than silently mis-forwarded.
		Attachment               attachments_[kMaxAttachments]{};
		std::atomic<std::size_t> attachCount_{ 0 };

		// Arm payload: written before the release store of armed_, read after the
		// acquire load. armedBody_ is the body the payload targets.
		std::atomic<bool>          armed_{ false };
		std::atomic<std::uint32_t> formId_{ 0 };
		RE::hkpCharacterRigidBody* armedBody_ = nullptr;
		float                      dir_[3]{};
		float                      dv_ = 0.0f;
		std::uint64_t              endMs_ = 0;

		std::atomic<std::uint64_t> callbacks_{ 0 };
		std::atomic<std::uint64_t> forwards_{ 0 };
		std::atomic<std::uint64_t> writes_{ 0 };
		std::atomic<std::uint64_t> lastWriteMs_{ 0 };
		std::atomic<float>         lastAccel_[3]{};
		std::atomic<float>         lastRbVel_[3]{};
		std::atomic<float>         lastOutBefore_[3]{};
		std::atomic<float>         lastOutAfter_[3]{};
		std::atomic<float>         lastPos_[3]{};
	};

	// Static storage: the listener must outlive the bodies it is chained onto.
	[[nodiscard]] CharacterStepListener& GetCharacterStepListener();
}
