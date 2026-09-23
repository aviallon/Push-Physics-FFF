#!/usr/bin/env bash
# ingame-harness.sh - unattended in-game test harness for the PushAside SKSE plugin.
#
# The point of this script is to let an agent (or a human) cycle instrumented
# game runs without a human at the keyboard:
#
#   snapshot -> start -> load -> walk -> cmd ... -> collect -> stop
#
# or the whole composition at once:
#
#   tools/ingame-harness.sh run --walk 10 --cmd status --cmd vtables
#
# Everything it changes outside the plugin's own log/out/trace files is
# reversible, and every wait is bounded.  It never touches the deployed
# PushAside.dll or PushAside.ini: the caller manages those.
#
# Effects on user files (all undone by `stop`, or by `run`'s EXIT trap):
#
#   1. Saves.  `snapshot` tars the Proton-prefix Saves directory to a run-scoped
#      archive; `restore` mirrors that archive back with `rsync -a --delete`, so
#      autosaves created during a run do not survive.  If the run-scoped archive
#      is missing, `restore` falls back to the orchestrator's full backup
#      (~/pa-saves-backup-*) and says so.  It never deletes saves when it cannot
#      first prove it has a good archive.
#   2. The SKSE loader symlinks.  skse64_loader.exe in the game dir is a symlink
#      into the Amethyst mod dir.  Wine resolves it, so GetModuleFileName returns
#      the mod dir and the loader aborts ("Couldn't find SkyrimSE.exe").  `start`
#      replaces the two symlinks with real copies and records the original
#      targets; `stop` recreates the symlinks.
#
# Focus is not cosmetic here.  Skyrim does not step Havok while the window is
# unfocused, so an unfocused run silently produces a stalled simulation and a
# meaningless zero.  `start` activates the game window through KWin (kdotool)
# and refuses to continue if it cannot confirm the window is active.
#
# Hang safety.  A previous session hung with the plugin's counters frozen and the
# main thread spinning.  Every wait here has a timeout; `walk` fails loudly if
# `constraints=` does not advance, and a freeze longer than PA_FREEZE_SEC grabs
# a thread sample, collects the evidence, kills the game and reports.
#
# Requires: bash, tar, rsync, pgrep/pkill, /proc.  kdotool and dotool are taken
# from PATH if present, otherwise from `nix shell nixpkgs#...` (never installed
# system-wide).  gdb is used best-effort for the hang thread sample.

set -uo pipefail

HARNESS_VERSION="1.0.0"
SCRIPT_NAME="${0##*/}"

# ---------------------------------------------------------------------------
# Configuration (every value is overridable through the environment)
# ---------------------------------------------------------------------------
: "${HOME:?HOME must be set}"

GAME_DIR="${PA_GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/Skyrim Special Edition}"
STEAM_DIR="${PA_STEAM_DIR:-$HOME/.local/share/Steam}"
APPID="${PA_APPID:-489830}"
COMPAT_DATA="${PA_COMPAT_DATA:-$STEAM_DIR/steamapps/compatdata/$APPID}"
PREFIX="$COMPAT_DATA/pfx"
DOCS="$PREFIX/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition"
SKSE_DIR="$DOCS/SKSE"
SAVES_DIR="$DOCS/Saves"

LOG="$SKSE_DIR/PushAside.log"
CMD_FILE="$SKSE_DIR/PushAside.cmd"
OUT_FILE="$SKSE_DIR/PushAside.out"
TRACE_FILE="$SKSE_DIR/PushAside.trace"
LOADER_LOG="$SKSE_DIR/skse64_loader.log"

PLUGIN_DLL="$GAME_DIR/Data/SKSE/Plugins/PushAside.dll"
LOADER_EXE="$GAME_DIR/skse64_loader.exe"
SKSE_DLL="$GAME_DIR/skse64_1_7_104.dll"
SKSE_STEAM_LOADER_DLL="$GAME_DIR/skse64_steam_loader.dll"

PA_ROOT="${PA_ROOT:-$HOME/.cache/pushaside-harness}"
STATE_DIR="$PA_ROOT/state"
RUNS_DIR="$PA_ROOT/runs"

# Timeouts / thresholds (seconds unless stated).
PA_START_TIMEOUT="${PA_START_TIMEOUT:-300}"   # window appears
PA_MENU_TIMEOUT="${PA_MENU_TIMEOUT:-300}"     # plugin starts ticking
PA_LOAD_TIMEOUT="${PA_LOAD_TIMEOUT:-180}"     # player-proxy attach after Enter
PA_CMD_TIMEOUT="${PA_CMD_TIMEOUT:-45}"        # command response
PA_WALK_DEFAULT="${PA_WALK_DEFAULT:-10}"
PA_FREEZE_SEC="${PA_FREEZE_SEC:-8}"           # frozen constraints => hang
PA_QUIT_TIMEOUT="${PA_QUIT_TIMEOUT:-30}"
PA_GLOBAL_TIMEOUT="${PA_GLOBAL_TIMEOUT:-900}"

# Fallback full backup made by the orchestrator before this harness existed.
PA_FALLBACK_BACKUP="${PA_FALLBACK_BACKUP:-}"

DRY_RUN=0
RUN_ID=""
RUN_DIR=""
DEADLINE_EPOCH=0
RUN_STARTED_FILE=""

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
	readonly C_RED=$'\033[31m' C_GRN=$'\033[32m' C_YEL=$'\033[33m' C_BLU=$'\033[34m' C_OFF=$'\033[0m'
else
	readonly C_RED='' C_GRN='' C_YEL='' C_BLU='' C_OFF=''
fi

log()  { printf '%s[%s]%s %s\n' "$C_BLU" "$(date +%H:%M:%S)" "$C_OFF" "$*"; }
ok()   { printf '%s[ OK ]%s %s\n' "$C_GRN" "$C_OFF" "$*"; }
warn() { printf '%s[WARN]%s %s\n' "$C_YEL" "$C_OFF" "$*" >&2; }
err()  { printf '%s[FAIL]%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; }
die()  { err "$*"; exit 1; }

# Run a command, or just print it in dry-run mode.
mutate() {
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] %s\n' "$*"
		return 0
	fi
	"$@"
}

