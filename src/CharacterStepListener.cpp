#include "PCH.h"

#include "CharacterStepListener.h"

#include "FrameClock.h"
#include "HkMath.h"

#include <RE/B/bhkCharacterController.h>
#include <RE/H/hkArray.h>
#include <RE/H/hkpLinkedCollidable.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkpRigidBody.h>

#include <cmath>

// CommonLibSSE-NG declares hkpCharacterRigidBodyListener's virtuals but does not
// define them (there is no hkpCharacterRigidBodyListener.cpp), because in the
// engine they live in the game binary. Deriving from the class makes the
// compiler emit the base subobject vftable, which references those symbols, so
// the link needs them. Our derived class overrides every one of them and the
// base vftable is only a transient construction state - never dispatched to from
// the script - so the definitions below are deliberately inert. Defining them
// here (and not in the vendored headers) keeps the change local and reviewable.
namespace RE
{
	hkpCharacterRigidBodyListener::~hkpCharacterRigidBodyListener() = default;
	void hkpCharacterRigidBodyListener::CharacterCallback(hkpWorld*, hkpCharacterRigidBody*) {}
	void hkpCharacterRigidBodyListener::ProcessActualPoints(const hkpWorld*, hkpCharacterRigidBody*,
		const hkpLinkedCollidable::CollisionEntry&, hkpSimpleConstraintContactMgr*, hkArray<std::uint16_t>&)
	{
	}
	void hkpCharacterRigidBodyListener::UnweldContactPoints(hkpCharacterRigidBody*,
		const hkpLinkedCollidable::CollisionEntry&, hkpSimpleConstraintContactMgr*, const hkArray<std::uint16_t>&)
	{
	}
	void hkpCharacterRigidBodyListener::ConsiderCollisionEntryForSlope(const hkpWorld*, hkpCharacterRigidBody*,
		const hkpLinkedCollidable::CollisionEntry&, hkpSimpleConstraintContactMgr*, hkArray<std::uint16_t>&)
	{
	}
	void hkpCharacterRigidBodyListener::ConsiderCollisionEntryForMassModification(const hkpWorld*,
		hkpCharacterRigidBody*, const hkpLinkedCollidable::CollisionEntry&, hkpSimpleConstraintContactMgr*,
		const hkArray<std::uint16_t>&)
	{
	}
}

namespace pa
{
	namespace
	{
		// hkpCharacterRigidBody::SetLinearVelocity(newVel, timestep), engine AE id
		// 62456, RVA 0x0B99710. Read off SkyrimSE.exe 1.7.104: it stores
		// acceleration = (newVel - motion.linearVelocity)/timestep * k and then
		// calls hkpMotion::SetLinearVelocity(newVel). This is the exact call
		// bhkCharRigidBodyController::CharacterCallback makes to feed the next
		// step, so using it is the engine's own input path, not a guess.
		//
		// Only the AE id is verified; this plugin is built for AE 1.7.104
		// (ENABLE_SKYRIM_AE, versionlib-1-7-104-0), so the SE slot is 0 and a
		// non-AE build must not reach it (comment, never a silent guess).
		using SetVelFn = void (*)(RE::hkpCharacterRigidBody*, const RE::hkVector4&, float);
		[[nodiscard]] SetVelFn SetLinearVelocityFn()
		{
			static REL::Relocation<SetVelFn> func{ RELOCATION_ID(0, 62456) };
			const auto                       address = func.address();
			return address != 0 ? reinterpret_cast<SetVelFn>(address) : nullptr;
		}

		void StoreVec3(std::atomic<float> (&a_dst)[3], const RE::hkVector4& a_v)
		{
			a_dst[0].store(Hx(a_v), std::memory_order_relaxed);
			a_dst[1].store(Hy(a_v), std::memory_order_relaxed);
			a_dst[2].store(Hz(a_v), std::memory_order_relaxed);
		}
	}

