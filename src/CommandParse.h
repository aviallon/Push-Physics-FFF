#pragma once

// Pure parser for the PushAside.cmd command channel. No Windows/RE dependency,
// so every accepted and rejected form is exercised by the off-game tests.
//
// One command per line; '#' starts a comment; blank lines are ignored. The
// parser never allocates and never throws: string_views point into the caller's
// line buffer, which must outlive the dispatch.

#include <cstdint>
#include <string_view>

namespace pa
{
	enum class CommandKind
	{
		kNone,      // blank / comment: silently ignored
		kHelp,
		kStatus,
		kRegistry,
		kBump,
		kVtables,
		kActors,
		kWatch,
		kUnwatch,
		kTrace,
		kSet,
		kPush,
		kPushHere,
		kUnknown,   // recognised as "something" but not a command
	};

	enum class PushMode : std::uint8_t
	{
		kCtrl = 1,
		kRb = 2,
		kBoth = 3,
	};

	enum class TraceAction : std::uint8_t
	{
		kOn,
		kOff,
		kStatus,
		kEvery,
	};

	struct ParsedCommand
	{
		CommandKind kind = CommandKind::kNone;
		bool        valid = true;   // false: malformed; `error` says why
		std::string_view error;
		std::string_view line;      // the trimmed original line
		std::string_view arg1;
		std::string_view arg2;
		std::string_view arg3;

		// Typed arguments, filled for the commands that take them.
		std::uint32_t formId = 0;
		float         dv = 0.0f;
		PushMode      mode = PushMode::kBoth;
		TraceAction   traceAction = TraceAction::kStatus;
		std::uint32_t every = 1;
	};

	// Strip leading/trailing ASCII whitespace and a trailing '\r' (a CRLF file
	// written on Windows).
	[[nodiscard]] std::string_view Trim(std::string_view a_text);

	[[nodiscard]] bool ParseFormId(std::string_view a_text, std::uint32_t& a_out);
	[[nodiscard]] bool ParseFloatArg(std::string_view a_text, float& a_out);
	[[nodiscard]] bool ParseU32Arg(std::string_view a_text, std::uint32_t& a_out);
	[[nodiscard]] bool ParsePushMode(std::string_view a_text, PushMode& a_out);
	[[nodiscard]] const char* PushModeName(PushMode a_mode);
	[[nodiscard]] const char* PushMechanismName(int a_mask);  // 0 none, 1 ctrl, 2 rb, 3 both

	// Parse one line. `line` in the result is the trimmed input.
	[[nodiscard]] ParsedCommand ParseCommandLine(std::string_view a_line);
}