have() { command -v "$1" >/dev/null 2>&1; }

# kdotool / dotool from PATH, else from a nix expression (never a raw store path).
if have kdotool; then
	KX_CMD=(kdotool)
else
	KX_CMD=(nix shell nixpkgs#kdotool -c kdotool)
fi
if have dotool; then
	DT_CMD=(dotool)
else
	DT_CMD=(nix shell nixpkgs#dotool -c dotool)
fi
kx() { "${KX_CMD[@]}" "$@"; }
dt() { "${DT_CMD[@]}"; }  # reads actions from stdin

now_epoch() { date +%s; }
now_hms()   { date +%H:%M:%S; }

# ---------------------------------------------------------------------------
# Run / state plumbing
# ---------------------------------------------------------------------------
state_file() { printf '%s/%s' "$STATE_DIR" "$1"; }

ensure_dirs() {
	mkdir -p "$STATE_DIR" "$RUNS_DIR"
}

new_run_id() { date +%Y%m%d-%H%M%S; }

resolve_run_dir() {
	RUN_ID="${PA_RUN_ID:-$(cat "$(state_file run-id)" 2>/dev/null || true)}"
	if [ -z "$RUN_ID" ]; then
		RUN_ID="$(new_run_id)"
	fi
	RUN_DIR="$RUNS_DIR/$RUN_ID"
}

start_run() {
	ensure_dirs
	resolve_run_dir
	mkdir -p "$RUN_DIR"
	printf '%s\n' "$RUN_ID" >"$(state_file run-id)"
	RUN_STARTED_FILE="$RUN_DIR/started"
	: >"$RUN_STARTED_FILE"
	DEADLINE_EPOCH=$(( $(now_epoch) + PA_GLOBAL_TIMEOUT ))
}

remaining_seconds() {
	if [ "$DEADLINE_EPOCH" = 0 ]; then
		printf '%s' "$PA_GLOBAL_TIMEOUT"
		return
	fi
	local left=$(( DEADLINE_EPOCH - $(now_epoch) ))
	[ "$left" -lt 0 ] && left=0
	printf '%s' "$left"
}

deadline_reached() { [ "$(remaining_seconds)" = 0 ]; }

# ---------------------------------------------------------------------------
# Process / window helpers
# ---------------------------------------------------------------------------
game_pids() { pgrep -f 'SkyrimSE[.]exe' 2>/dev/null || true; }
loader_pids() { pgrep -f 'skse64_loader[.]exe' 2>/dev/null || true; }

game_running() { [ -n "$(game_pids)" ]; }

# Window id of the game, tried by class first (the exact Wine window class),
# then by title.  Empty when there is no game window.
window_is_game() {
	local id="$1" cls title
	[ -n "$id" ] || return 1
	cls="$(kx getwindowclassname "$id" 2>/dev/null)"
	case "$cls" in
		steam_app_489830|skyrimse.exe|SkyrimSE.exe) return 0 ;;
	esac
	title="$(kx getwindowname "$id" 2>/dev/null)"
	[ "$title" = "Skyrim Special Edition" ]
}

# Window id of the game.  The class search is authoritative; the title fallback
# is anchored, because an unanchored 'Skyrim Special Edition' also matches a
# Firefox tab whose title merely contains it (a real false positive this
# harness hit: it focused the browser and pressed Enter into the mod page).
game_window_id() {
	local id spec
	for spec in steam_app_489830 skyrimse.exe SkyrimSE.exe; do
		id="$(kx search --class "^${spec}$" 2>/dev/null | head -n1)"
		[ -n "$id" ] && { printf '%s' "$id"; return 0; }
	done
	id="$(kx search --title '^Skyrim Special Edition$' 2>/dev/null | head -n1)"
	[ -n "$id" ] && { printf '%s' "$id"; return 0; }
	return 1
}

# The cached game window, re-resolved when the cache is stale or points at
# something that is not the game.
resolve_game_window() {
	local id
	id="$(cat "$(state_file game-window)" 2>/dev/null || true)"
	if [ -n "$id" ] && window_is_game "$id"; then
		printf '%s' "$id"
		return 0
	fi
	id="$(game_window_id)" || return 1
	printf '%s\n' "$id" >"$(state_file game-window)"
	printf '%s' "$id"
}

active_window_id() { kx getactivewindow 2>/dev/null || true; }

window_is_focused() {
	local id="$1"
	[ -n "$id" ] || return 1
	[ "$(active_window_id)" = "$id" ]
}

# Activate the game window and prove it.  kdotool's windowactivate is honoured
# by KWin 6 (verified on this machine); the earlier attempt that "did not stick"
# was a KWin script that set activeWindow once before the window had settled.
ensure_focus() {
	local id="$1"
	for _ in 1 2 3 4 5; do
		kx windowraise "$id" >/dev/null 2>&1 || true
		kx windowactivate "$id" >/dev/null 2>&1 || true
		sleep 0.6
		if window_is_focused "$id"; then
			return 0
		fi
	done
	return 1
}

# --- /proc sampling --------------------------------------------------------
proc_cpu_ticks() {
	awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null
}

proc_cpu_percent() {
	local pid="$1" interval="${2:-1}" hz a b
	hz="$(getconf CLK_TCK 2>/dev/null || echo 100)"
	a="$(proc_cpu_ticks "$pid")" || return 1
	[ -n "$a" ] || return 1
	sleep "$interval"
	b="$(proc_cpu_ticks "$pid")" || return 1
	[ -n "$b" ] || return 1
	awk -v a="$a" -v b="$b" -v hz="$hz" -v iv="$interval" \
		'BEGIN { printf "%.1f", (b-a)/hz/iv*100 }'
}

thread_sample() {
	local pid="$1" out="$2"
	{
		echo "# thread sample $(date -Is) pid=$pid"
		echo "## ps -L"
		ps -L -o pid,tid,pcpu,stat,wchan:24,comm -p "$pid" 2>&1 || true
		echo
		echo "## gdb info threads (best effort; ptrace_scope=$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo n/a))"
		if have gdb; then
			timeout 25 gdb -p "$pid" -batch \
				-ex 'set pagination off' -ex 'info threads' 2>&1 || true
		else
			echo "(gdb not on PATH; skipped)"
		fi
	} >"$out" 2>&1
}

# ---------------------------------------------------------------------------
# Plugin log helpers
# ---------------------------------------------------------------------------
record_log_baseline() {
	local inode size
	if [ -f "$LOG" ]; then
		inode="$(stat -c%i "$LOG" 2>/dev/null || echo none)"
		size="$(stat -c%s "$LOG" 2>/dev/null || echo 0)"
	else
		inode="none"
		size=0
	fi
	printf '%s %s\n' "$inode" "$size" >"$(state_file log-baseline)"
}

# Print the part of the log that belongs to the current session.  A changed
# inode means SKSE rotated the log (it renames the old one to .prev-<id>), so the
# whole file is current.  A shrunk file is also treated as fresh.
log_since_baseline() {
	local base inode size cur_inode cur_size
	base="$(cat "$(state_file log-baseline)" 2>/dev/null || echo 'none 0')"
	inode="${base%% *}"
	size="${base##* }"
	[ -f "$LOG" ] || return 0
	cur_inode="$(stat -c%i "$LOG" 2>/dev/null || echo none)"
	cur_size="$(stat -c%s "$LOG" 2>/dev/null || echo 0)"
	if [ "$cur_inode" != "$inode" ] || [ "$cur_size" -lt "$size" ]; then
		cat "$LOG"
	else
		tail -c +$(( size + 1 )) "$LOG" 2>/dev/null
	fi
}

log_has_tick()   { log_since_baseline | grep -q 'listener stats:'; }
log_has_attach() { log_since_baseline | grep -q 'listener attached to player proxy'; }

# Newest `constraints=` value in the whole current log (monotonic per session).
latest_constraints() {
	local v
	v="$(tail -c 262144 "$LOG" 2>/dev/null | grep -o 'constraints=[0-9]*' | tail -n1 | cut -d= -f2)"
	printf '%s' "${v:-0}"
}

# Wait for a predicate function; returns 0 on success, 1 on timeout.
wait_for() {
	local pred="$1" timeout="$2" interval="${3:-1}"
	local waited=0 budget
	budget="$timeout"
	[ "$budget" -gt "$(remaining_seconds)" ] && budget="$(remaining_seconds)"
	while [ "$waited" -lt "$budget" ]; do
		if "$pred"; then
			return 0
		fi
		sleep "$interval"
		waited=$(( waited + interval ))
	done
	return 1
}

# ---------------------------------------------------------------------------
# snapshot / restore
# ---------------------------------------------------------------------------
snapshot_path() {
	if [ -n "${PA_SNAPSHOT:-}" ]; then
		printf '%s' "$PA_SNAPSHOT"
	elif [ -f "$(state_file last-snapshot)" ]; then
		cat "$(state_file last-snapshot)"
	else
		printf '%s/saves.tar' "$STATE_DIR"
	fi
}

verify_snapshot() {
	local snap="$1" n
	[ -f "$snap" ] || return 1
	tar -tf "$snap" >/dev/null 2>&1 || return 1
	n="$(tar -tf "$snap" 2>/dev/null | grep -c '\.ess$' || true)"
	[ "${n:-0}" -gt 0 ]
}

cmd_snapshot() {
	ensure_dirs
	resolve_run_dir
	local snap
	snap="${PA_SNAPSHOT:-$STATE_DIR/saves.tar}"
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] tar -C %q -cf %q Saves\n' "$DOCS" "$snap"
		return 0
	fi
	if [ -f "$snap" ] && verify_snapshot "$snap" && [ "${PA_FORCE_SNAPSHOT:-0}" != 1 ]; then
		ok "snapshot already present and valid: $snap ($(tar -tf "$snap" | wc -l) entries)"
		printf '%s\n' "$snap" >"$(state_file last-snapshot)"
		return 0
	fi
	[ -d "$SAVES_DIR" ] || die "saves dir not found: $SAVES_DIR"
	local n
	n="$(find "$SAVES_DIR" -maxdepth 1 -type f | wc -l)"
	log "snapshot: $SAVES_DIR ($n files) -> $snap"
	mkdir -p "$(dirname "$snap")"
	tar -C "$DOCS" -cf "$snap" Saves
	verify_snapshot "$snap" || die "snapshot failed verification: $snap"
	printf '%s\n' "$snap" >"$(state_file last-snapshot)"
	ok "snapshot: $(tar -tf "$snap" 2>/dev/null | wc -l) entries, $(du -h "$snap" | cut -f1)"
}