	void CharacterStepListener::CharacterCallback(RE::hkpWorld* a_world, RE::hkpCharacterRigidBody* a_characterRB)
	{
		callbacks_.fetch_add(1, std::memory_order_relaxed);
		if (!a_characterRB) {
			return;
		}

		// Find the body this callback belongs to. The engine calls us only for
		// bodies we chained onto, so a miss means the table and the engine
		// disagree - forward to the saved listener anyway if we can, so a miss
		// cannot silently disable that character.
		const auto count = attachCount_.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < count && i < kMaxAttachments; ++i) {
			Attachment& a = attachments_[i];
			if (a.body != a_characterRB) {
				continue;
			}
			if (a.prev && a.prev != this) {
				a.prev->CharacterCallback(a_world, a_characterRB);
				forwards_.fetch_add(1, std::memory_order_relaxed);
			}
			if (!armed_.load(std::memory_order_acquire) || a_characterRB != armedBody_ || !a.ctrl) {
				return;
			}
			// Expire inside the callback (callback-confined once the main thread's
			// window has passed). A later arm re-publishes a fresh payload.
			const std::uint64_t now = FrameClock::NowMs();
			if (now >= endMs_) {
				armed_.store(false, std::memory_order_release);
				return;
			}

			// The per-frame timestep the engine feeds SetLinearVelocity: the
			// controller's stepInfo.deltaTime (+0x88). Clamp to a sane step so a
			// bad value cannot produce an enormous acceleration.
			float dt = a.ctrl->stepInfo.deltaTime;
			if (!(dt > 1.0e-4f) || dt > 0.2f) {
				dt = 1.0f / 60.0f;
			}

			const RE::hkVector4 desired = Hk(dir_[0] * dv_, dir_[1] * dv_, dir_[2] * dv_);
			const auto           outBefore = a.ctrl->outVelocity;

			// The engine's own character velocity input. Called AFTER the saved
			// listener's CharacterCallback (above), so this is the last write to the
			// motion velocity before the next step integrates it.
			if (auto setVel = SetLinearVelocityFn()) {
				setVel(a_characterRB, desired, dt);
			}
			// The movement accumulator the task names. Written too, and observable in
			// the trace's watch_out_* columns, so "outVelocity went non-zero after the
			// write" is a measured row rather than a claim.
			a.ctrl->outVelocity = desired;

			StoreVec3(lastAccel_, a_characterRB->acceleration);
			const auto motion = a_characterRB->character ? a_characterRB->character->motion.linearVelocity : RE::hkVector4{};
			StoreVec3(lastRbVel_, motion);
			StoreVec3(lastOutBefore_, outBefore);
			StoreVec3(lastOutAfter_, a.ctrl->outVelocity);
			if (a_characterRB->character) {
				const auto pos = a_characterRB->character->motion.motionState.transform.translation;
				lastPos_[0].store(Hx(pos), std::memory_order_relaxed);
				lastPos_[1].store(Hy(pos), std::memory_order_relaxed);
				lastPos_[2].store(Hz(pos), std::memory_order_relaxed);
			}
			lastWriteMs_.store(now, std::memory_order_relaxed);
			writes_.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}

