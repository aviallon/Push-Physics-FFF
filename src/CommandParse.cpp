#include "CommandParse.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

namespace pa
{
	namespace
	{
		[[nodiscard]] bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
		{
			if (a_lhs.size() != a_rhs.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_lhs.size(); ++i) {
				char l = a_lhs[i];
				char r = a_rhs[i];
				if (l >= 'A' && l <= 'Z') {
					l = static_cast<char>(l + ('a' - 'A'));
				}
				if (r >= 'A' && r <= 'Z') {
					r = static_cast<char>(r + ('a' - 'A'));
				}
				if (l != r) {
					return false;
				}
			}
			return true;
		}

		struct Tokens
		{
			std::string_view t[4];
			std::size_t      count = 0;
		};

		// Split on ASCII whitespace. At most 4 tokens are needed by any command;
		// extra tokens are ignored (and the caller can detect that via `count`).
		[[nodiscard]] Tokens Split(std::string_view a_text)
		{
			Tokens out;
			std::size_t i = 0;
			while (i < a_text.size()) {
				while (i < a_text.size() && (a_text[i] == ' ' || a_text[i] == '\t')) {
					++i;
				}
				const auto start = i;
				while (i < a_text.size() && a_text[i] != ' ' && a_text[i] != '\t') {
					++i;
				}
				if (i > start && out.count < 4) {
					out.t[out.count++] = a_text.substr(start, i - start);
				}
			}
			return out;
		}

		[[nodiscard]] bool IsCommentOrBlank(std::string_view a_line)
		{
			return a_line.empty() || a_line.front() == '#';
		}
	}

	std::string_view Trim(std::string_view a_text)
	{
		std::size_t begin = 0;
		std::size_t end = a_text.size();
		const auto  space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
		while (begin < end && space(a_text[begin])) {
			++begin;
		}
		while (end > begin && space(a_text[end - 1])) {
			--end;
		}
		return a_text.substr(begin, end - begin);
	}

	bool ParseFormId(std::string_view a_text, std::uint32_t& a_out)
	{
		if (a_text.empty()) {
			return false;
		}
		std::string text(a_text);
		errno = 0;
		char* end = nullptr;
		// Form IDs are conventionally written in hex ("0x0001A2B3") but a decimal
		// value is accepted too. The base is chosen explicitly: strtoul(..., 0)
		// would read a leading-zero decimal as octal.
		const int base = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
		const unsigned long value = std::strtoul(text.c_str(), &end, base);
		if (end == text.c_str() || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFul) {
			return false;
		}
		a_out = static_cast<std::uint32_t>(value);
		return true;
	}

	bool ParseFloatArg(std::string_view a_text, float& a_out)
	{
		if (a_text.empty()) {
			return false;
		}
		std::string text(a_text);
		errno = 0;
		char*       end = nullptr;
		const float value = std::strtof(text.c_str(), &end);
		if (end == text.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
			return false;
		}
		a_out = value;
		return true;
	}

	bool ParseU32Arg(std::string_view a_text, std::uint32_t& a_out)
	{
		if (a_text.empty()) {
			return false;
		}
		std::string text(a_text);
		errno = 0;
		char*               end = nullptr;
		const unsigned long value = std::strtoul(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFul) {
			return false;
		}
		a_out = static_cast<std::uint32_t>(value);
		return true;
	}

	bool ParsePushMode(std::string_view a_text, PushMode& a_out)
	{
		if (IEquals(a_text, "ctrl")) {
			a_out = PushMode::kCtrl;
			return true;
		}
		if (IEquals(a_text, "rb")) {
			a_out = PushMode::kRb;
			return true;
		}
		if (IEquals(a_text, "both")) {
			a_out = PushMode::kBoth;
			return true;
		}
		return false;
	}

	const char* PushModeName(PushMode a_mode)
	{
		switch (a_mode) {
		case PushMode::kCtrl:
			return "ctrl";
		case PushMode::kRb:
			return "rb";
		case PushMode::kBoth:
			return "both";
		}
		return "both";
	}

	const char* PushMechanismName(int a_mask)
	{
		switch (a_mask & 0x3) {
		case 1:
			return "ctrl";
		case 2:
			return "rb";
		case 3:
			return "both";
		default:
			return "none";
		}
	}