fallback_backup() {
	if [ -n "$PA_FALLBACK_BACKUP" ]; then
		printf '%s' "$PA_FALLBACK_BACKUP"
		return 0
	fi
	# Newest ~/pa-saves-backup-* directory.
	local d
	d="$(find "$HOME" -maxdepth 1 -type d -name 'pa-saves-backup-*' -printf '%T@ %p\n' 2>/dev/null \
		| sort -rn | head -n1 | cut -d' ' -f2-)"
	printf '%s' "$d"
}

cmd_restore() {
	ensure_dirs
	local snap src tmp
	snap="$(snapshot_path)"
	if verify_snapshot "$snap"; then
		log "restore: mirroring $snap onto $SAVES_DIR"
		tmp="$(mktemp -d "$PA_ROOT/restore.XXXXXX")"
		tar -C "$tmp" -xf "$snap" || { rm -rf "$tmp"; die "cannot extract $snap"; }
		src="$tmp/Saves"
		[ -d "$src" ] || src="$tmp"
		mutate rsync -a --delete "$src/" "$SAVES_DIR/"
		rm -rf "$tmp"
		if [ "$DRY_RUN" = 0 ]; then
			ok "restore: saves mirrored from snapshot ($(find "$SAVES_DIR" -maxdepth 1 -type f | wc -l) files)"
		fi
		return 0
	fi

	local fb
	fb="$(fallback_backup)"
	if [ -n "$fb" ] && [ -d "$fb" ]; then
		warn "run snapshot missing/corrupt ($snap); falling back to $fb"
		mutate rsync -a --delete "$fb/" "$SAVES_DIR/"
		[ "$DRY_RUN" = 0 ] && ok "restore: saves mirrored from fallback $fb"
		return 0
	fi
	err "no usable snapshot ($snap) and no fallback backup; refusing to touch $SAVES_DIR"
	return 1
}

