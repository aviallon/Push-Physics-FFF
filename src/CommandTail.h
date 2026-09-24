#pragma once

// Pure tailer for the PushAside.cmd side channel. No filesystem and no game
// dependency, so every append / truncate / in-place-rewrite case is pinned by
// the off-game tests.
//
// Why not a byte offset: the command file is rewritten by hand and by the test
// harness, and a truncate-then-rewrite that ends up LARGER than the previous
// read offset leaves a plain `offset = size` tailer seeking into the middle of
// the new file. That silently skipped the new file's first command - the bug
// that made `watch 0x000A2C94` "succeed" in the response stream while `status`
// still said `watch=none`. This tailer finds the longest common prefix with what
// it has already consumed, so a rewrite is recognised as such (common prefix
// short) and every new complete line is read, while a plain append (common
// prefix == the old file) reads only the appended lines.

#include <cstddef>
#include <string>
#include <string_view>

namespace pa
{
	class CommandTail
	{
	public:
		static constexpr std::size_t npos = static_cast<std::size_t>(-1);

		// Everything currently in the file counts as already consumed. Called
		// once per session, so commands that were present before the plugin
		// started are not replayed. An absent file is Baseline("").
		void Baseline(std::string_view a_content)
		{
			consumed_.assign(a_content);
		}

		// Feed the whole current file content. Returns the byte offset of the
		// first new complete line, or npos when there is none. Only complete
		// lines (terminated by '\n') are returned, so a line still being written
		// waits for the next feed. The consumed prefix advances past the last
		// complete line returned, never past a partial one.
		[[nodiscard]] std::size_t Feed(std::string_view a_content)
		{
			std::size_t common = 0;
			const std::size_t limit = consumed_.size() < a_content.size() ? consumed_.size() : a_content.size();
			while (common < limit && consumed_[common] == a_content[common]) {
				++common;
			}

			const std::size_t lastNewline = a_content.rfind('\n');
			if (lastNewline == std::string_view::npos) {
				return npos;  // no complete line exists yet
			}

			std::size_t start = common;
			if (start > 0 && a_content[start - 1] != '\n') {
				// The common prefix ends mid-line: the rest of that line is a
				// rewrite fragment, so begin at the following line instead of
				// reporting a partial command.
				const std::size_t nl = a_content.find('\n', start);
				if (nl == std::string_view::npos) {
					return npos;
				}
				start = nl + 1;
			}

			if (start > lastNewline) {
				// Every complete line is already covered by the consumed prefix:
				// this was a truncation (or a rewrite of the same content). Just
				// re-anchor the consumed prefix to the current file.
				consumed_.assign(a_content.substr(0, lastNewline + 1));
				return npos;
			}

			consumed_.assign(a_content.substr(0, lastNewline + 1));
			return start;
		}

		[[nodiscard]] std::string_view Consumed() const { return consumed_; }

	private:
		// The prefix of the file that has already been handed to the parser. It
		// never includes a line without its terminating newline.
		std::string consumed_;
	};
}