	ParsedCommand ParseCommandLine(std::string_view a_line)
	{
		ParsedCommand out;
		out.line = Trim(a_line);
		if (IsCommentOrBlank(out.line)) {
			out.kind = CommandKind::kNone;
			return out;
		}

		const auto tokens = Split(out.line);
		if (tokens.count == 0) {
			out.kind = CommandKind::kNone;
			return out;
		}
		out.arg1 = tokens.count > 1 ? tokens.t[1] : std::string_view{};
		out.arg2 = tokens.count > 2 ? tokens.t[2] : std::string_view{};
		out.arg3 = tokens.count > 3 ? tokens.t[3] : std::string_view{};

		const auto word = tokens.t[0];
		if (IEquals(word, "help")) {
			out.kind = CommandKind::kHelp;
		} else if (IEquals(word, "status")) {
			out.kind = CommandKind::kStatus;
		} else if (IEquals(word, "registry")) {
			out.kind = CommandKind::kRegistry;
		} else if (IEquals(word, "bump")) {
			out.kind = CommandKind::kBump;
		} else if (IEquals(word, "vtables")) {
			out.kind = CommandKind::kVtables;
		} else if (IEquals(word, "actors")) {
			out.kind = CommandKind::kActors;
		} else if (IEquals(word, "unwatch")) {
			out.kind = CommandKind::kUnwatch;
		} else if (IEquals(word, "watch")) {
			out.kind = CommandKind::kWatch;
			if (out.arg1.empty()) {
				out.valid = false;
				out.error = "usage: watch <formID>";
			} else if (!ParseFormId(out.arg1, out.formId)) {
				out.valid = false;
				out.error = "watch: formID must be hex (0x...) or decimal";
			}
		} else if (IEquals(word, "trace")) {
			out.kind = CommandKind::kTrace;
			if (out.arg1.empty() || IEquals(out.arg1, "status")) {
				out.traceAction = TraceAction::kStatus;
			} else if (IEquals(out.arg1, "on")) {
				out.traceAction = TraceAction::kOn;
			} else if (IEquals(out.arg1, "off")) {
				out.traceAction = TraceAction::kOff;
			} else if (IEquals(out.arg1, "every")) {
				out.traceAction = TraceAction::kEvery;
				if (out.arg2.empty() || !ParseU32Arg(out.arg2, out.every) || out.every == 0) {
					out.valid = false;
					out.error = "usage: trace every <n> (n >= 1)";
				}
			} else {
				out.valid = false;
				out.error = "usage: trace on|off|status|every <n>";
			}
		} else if (IEquals(word, "set")) {
			out.kind = CommandKind::kSet;
			// A bare `set` lists the supported keys; one argument is a mistake.
			if (out.arg1.empty()) {
				// listing mode
			} else if (out.arg2.empty()) {
				out.valid = false;
				out.error = "usage: set <Section>:<Key> <value>";
			}
		} else if (IEquals(word, "push")) {
			out.kind = CommandKind::kPush;
			if (out.arg1.empty() || out.arg2.empty()) {
				out.valid = false;
				out.error = "usage: push <formID> <dv> [ctrl|rb|both]";
			} else if (!ParseFormId(out.arg1, out.formId)) {
				out.valid = false;
				out.error = "push: formID must be hex (0x...) or decimal";
			} else if (!ParseFloatArg(out.arg2, out.dv)) {
				out.valid = false;
				out.error = "push: dv must be a number (units/s)";
			} else if (!out.arg3.empty() && !ParsePushMode(out.arg3, out.mode)) {
				out.valid = false;
				out.error = "push: mode must be ctrl, rb or both";
			}
		} else if (IEquals(word, "pushhere")) {
			out.kind = CommandKind::kPushHere;
			out.dv = 120.0f;  // a visible nudge when the caller gives no magnitude
			// One or two optional arguments, either order: a number is the dv, a
			// mode name is the mechanism.
			const std::string_view args[2]{ out.arg1, out.arg2 };
			for (const auto arg : args) {
				if (arg.empty()) {
					continue;
				}
				float         dv = 0.0f;
				PushMode      mode = PushMode::kBoth;
				if (ParseFloatArg(arg, dv)) {
					out.dv = dv;
				} else if (ParsePushMode(arg, mode)) {
					out.mode = mode;
				} else {
					out.valid = false;
					out.error = "usage: pushhere [dv] [ctrl|rb|both]";
					break;
				}
			}
		} else {
			out.kind = CommandKind::kUnknown;
			out.valid = false;
			out.error = "unknown command (try 'help')";
		}
		return out;
	}
}
