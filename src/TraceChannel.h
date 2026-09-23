#pragma once

#include <cstdint>
#include <string>

namespace pa::TraceChannel
{
	// Main thread, once per frame (driven from ProxyRegistry::MainThreadTick).
	// Inert when trace is off; writes a row every `every`-th frame otherwise.
	void Tick();

	void               SetEnabled(bool a_enabled);
	[[nodiscard]] bool Enabled();

	void                   SetEvery(std::uint32_t a_every);
	[[nodiscard]] std::uint32_t Every();

	// The single watched actor's formID (0 = none). Recorded in every row.
	void                   SetWatch(std::uint32_t a_formId);
	[[nodiscard]] std::uint32_t Watch();

	[[nodiscard]] std::string StatusLine();
	[[nodiscard]] std::uint64_t RowsWritten();
	[[nodiscard]] std::uint64_t BytesWritten();
}
