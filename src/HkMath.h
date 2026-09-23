#pragma once

// hkVector4 <-> Vec3 helpers for the plugin translation units. The maths lives
// in PhysicsMath.h; this header is only conversion and the handful of hkVector4
// operations the model needs.

#include "PhysicsMath.h"

#include <RE/H/hkVector4.h>

#include <immintrin.h>

namespace pa
{
	[[nodiscard]] inline float Hx(const RE::hkVector4& a_v) { return _mm_cvtss_f32(a_v.quad); }
	[[nodiscard]] inline float Hy(const RE::hkVector4& a_v) { return _mm_cvtss_f32(_mm_shuffle_ps(a_v.quad, a_v.quad, _MM_SHUFFLE(1, 1, 1, 1))); }
	[[nodiscard]] inline float Hz(const RE::hkVector4& a_v) { return _mm_cvtss_f32(_mm_shuffle_ps(a_v.quad, a_v.quad, _MM_SHUFFLE(2, 2, 2, 2))); }

	[[nodiscard]] inline RE::hkVector4 Hk(float a_x, float a_y, float a_z) { return RE::hkVector4(a_x, a_y, a_z, 0.0f); }

	[[nodiscard]] inline math::Vec3 ToVec3(const RE::hkVector4& a_v) { return { Hx(a_v), Hy(a_v), Hz(a_v) }; }
	[[nodiscard]] inline RE::hkVector4 ToHk(const math::Vec3& a_v) { return Hk(a_v.x, a_v.y, a_v.z); }
}
