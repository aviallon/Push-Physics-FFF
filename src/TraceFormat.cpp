#include "TraceFormat.h"

#include <cstdio>

namespace pa
{
	namespace
	{
		[[nodiscard]] unsigned long long P(std::uintptr_t a_value)
		{
			return static_cast<unsigned long long>(a_value);
		}
	}

	const char* TraceHeader()
	{
		return "frame,ms,"
			   "player_proxy,player_x,player_y,player_z,player_vx,player_vy,player_vz,player_mass,"
			   "bump_charBody,bump_refr,bump_actor,bump_ctrl,bump_ctrl_vptr,bump_rb,"
			   "bump_rb_vx,bump_rb_vy,bump_rb_vz,bump_rb_x,bump_rb_y,bump_rb_z,"
			   "bump_rb_motion,bump_rb_mass,bump_rb_dynamic,"
			   "push_mode,push_dv,push_mech,push_changed,"
			   "push_ctrl_from_x,push_ctrl_from_y,push_ctrl_from_z,"
			   "push_ctrl_to_x,push_ctrl_to_y,push_ctrl_to_z,"
			   "push_rb_from_x,push_rb_from_y,push_rb_from_z,"
			   "push_rb_to_x,push_rb_to_y,push_rb_to_z,"
			   "watch_form,watch_x,watch_y,watch_z,"
			   "watch_ctrl_vx,watch_ctrl_vy,watch_ctrl_vz,"
			   "watch_rb_vx,watch_rb_vy,watch_rb_vz\n";
	}

	const char* TraceLegend()
	{
		return "# columns: 51 (frame..watch_rb_vz)\n"
			   "# push_mode: 0=none 1=ctrl 2=rb 3=both; push_mech: 1=ctrl 2=rb 3=both\n"
			   "# push_changed: 1 when the mechanism's post-write velocity differed from its before value\n"
			   "# bump_rb_motion: hkpMotion::MotionType 0=invalid 1=dynamic 2=sphereInertia 3=boxInertia "
			   "4=keyframed 5=fixed 6=thinBoxInertia 7=character; -1 = no rigid body\n"
			   "# bump_rb_dynamic: 1 dynamic, 0 keyframed/fixed, -1 unknown\n";
	}

	int FormatTraceRow(const TraceSample& s, char* a_out, std::size_t a_size)
	{
		return std::snprintf(a_out, a_size,
			"%llu,%.3f,"
			"%llX,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
			"%llX,%08X,%llX,%llX,%llX,%llX,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%d,"
			"%d,%.3f,%d,%d,"
			"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
			"%08X,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
			static_cast<unsigned long long>(s.frame), s.ms,
			P(s.playerProxy), static_cast<double>(s.playerPos[0]), static_cast<double>(s.playerPos[1]), static_cast<double>(s.playerPos[2]),
			static_cast<double>(s.playerVel[0]), static_cast<double>(s.playerVel[1]), static_cast<double>(s.playerVel[2]),
			static_cast<double>(s.playerMass),
			P(s.bumpCharBody), s.bumpRefr, P(s.bumpActor), P(s.bumpCtrl), P(s.bumpCtrlVptr), P(s.bumpRb),
			static_cast<double>(s.bumpRbVel[0]), static_cast<double>(s.bumpRbVel[1]), static_cast<double>(s.bumpRbVel[2]),
			static_cast<double>(s.bumpRbPos[0]), static_cast<double>(s.bumpRbPos[1]), static_cast<double>(s.bumpRbPos[2]),
			s.bumpRbMotion, static_cast<double>(s.bumpRbMass), s.bumpRbDynamic,
			s.pushMode, static_cast<double>(s.pushDv), s.pushMech, s.pushChanged,
			static_cast<double>(s.pushCtrlFrom[0]), static_cast<double>(s.pushCtrlFrom[1]), static_cast<double>(s.pushCtrlFrom[2]),
			static_cast<double>(s.pushCtrlTo[0]), static_cast<double>(s.pushCtrlTo[1]), static_cast<double>(s.pushCtrlTo[2]),
			static_cast<double>(s.pushRbFrom[0]), static_cast<double>(s.pushRbFrom[1]), static_cast<double>(s.pushRbFrom[2]),
			static_cast<double>(s.pushRbTo[0]), static_cast<double>(s.pushRbTo[1]), static_cast<double>(s.pushRbTo[2]),
			s.watchForm,
			static_cast<double>(s.watchPos[0]), static_cast<double>(s.watchPos[1]), static_cast<double>(s.watchPos[2]),
			static_cast<double>(s.watchCtrlVel[0]), static_cast<double>(s.watchCtrlVel[1]), static_cast<double>(s.watchCtrlVel[2]),
			static_cast<double>(s.watchRbVel[0]), static_cast<double>(s.watchRbVel[1]), static_cast<double>(s.watchRbVel[2]));
	}

	int FormatTraceComment(const char* a_text, char* a_out, std::size_t a_size)
	{
		return std::snprintf(a_out, a_size, "# %s\n", a_text != nullptr ? a_text : "");
	}
}
