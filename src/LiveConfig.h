#pragma once

// Narrowly scoped live-override mechanism for Config.
//
// Config is loaded once at plugin load and then read from BOTH threads: the main
// thread (registry refresh, trace) and the Havok physics callback
// (PushModel::OnCharacterContact, PushListener::ProcessConstraintsCallback, ...).
// Writing a plain field from the main thread while the physics thread reads it is
// a data race, so `set <Section>:<Key> <value>` does not touch the struct the
// callback reads directly. Instead the main thread publishes a whole Config
// snapshot behind a seqlock and every physics-thread reader takes a consistent
// copy with Snapshot().
//
// This is deliberately *not* a change to config loading: Load() still fills the
// one Config struct, and every non-callback reader keeps using Config::Get().
// The only additions are Publish()/Snapshot() and the key table the `set`
// command is built on.
//
// Nothing here touches Windows/RE, so ParseSet/Publish/Snapshot are exercised by
// the off-game tests.

#include "Config.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace pa::LiveConfig
{
	// Main thread: make a_cfg the current snapshot. Called once after
	// Config::Load() and again after every successful ParseSet.
	void Publish(const Config& a_cfg);

	// Any thread: a consistent copy of the current snapshot. Lock-free; retries
	// only while a publish is in flight (publishes are rare - one per `set`).
	[[nodiscard]] Config Snapshot();

	// Main thread: parse "<Section>:<Key>" and a value, apply it to a_cfg on
	// success. `a_out` receives a normalised "<Section>:<Key>=<value>" line on
	// success, or the reason on failure. The section is matched
	// case-insensitively; "General" (or an omitted section) is a wildcard that
	// matches the key in whichever section defines it.
	[[nodiscard]] bool ParseSet(std::string_view a_key, std::string_view a_value, Config& a_cfg, std::string& a_out);

	// One "<Section>:<Key> (<type>)" line per supported key.
	[[nodiscard]] std::string SupportedKeys();
}