# ---------------------------------------------------------------------------
# SKSE loader symlink trap
# ---------------------------------------------------------------------------
links_state() { state_file loader-links.tsv; }

fix_loader_links() {
	local p real
	ensure_dirs
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] replace the SKSE loader symlinks with real copies (recorded for restore)\n'
		return 0
	fi
	[ -f "$(links_state)" ] && return 0
	: >"$(links_state)"
	for p in "$LOADER_EXE" "$SKSE_DLL" "$SKSE_STEAM_LOADER_DLL"; do
		if [ -L "$p" ]; then
			real="$(readlink -f "$p")"
			printf '%s\t%s\n' "$p" "$real" >>"$(links_state)"
			log "loader trap: replacing symlink $p -> $real with a real copy"
			mutate cp --remove-destination "$real" "$p"
		elif [ -e "$p" ]; then
			printf '%s\t-\n' "$p" >>"$(links_state)"
		fi
	done
}

restore_loader_links() {
	local st p target
	st="$(links_state)"
	[ -f "$st" ] || return 0
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] recreate the SKSE loader symlinks recorded in %s\n' "$st"
		return 0
	fi
	while IFS=$'\t' read -r p target; do
		[ -n "$p" ] || continue
		if [ "$target" = "-" ]; then
			continue
		fi
		mutate ln -sfn "$target" "$p"
	done <"$st"
	if [ "$DRY_RUN" = 0 ]; then
		rm -f "$st"
		ok "loader symlinks restored"
	fi
}

