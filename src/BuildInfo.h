#pragma once

// One place for the version and build id, so the startup banner and the CI
// string checks cannot drift.
//
// PA_BUILD_ID is defined by xmake.lua at configure time from the environment
// (GITHUB_SHA in CI, PA_BUILD_ID for a developer), with a safe fallback when
// neither is set. It is a recognisability stamp for a run, not a security claim.

#define PA_VERSION "0.1.0"

#ifndef PA_BUILD_ID
#	define PA_BUILD_ID "unknown"
#endif