	void CharacterStepListener::ProcessActualPoints(const RE::hkpWorld* a_world,
		RE::hkpCharacterRigidBody* a_characterRB, const RE::hkpLinkedCollidable::CollisionEntry& a_entry,
		RE::hkpSimpleConstraintContactMgr* a_mgr, RE::hkArray<std::uint16_t>& a_contactPointIds)
	{
		const auto count = attachCount_.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < count && i < kMaxAttachments; ++i) {
			if (attachments_[i].body == a_characterRB && attachments_[i].prev && attachments_[i].prev != this) {
				attachments_[i].prev->ProcessActualPoints(a_world, a_characterRB, a_entry, a_mgr, a_contactPointIds);
			}
		}
	}

	void CharacterStepListener::UnweldContactPoints(RE::hkpCharacterRigidBody* a_characterRB,
		const RE::hkpLinkedCollidable::CollisionEntry& a_entry, RE::hkpSimpleConstraintContactMgr* a_mgr,
		const RE::hkArray<std::uint16_t>& a_contactPointIds)
	{
		const auto count = attachCount_.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < count && i < kMaxAttachments; ++i) {
			if (attachments_[i].body == a_characterRB && attachments_[i].prev && attachments_[i].prev != this) {
				attachments_[i].prev->UnweldContactPoints(a_characterRB, a_entry, a_mgr, a_contactPointIds);
			}
		}
	}

	void CharacterStepListener::ConsiderCollisionEntryForSlope(const RE::hkpWorld* a_world,
		RE::hkpCharacterRigidBody* a_characterRB, const RE::hkpLinkedCollidable::CollisionEntry& a_entry,
		RE::hkpSimpleConstraintContactMgr* a_mgr, RE::hkArray<std::uint16_t>& a_contactPointIds)
	{
		const auto count = attachCount_.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < count && i < kMaxAttachments; ++i) {
			if (attachments_[i].body == a_characterRB && attachments_[i].prev && attachments_[i].prev != this) {
				attachments_[i].prev->ConsiderCollisionEntryForSlope(a_world, a_characterRB, a_entry, a_mgr, a_contactPointIds);
			}
		}
	}

	void CharacterStepListener::ConsiderCollisionEntryForMassModification(const RE::hkpWorld* a_world,
		RE::hkpCharacterRigidBody* a_characterRB, const RE::hkpLinkedCollidable::CollisionEntry& a_entry,
		RE::hkpSimpleConstraintContactMgr* a_mgr, const RE::hkArray<std::uint16_t>& a_contactPointIds)
	{
		const auto count = attachCount_.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < count && i < kMaxAttachments; ++i) {
			if (attachments_[i].body == a_characterRB && attachments_[i].prev && attachments_[i].prev != this) {
				attachments_[i].prev->ConsiderCollisionEntryForMassModification(a_world, a_characterRB, a_entry, a_mgr, a_contactPointIds);
			}
		}
	}

	bool CharacterStepListener::Attach(RE::hkpCharacterRigidBody* a_body, RE::bhkCharacterController* a_ctrl)
	{
		if (!a_body || !a_ctrl) {
			return false;
		}
		auto count = attachCount_.load(std::memory_order_acquire);
		for (std::size_t i = 0; i < count && i < kMaxAttachments; ++i) {
			if (attachments_[i].body == a_body) {
				attachments_[i].ctrl = a_ctrl;
				return true;
			}
		}
		if (count >= kMaxAttachments) {
			return false;  // never evict: a silent mis-forward would corrupt a character
		}

		// Keep the saved listener alive for the process lifetime. The engine never
		// releases the old pointer when the field is replaced, and the controller
		// owns itself, so this is belt-and-braces against a freeing race.
		if (auto* prev = a_body->listener; prev && prev != this) {
			prev->AddReference();
		}
		// The listener must not be refcount-deleted by bhkCharacterRigidBody's
		// clearListener when the actor unloads; hold one reference forever.
		this->AddReference();

		attachments_[count] = Attachment{ a_body, a_body->listener, a_ctrl };
		// Publish the recorded attachment BEFORE installing the pointer, so a
		// callback can never see itself chained but untracked.
		attachCount_.store(count + 1, std::memory_order_release);
		a_body->listener = this;
		return true;
	}

	void CharacterStepListener::Arm(RE::hkpCharacterRigidBody* a_body, const float a_dir[3], float a_dv,
		std::uint32_t a_formId, std::uint64_t a_endMs)
	{
		if (!a_body) {
			return;
		}
		armedBody_ = a_body;
		formId_.store(a_formId, std::memory_order_relaxed);
		for (int i = 0; i < 3; ++i) {
			dir_[i] = a_dir[i];
		}
		dv_ = a_dv;
		endMs_ = a_endMs;
		armed_.store(true, std::memory_order_release);
	}

	void CharacterStepListener::Disarm()
	{
		armed_.store(false, std::memory_order_release);
		armedBody_ = nullptr;
	}

	CharacterStepListener::Snapshot CharacterStepListener::Read() const
	{
		Snapshot s;
		const auto count = attachCount_.load(std::memory_order_acquire);
		if (count > 0) {
			s.body = reinterpret_cast<std::uintptr_t>(attachments_[0].body);
			s.prev = reinterpret_cast<std::uintptr_t>(attachments_[0].prev);
			s.ctrl = reinterpret_cast<std::uintptr_t>(attachments_[0].ctrl);
		}
		s.armed = armed_.load(std::memory_order_acquire);
		for (int i = 0; i < 3; ++i) {
			s.dir[i] = dir_[i];
			s.lastAccel[i] = lastAccel_[i].load(std::memory_order_relaxed);
			s.lastRbVel[i] = lastRbVel_[i].load(std::memory_order_relaxed);
			s.lastOutBefore[i] = lastOutBefore_[i].load(std::memory_order_relaxed);
			s.lastOutAfter[i] = lastOutAfter_[i].load(std::memory_order_relaxed);
			s.lastPos[i] = lastPos_[i].load(std::memory_order_relaxed);
		}
		s.dv = dv_;
		s.endMs = endMs_;
		s.callbacks = callbacks_.load(std::memory_order_relaxed);
		s.forwards = forwards_.load(std::memory_order_relaxed);
		s.writes = writes_.load(std::memory_order_relaxed);
		s.lastWriteMs = lastWriteMs_.load(std::memory_order_relaxed);
		return s;
	}

	CharacterStepListener& GetCharacterStepListener()
	{
		static CharacterStepListener listener;
		return listener;
	}
}