# ---------------------------------------------------------------------------
# start
# ---------------------------------------------------------------------------
# The Steam launch options for this appid.  Amethyst's deploy does not put SKSE
# there (they are a gamescope/HDR wrapper around %command%), so launching through
# Steam would run SkyrimSE.exe *without* SKSE.  Read them to decide, rather than
# guessing.
steam_launch_options() {
	local f
	for f in "$STEAM_DIR"/userdata/*/config/localconfig.vdf; do
		[ -f "$f" ] || continue
		grep -A60 '"489830"' "$f" 2>/dev/null | grep -m1 'LaunchOptions'
	done
}

launch_game() {
	mkdir -p "$RUN_DIR"
	local desc
	local -a cmd
	if [ -n "${PA_LAUNCH_CMD:-}" ]; then
		# Caller-supplied command line (word-split on purpose).
		# shellcheck disable=SC2206
		cmd=($PA_LAUNCH_CMD)
		desc="$PA_LAUNCH_CMD"
	elif steam_launch_options | grep -qi 'skse'; then
		cmd=(steam -applaunch "$APPID")
		desc="steam -applaunch $APPID"
	else
		cmd=(protontricks-launch --appid "$APPID" "$LOADER_EXE")
		desc="protontricks-launch --appid $APPID $LOADER_EXE"
	fi
	log "launching ($desc) cwd=$GAME_DIR"
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] (cd %q && setsid nohup %s >%q 2>&1 &)\n' \
			"$GAME_DIR" "$desc" "$RUN_DIR/launch.log"
		return 0
	fi
	(
		cd "$GAME_DIR" || exit 1
		setsid nohup "${cmd[@]}" >"$RUN_DIR/launch.log" 2>&1 </dev/null &
		echo $! >"$RUN_DIR/launcher.pid"
	)
	printf '%s\n' "$desc" >"$(state_file launch-method)"
}

wait_game_process() {
	local timeout="$1" waited=0 budget
	budget="$timeout"
	[ "$budget" -gt "$(remaining_seconds)" ] && budget="$(remaining_seconds)"
	while [ "$waited" -lt "$budget" ]; do
		if game_running; then
			return 0
		fi
		# Fail early only when the launcher is gone AND its log already shows an
		# error: a wrapper that has merely returned while wine starts must not be
		# mistaken for a failure.
		if [ -f "$RUN_DIR/launcher.pid" ] && [ "$waited" -gt 15 ] \
			&& ! kill -0 "$(cat "$RUN_DIR/launcher.pid")" 2>/dev/null \
			&& grep -qi 'error\|couldn.t find' "$RUN_DIR/launch.log" 2>/dev/null; then
			return 1
		fi
		sleep 2
		waited=$(( waited + 2 ))
	done
	return 1
}

cmd_start() {
	ensure_dirs
	resolve_run_dir
	mkdir -p "$RUN_DIR"
	[ -f "$RUN_STARTED_FILE" ] || RUN_STARTED_FILE="$RUN_DIR/started"
	: >"$RUN_STARTED_FILE"
	if game_running; then
		die "SkyrimSE.exe is already running (pids: $(game_pids | tr '\n' ' ')); run '$SCRIPT_NAME stop' first"
	fi
	record_log_baseline
	fix_loader_links
	launch_game

	if [ "$DRY_RUN" = 1 ]; then
		ok "dry-run: would wait up to ${PA_START_TIMEOUT}s for the game window, then focus it"
		return 0
	fi

	log "waiting up to ${PA_START_TIMEOUT}s for the game process ..."
	if ! wait_game_process "$PA_START_TIMEOUT"; then
		err "the game process never appeared"
		[ -f "$RUN_DIR/launch.log" ] && { echo "--- launch.log ---"; tail -n 30 "$RUN_DIR/launch.log"; }
		[ -f "$LOADER_LOG" ] && { echo "--- skse64_loader.log ---"; tail -n 20 "$LOADER_LOG"; }
		[ -f "$LOG" ] && { echo "--- PushAside.log ---"; tail -n 20 "$LOG"; }
		return 1
	fi
	ok "game process: $(game_pids | tr '\n' ' ')"

	log "waiting up to ${PA_START_TIMEOUT}s for the game window ..."
	local id=""
	local waited=0 budget="$PA_START_TIMEOUT"
	[ "$budget" -gt "$(remaining_seconds)" ] && budget="$(remaining_seconds)"
	while [ "$waited" -lt "$budget" ]; do
		id="$(game_window_id)" && break
		sleep 2
		waited=$(( waited + 2 ))
	done
	if [ -z "$id" ]; then
		err "no game window appeared within ${PA_START_TIMEOUT}s"
		return 1
	fi
	printf '%s\n' "$id" >"$(state_file game-window)"
	ok "game window: $id ($(kx getwindowname "$id" 2>/dev/null))"

	log "focusing the game window (physics does not step while unfocused) ..."
	if ! ensure_focus "$id"; then
		err "could not focus the game window $id; an unfocused run is meaningless"
		err "active window is: $(active_window_id) ($(kx getwindowname "$(active_window_id)" 2>/dev/null))"
		return 1
	fi
	ok "game window is focused"
}

# ---------------------------------------------------------------------------
# load
# ---------------------------------------------------------------------------
send_key() {
	local chord="$1"
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] dotool key %s\n' "$chord"
		return 0
	fi
	printf 'key %s\n' "$chord" | dt
}

cmd_load() {
	resolve_run_dir
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] wait for plugin tick, focus window, press Enter until the attach line appears\n'
		return 0
	fi
	[ -f "$LOG" ] || log "note: $LOG does not exist yet"

	log "waiting up to ${PA_MENU_TIMEOUT}s for the plugin to tick ..."
	if ! wait_for log_has_tick "$PA_MENU_TIMEOUT" 2; then
		err "the plugin never logged a 'listener stats:' line; the game may not have reached the main menu"
		[ -f "$LOG" ] && tail -n 20 "$LOG"
		return 1
	fi
	ok "plugin is ticking"

	if log_has_attach; then
		ok "player proxy already attached (this session is already in a save)"
		return 0
	fi

	local id attempt
	id="$(resolve_game_window || true)"
	if [ -n "$id" ]; then
		ensure_focus "$id" || warn "could not re-confirm focus before loading"
	fi

	# Main menu: 'Continue' is the default selection; Enter loads the most recent
	# save.  Re-send a few times in case the menu was not up on the first try.
	for attempt in 1 2 3 4 5 6; do
		log "load attempt $attempt: pressing Enter (Continue)"
		send_key enter
		if wait_for log_has_attach 25 1; then
			ok "player proxy attached: $(log_since_baseline | grep 'listener attached to player proxy' | tail -n1)"
			log_since_baseline | grep 'listener attached to player proxy' | tail -n1 >"$RUN_DIR/attach.txt"
			return 0
		fi
		[ "$(remaining_seconds)" = 0 ] && break
	done
	err "no player-proxy attach after 6 Enter attempts (${PA_LOAD_TIMEOUT}s budget)"
	[ -f "$LOG" ] && tail -n 25 "$LOG"
	return 1
}

# ---------------------------------------------------------------------------
# walk
# ---------------------------------------------------------------------------
cmd_walk() {
	local seconds="${1:-$PA_WALK_DEFAULT}" look="${2:-0}"
	resolve_run_dir
	if [ "$DRY_RUN" = 1 ]; then
		if [ "$look" = 1 ]; then
			printf '[dry-run] hold forward for %ss with mouse-look, asserting constraints= advances\n' "$seconds"
		else
			printf '[dry-run] hold forward for %ss, asserting constraints= advances\n' "$seconds"
		fi
		return 0
	fi

	local id
	id="$(resolve_game_window || true)"
	[ -n "$id" ] || die "no game window recorded; run '$SCRIPT_NAME start' first"
	if ! ensure_focus "$id"; then
		die "game window is not focused; refusing to walk (the simulation would not step)"
	fi

	local c_start c_now c_last last_advance now
	c_start="$(latest_constraints)"
	c_last="$c_start"
	last_advance="$(now_epoch)"
	log "walk: holding forward for ${seconds}s (constraints start=$c_start)"

	# One dotool process holds the key, so there is no per-tick device
	# re-registration; the optional camera nudge is streamed in between.
	{
		echo 'keydown w'
		if [ "$look" = 1 ]; then
			for _ in 1 2 3 4; do
				sleep 1
				echo 'mousemove 160 0'
			done
		fi
		sleep "$seconds"
		echo 'keyup w'
	} | dt >/dev/null 2>&1 &
	local holder=$!

	while kill -0 "$holder" 2>/dev/null; do
		sleep 1
		c_now="$(latest_constraints)"
		if [ "$c_now" -gt "$c_last" ]; then
			c_last="$c_now"
			last_advance="$(now_epoch)"
		fi
		now="$(now_epoch)"
		if [ $(( now - last_advance )) -ge "$PA_FREEZE_SEC" ]; then
			err "HANG: constraints frozen at $c_last for $(( now - last_advance ))s (start=$c_start)"
			local pid cpu sample
			pid="$(game_pids | head -n1)"
			if [ -n "$pid" ]; then
				cpu="$(proc_cpu_percent "$pid" 1 || echo '?')"
				err "game pid $pid cpu=${cpu}% (a spinning main thread looks like ~100%)"
				sample="$RUN_DIR/hang-$(date +%H%M%S).txt"
				thread_sample "$pid" "$sample"
				err "thread sample: $sample"
			fi
			kill "$holder" 2>/dev/null || true
			printf 'keyup w\n' | dt >/dev/null 2>&1 || true
			printf 'HANG constraints=%s frozen=%ss cpu=%s\n' "$c_last" "$(( now - last_advance ))" "${cpu:-?}" >"$RUN_DIR/walk-result"
			collect_evidence "$RUN_DIR/evidence-hang" >/dev/null || true
			return 2
		fi
		if [ "$(remaining_seconds)" = 0 ]; then
			warn "global deadline reached during walk"
			break
		fi
	done

	wait "$holder" 2>/dev/null || true
	printf 'keyup w\n' | dt >/dev/null 2>&1 || true
	sleep 2
	local c_end
	c_end="$(latest_constraints)"
	printf 'constraints start=%s end=%s\n' "$c_start" "$c_end" >"$RUN_DIR/walk-result"
	if [ "$c_end" -le "$c_start" ]; then
		err "walk FAILED: constraints did not advance ($c_start -> $c_end); the simulation is not stepping"
		err "an unfocused or paused game is the usual cause"
		return 1
	fi
	ok "walk: constraints advanced $c_start -> $c_end (+$(( c_end - c_start )))"
	return 0
}

# ---------------------------------------------------------------------------
# cmd
# ---------------------------------------------------------------------------
out_size() { stat -c%s "$OUT_FILE" 2>/dev/null || echo 0; }

out_since() {
	local off="$1"
	[ -f "$OUT_FILE" ] || return 0
	tail -c +$(( off + 1 )) "$OUT_FILE" 2>/dev/null
}

cmd_cmd() {
	[ "$#" -gt 0 ] || die "cmd: needs at least one command line"
	resolve_run_dir
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] append to %s and wait for responses: %s\n' "$CMD_FILE" "$*"
		return 0
	fi
	# The plugin baselines the command file on first sight, so it must already be
	# ticking before we append or the new lines would be swallowed.
	if ! wait_for log_has_tick "$PA_MENU_TIMEOUT" 2; then
		die "plugin is not ticking; cannot send commands"
	fi

	local off line
	off="$(out_size)"
	printf '%s\n' "$*" >>"$CMD_FILE"
	log "cmd: appended '$*' (out offset $off)"

	local waited=0 budget="$PA_CMD_TIMEOUT"
	[ "$budget" -gt "$(remaining_seconds)" ] && budget="$(remaining_seconds)"
	while [ "$waited" -lt "$budget" ]; do
		local chunk
		chunk="$(out_since "$off")"
		local all=1
		for line in "$@"; do
			if ! printf '%s' "$chunk" | grep -qF "] $line"; then
				all=0
				break
			fi
		done
		if [ "$all" = 1 ]; then
			sleep 1  # grace for the result body
			out_since "$off" | sed 's/^/    /'
			ok "cmd: response received"
			return 0
		fi
		sleep 1
		waited=$(( waited + 1 ))
	done
	err "cmd: timed out after ${budget}s waiting for a response to: $*"
	out_since "$off" | tail -n 40 | sed 's/^/    /'
	return 1
}

# ---------------------------------------------------------------------------
# collect
# ---------------------------------------------------------------------------
collect_evidence() {
	local outdir="$1"
	[ -n "$outdir" ] || outdir="$RUN_DIR/evidence"
	mkdir -p "$outdir"
	local f
	for f in "$LOG" "$OUT_FILE" "$TRACE_FILE" "$CMD_FILE" "$LOADER_LOG"; do
		[ -f "$f" ] && cp -f "$f" "$outdir/" 2>/dev/null || true
	done
	[ -f "$RUN_DIR/launch.log" ] && cp -f "$RUN_DIR/launch.log" "$outdir/" 2>/dev/null || true
	# Crash logs created during this run.
	if [ -n "$RUN_STARTED_FILE" ] && [ -f "$RUN_STARTED_FILE" ]; then
		while IFS= read -r f; do
			cp -f "$f" "$outdir/" 2>/dev/null || true
		done < <(find "$SKSE_DIR" -maxdepth 1 -name 'crash-*.log' -newer "$RUN_STARTED_FILE" 2>/dev/null)
	fi
	[ -f "$SKSE_DIR/CrashLogger.log" ] && cp -f "$SKSE_DIR/CrashLogger.log" "$outdir/" 2>/dev/null || true
	printf '%s\n' "$outdir"
}

write_manifest() {
	local dest="$1"
	{
		echo "harness_version: $HARNESS_VERSION"
		echo "collected: $(date -Is)"
		echo "run_id: $RUN_ID"
		echo "launch_method: $(cat "$(state_file launch-method)" 2>/dev/null || echo unknown)"
		echo "plugin_dll: $PLUGIN_DLL"
		if [ -f "$PLUGIN_DLL" ]; then
			echo "plugin_dll_sha256: $(sha256sum "$PLUGIN_DLL" | awk '{print $1}')"
		fi
		echo "game_window: $(cat "$(state_file game-window)" 2>/dev/null || echo none)"
		echo "attach: $(cat "$RUN_DIR/attach.txt" 2>/dev/null || echo 'not observed')"
		echo "walk: $(cat "$RUN_DIR/walk-result" 2>/dev/null | tr '\n' ';' || echo 'not run')"
		echo "constraints_latest: $(latest_constraints)"
		echo "game_running_at_collect: $(game_running && echo yes || echo no)"
		echo "loadavg: $(cat /proc/loadavg 2>/dev/null)"
	} >"$dest/MANIFEST.txt"
}

cmd_collect() {
	local outdir="${1:-$RUNS_DIR/collected}"
	resolve_run_dir
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] copy PushAside.{log,out,trace}, crash logs and the dll sha256 into %s\n' "$outdir"
		return 0
	fi
	mkdir -p "$outdir"
	local dest
	dest="$outdir/pa-${RUN_ID}-$(date +%H%M%S)"
	mkdir -p "$dest"
	collect_evidence "$dest" >/dev/null
	write_manifest "$dest"
	ok "collected evidence -> $dest"
	ls -la "$dest"
	printf '%s\n' "$dest" >"$(state_file last-collect)"
	printf '%s\n' "$dest"
}

# ---------------------------------------------------------------------------
# stop
# ---------------------------------------------------------------------------
kill_game() {
	local pids
	pids="$(game_pids)"
	if [ -z "$pids" ]; then
		return 0
	fi
	# shellcheck disable=SC2086
	log "killing game pids: $(printf '%s ' $pids)"
	# shellcheck disable=SC2086
	mutate kill $pids 2>/dev/null || true
	local waited=0
	while [ "$waited" -lt 10 ] && game_running; do
		sleep 1
		waited=$(( waited + 1 ))
	done
	pids="$(game_pids)"
	if [ -n "$pids" ]; then
		warn "game did not exit on SIGTERM; sending SIGKILL"
		# shellcheck disable=SC2086
		mutate kill -9 $pids 2>/dev/null || true
		sleep 2
	fi
	local lp
	lp="$(loader_pids)"
	# shellcheck disable=SC2086
	[ -n "$lp" ] && mutate kill $lp 2>/dev/null || true
}

cmd_stop() {
	resolve_run_dir
	local no_restore="${1:-0}"
	if [ "$DRY_RUN" = 1 ]; then
		printf '[dry-run] close/quit the game, restore loader symlinks and the save snapshot\n'
		return 0
	fi

	if game_running; then
		local id
		id="$(resolve_game_window || true)"
		if [ -n "$id" ]; then
			ensure_focus "$id" >/dev/null 2>&1 || true
			log "stop: asking the window to close (clean quit)"
			kx windowclose "$id" >/dev/null 2>&1 || true
			local waited=0 budget="$PA_QUIT_TIMEOUT"
			[ "$budget" -gt "$(remaining_seconds)" ] && budget="$(remaining_seconds)"
			while [ "$waited" -lt "$budget" ] && game_running; do
				sleep 1
				waited=$(( waited + 1 ))
			done
		fi
	fi
	if game_running; then
		log "stop: clean quit did not take; sending Alt+F4 + Enter"
		printf 'key alt+F4\n' | dt >/dev/null 2>&1 || true
		sleep 2
		printf 'key enter\n' | dt >/dev/null 2>&1 || true
		local waited=0
		while [ "$waited" -lt 10 ] && game_running; do
			sleep 1
			waited=$(( waited + 1 ))
		done
	fi
	kill_game

	restore_loader_links

	local restore_rc=0
	if [ "$no_restore" != 1 ]; then
		cmd_restore || restore_rc=1
	fi

	if game_running; then
		err "SkyrimSE.exe still present: $(game_pids | tr '\n' ' ')"
		return 1
	fi
	if [ "$restore_rc" != 0 ]; then
		err "stop: the save restore failed; the run snapshot is still under $STATE_DIR"
		return 1
	fi
	ok "stop: no SkyrimSE.exe remains"
	log "load average: $(cat /proc/loadavg)"
}

# ---------------------------------------------------------------------------
# run (composition)
# ---------------------------------------------------------------------------
RUN_FAILED=0
RUN_COLLECTED=0
RUN_STOPPED=0
COLLECT_OUTDIR=""
CMD_LINES=()

run_cleanup() {
	local rc=$?
	if [ "$RUN_STOPPED" = 0 ]; then
		warn "run: cleanup after failure/exit"
		kill_game 2>/dev/null || true
		restore_loader_links 2>/dev/null || true
		if [ "$RUN_COLLECTED" = 0 ]; then
			collect_evidence "$RUN_DIR/evidence-final" >/dev/null 2>&1 || true
		fi
		cmd_restore 2>/dev/null || true
	fi
	exit "$rc"
}

cmd_run() {
	local walk_secs="$PA_WALK_DEFAULT" look=0 outdir="" cmds_file="" no_restore=0
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--walk) walk_secs="${2:?}"; shift 2 ;;
			--look) look=1; shift ;;
			--outdir) outdir="${2:?}"; shift 2 ;;
			--cmds) cmds_file="${2:?}"; shift 2 ;;
			--cmd) CMD_LINES+=("${2:?}"); shift 2 ;;
			--timeout) PA_GLOBAL_TIMEOUT="${2:?}"; shift 2 ;;
			--no-restore) no_restore=1; shift ;;
			*) die "run: unknown option: $1" ;;
		esac
	done
	if [ -n "$cmds_file" ]; then
		[ -f "$cmds_file" ] || die "run: command file not found: $cmds_file"
		while IFS= read -r line; do
			line="${line%%$'\r'}"
			case "$line" in ''|'#'*) continue ;; esac
			CMD_LINES+=("$line")
		done <"$cmds_file"
	fi
	[ "${#CMD_LINES[@]}" -gt 0 ] || CMD_LINES=("status")

	start_run
	COLLECT_OUTDIR="${outdir:-$RUNS_DIR/collected}"
	trap run_cleanup EXIT
	trap 'RUN_FAILED=1; exit 130' INT TERM

	local rc
	log "run $RUN_ID: walk=${walk_secs}s look=$look timeout=${PA_GLOBAL_TIMEOUT}s"
	log "run: commands: ${CMD_LINES[*]}"

	log "=== stage: snapshot ==="
	PA_SNAPSHOT="$RUN_DIR/saves.tar" cmd_snapshot || RUN_FAILED=1

	log "=== stage: start ==="
	if ! cmd_start; then RUN_FAILED=1; fi

	if [ "$RUN_FAILED" = 0 ]; then
		log "=== stage: load ==="
		cmd_load || RUN_FAILED=1
	fi

	if [ "$RUN_FAILED" = 0 ]; then
		log "=== stage: walk ==="
		cmd_walk "$walk_secs" "$look" || RUN_FAILED=1
	fi

	if game_running; then
		log "=== stage: cmd ==="
		local c
		for c in "${CMD_LINES[@]}"; do
			cmd_cmd "$c" || RUN_FAILED=1
		done
	fi

	log "=== stage: collect ==="
	if cmd_collect "$COLLECT_OUTDIR" >/dev/null; then
		RUN_COLLECTED=1
	fi

	log "=== stage: stop ==="
	if [ "$no_restore" = 1 ]; then
		cmd_stop 1 && RUN_STOPPED=1 || RUN_FAILED=1
	else
		cmd_stop 0 && RUN_STOPPED=1 || RUN_FAILED=1
	fi

	if [ "$RUN_FAILED" = 0 ]; then
		ok "run $RUN_ID completed"
	else
		err "run $RUN_ID completed with failures (see collected evidence)"
	fi
	return "$RUN_FAILED"
}

# ---------------------------------------------------------------------------
# doctor / dry-run plan
# ---------------------------------------------------------------------------
cmd_doctor() {
	ensure_dirs
	resolve_run_dir
	echo "harness:      $HARNESS_VERSION"
	echo "game dir:     $GAME_DIR $([ -d "$GAME_DIR" ] && echo OK || echo MISSING)"
	echo "prefix:       $PREFIX $([ -d "$PREFIX" ] && echo OK || echo MISSING)"
	echo "saves:        $SAVES_DIR ($(find "$SAVES_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l) files, $(du -sh "$SAVES_DIR" 2>/dev/null | cut -f1))"
	echo "plugin dll:   $PLUGIN_DLL $([ -e "$PLUGIN_DLL" ] && echo OK || echo MISSING)"
	[ -e "$PLUGIN_DLL" ] && echo "plugin sha:   $(sha256sum "$PLUGIN_DLL" | awk '{print $1}')"
	local loader_desc
	if [ -L "$LOADER_EXE" ]; then loader_desc="(symlink -> $(readlink "$LOADER_EXE"))"; else loader_desc="(regular file)"; fi
	echo "loader exe:   $LOADER_EXE $loader_desc"
	echo "kdotool:      ${KX_CMD[*]}"
	echo "dotool:       ${DT_CMD[*]}"
	echo "protontricks: $(command -v protontricks-launch 2>/dev/null || echo MISSING)"
	echo "gdb:          $(command -v gdb 2>/dev/null || echo 'missing (thread sample falls back to ps -L)')"
	echo "game running: $(game_running && echo "yes ($(game_pids | tr '\n' ' '))" || echo no)"
	echo "loadavg:      $(cat /proc/loadavg)"
	echo "run root:     $PA_ROOT"
	echo "snapshot:     $(snapshot_path) $(verify_snapshot "$(snapshot_path)" && echo valid || echo 'absent/invalid')"
	echo "fallback:     $(fallback_backup)"
}

usage() {
	cat <<EOF
$SCRIPT_NAME $HARNESS_VERSION - unattended in-game harness for PushAside

Usage: $SCRIPT_NAME <command> [options]

  snapshot                 tar the Saves dir to a run-scoped archive
  restore                  mirror that archive back (fallback: ~/pa-saves-backup-*)
  start                    fix the loader symlink trap, launch, focus the window
  fix-links                replace the SKSE loader symlinks with real copies only
  restore-links            recreate the symlinks recorded by fix-links/start
  load                     wait for the menu, press Enter, wait for the attach line
  walk [SECONDS] [--look]  hold forward; FAIL if constraints= does not advance
  cmd LINE...              append commands and wait for their responses
  collect [OUTDIR]         copy log/out/trace + crash log + dll sha256
  stop [--no-restore]      quit/kill, restore symlinks and saves, report loadavg
  run [options]            snapshot+start+load+walk+cmd+collect+stop
  doctor                   print the environment and the paths it would use

Global options (before the subcommand): --dry-run, --run-id ID, -h/--help

run options:
  --walk SECONDS   forward hold (default $PA_WALK_DEFAULT)
  --look           add mouse-look during the walk
  --cmd LINE       a command to send (repeatable)
  --cmds FILE      one command per line; '#' comments allowed
  --outdir DIR     where collect writes (default $RUNS_DIR/collected)
  --timeout SECS   hard global timeout (default $PA_GLOBAL_TIMEOUT)
  --no-restore     do not restore the saves afterwards (unsafe; for debugging)

Environment: PA_GAME_DIR PA_ROOT PA_SNAPSHOT PA_START_TIMEOUT PA_MENU_TIMEOUT
  PA_LOAD_TIMEOUT PA_CMD_TIMEOUT PA_WALK_DEFAULT PA_FREEZE_SEC PA_QUIT_TIMEOUT
  PA_GLOBAL_TIMEOUT PA_FALLBACK_BACKUP
EOF
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
main() {
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--dry-run) DRY_RUN=1; shift ;;
			--run-id) PA_RUN_ID="${2:?}"; shift 2 ;;
			-h|--help) usage; exit 0 ;;
			-*) die "unknown global option: $1" ;;
			*) break ;;
		esac
	done
	[ "$#" -gt 0 ] || { usage; exit 1; }
	local sub="$1"; shift
	case "$sub" in
		snapshot) cmd_snapshot "$@" ;;
		restore)  cmd_restore "$@" ;;
		start)    cmd_start "$@" ;;
		fix-links) fix_loader_links ;;
		restore-links) restore_loader_links ;;
		load)     cmd_load "$@" ;;
		walk)
			local secs="$PA_WALK_DEFAULT" look=0
			while [ "$#" -gt 0 ]; do
				case "$1" in
					--look) look=1; shift ;;
					*) secs="$1"; shift ;;
				esac
			done
			cmd_walk "$secs" "$look"
			;;
		cmd)      cmd_cmd "$@" ;;
		collect)  cmd_collect "$@" ;;
		stop)
			local nr=0
			[ "${1:-}" = "--no-restore" ] && nr=1
			cmd_stop "$nr"
			;;
		run)      cmd_run "$@" ;;
		doctor)   cmd_doctor "$@" ;;
		help|-h|--help) usage ;;
		*) die "unknown command: $sub (try '$SCRIPT_NAME --help')" ;;
	esac
}

main "$@"
