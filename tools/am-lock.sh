#!/usr/bin/env bash
# Agentmaster — global build/launch mutex (multi-agent safe).  [Agentmaster]
#
# WHY ------------------------------------------------------------------------------------------
# The dev inner loop is close -> build -> relaunch (see CLAUDE.md "Deploy & run"). It is
# DESTRUCTIVE and process-global: there is exactly ONE dev instance and ONE build output tree
# (src\cascadia\CascadiaPackage\bin\x64\Debug). If two actors run it at once — two AI agents, or
# an agent and a human — they collide:
#   * one closes the WindowsTerminal.exe the other just launched;
#   * two msbuilds race on the same bin\x64\Debug outputs (corrupt/partial link);
#   * the exe link fails outright because a running WindowsTerminal.exe LOCKS the very
#     WindowsTerminal.exe being relinked.
# So the WHOLE cycle must be serialized behind one mutex. CLAUDE.md makes holding it a
# REQUIREMENT before any build or launch/deploy of our dev package.
#
# HOW ------------------------------------------------------------------------------------------
# A filesystem lock built on the atomicity of mkdir(2): a directory is created by exactly one
# caller or already exists — there is no check-then-create TOCTOU window (on Windows mkdir maps
# to CreateDirectory, which fails ERROR_ALREADY_EXISTS atomically). The lock is a DIRECTORY on
# disk, so it persists across separate shells: a single agent can `acquire` it, then run close /
# build / launch as separate Bash tool calls (each a fresh shell), then `release` it. It lives
# under %USERPROFILE%\.agentmaster\locks — the same un-virtualized state dir the app, the hooks,
# and every agent already agree on (NOT %LOCALAPPDATA%; see CLAUDE.md MSIX gotcha).
#
# A crashed holder can't wedge the lock forever: each lock records an epoch + TTL (default 30
# min, generous vs a ~4-min build). `acquire` treats an over-TTL lock as STALE and breaks it
# (atomically, via rename — see the break path). A long legitimate hold can `refresh` to extend.
#
# USAGE (each step is a separate Bash tool call by the SAME agent) ----------------------------
#   TOKEN=$(bash tools/am-lock.sh acquire --wait 600 --label "deploy HEAD") || exit 1
#   ... close ONLY our dev instance ...
#   ... build (full exe link) ...
#   ... relaunch ...
#   bash tools/am-lock.sh release --token "$TOKEN"
#
# Or, for a one-shot locked command (acquire -> run -> release in one call):
#   bash tools/am-lock.sh with --wait 600 --label build -- pwsh -File tools/Build-Agentmaster.ps1 -NoRestore
#
# COMMANDS ------------------------------------------------------------------------------------
#   acquire [--wait S] [--ttl-min M] [--label TXT] [--force]
#               -> on success prints the bare lock TOKEN to stdout (capture it) and exits 0;
#                  if held and not --wait, prints the holder to stderr and exits 3.
#   release [--token T] [--force]    -> idempotent; refuses on token mismatch (exit 4) unless --force.
#   refresh [--token T] [--ttl-min M]-> extend a long hold (heartbeat); exit 4 on mismatch.
#   status                            -> prints HELD / HELD-BUT-STALE / FREE + holder metadata.
#   with [opts] -- CMD [ARGS...]      -> acquire, run CMD, release (release even if CMD fails).
#
# Notes: stdout of `acquire` is ONLY the token (machine-parseable); all human diagnostics go to
# stderr. Exit codes: 0 ok, 2 bad args, 3 busy/timeout, 4 token mismatch.
set -u

LOCK_NAME="build-launch"
DEFAULT_TTL_MIN=30

# --- state dir -------------------------------------------------------------------------------
# Match the app exactly: %USERPROFILE%\.agentmaster (AgentmasterStateDir() in C++). Convert the
# Windows path to a unix path so bash builtins (mkdir/[ -d ]/rm) get clean forward slashes.
_am_state_dir() {
  if [ -n "${AGENTMASTER_STATE_DIR:-}" ]; then printf '%s' "$AGENTMASTER_STATE_DIR"; return; fi
  local base="${USERPROFILE:-$HOME}"
  if command -v cygpath >/dev/null 2>&1; then base="$(cygpath -u "$base" 2>/dev/null || printf '%s' "$base")"; fi
  printf '%s/.agentmaster' "$base"
}

LOCKS_DIR="$(_am_state_dir)/locks"
LOCK_DIR="$LOCKS_DIR/${LOCK_NAME}.lock.d"
OWNER_FILE="$LOCK_DIR/owner"

# --- small helpers ---------------------------------------------------------------------------
_am_now()  { date +%s; }
_am_iso()  { date -u +%Y-%m-%dT%H:%M:%SZ; }
_am_uuid() { cat /proc/sys/kernel/random/uuid 2>/dev/null || printf '%s-%s-%s' "$(_am_now)" "$$" "${RANDOM:-0}"; }
_am_int()  { case "${1:-}" in (''|*[!0-9]*) printf '%s' "$2";; (*) printf '%s' "$1";; esac; }  # value default

# Poll cadence while waiting: ~0.7–1.5s, JITTERED. The jitter de-synchronizes a herd of waiters so
# one doesn't repeatedly wake in lock-step and lose the same race (observed starvation under a
# fixed poll). Fine-grained vs a multi-minute build hold, so handoff after a release is ~1s. Falls
# back to a whole-second sleep if this sleep can't do fractional seconds.
_am_wait_sleep() {
  local n=$(( 700 + RANDOM % 800 ))
  sleep "$(printf '%d.%03d' "$(( n / 1000 ))" "$(( n % 1000 ))")" 2>/dev/null || sleep 1
}

_am_read_owner_field() {  # field -> value (first line), or empty
  [ -f "$OWNER_FILE" ] || return 1
  sed -n "s/^$1=//p" "$OWNER_FILE" 2>/dev/null | head -n1
}

_am_owner_summary() {
  if [ -f "$OWNER_FILE" ]; then
    local pid host iso label epoch age
    pid="$(_am_read_owner_field pid)";   host="$(_am_read_owner_field host)"
    iso="$(_am_read_owner_field iso)";   label="$(_am_read_owner_field label)"
    epoch="$(_am_read_owner_field epoch)"
    age=$(( $(_am_now) - $(_am_int "$epoch" 0) ))
    printf 'holder: host=%s pid=%s label="%s" acquired=%s age=%ss' "$host" "$pid" "$label" "$iso" "$age"
  else
    printf 'holder: (owner file not yet written — lock being created)'
  fi
}

_am_write_owner() {  # token label ttl_min  -> writes $OWNER_FILE
  # The reader (_am_read_owner_field) assumes one field per line, so sanitize the agent-supplied
  # label down to a single line (strip CR/LF) and emit it LAST.
  local lbl="${2//$'\n'/ }"; lbl="${lbl//$'\r'/ }"
  {
    printf 'token=%s\n'   "$1"
    printf 'pid=%s\n'     "$$"
    printf 'host=%s\n'    "${HOSTNAME:-$(hostname 2>/dev/null)}"
    printf 'epoch=%s\n'   "$(_am_now)"
    printf 'iso=%s\n'     "$(_am_iso)"
    printf 'ttl_min=%s\n' "$3"
    printf 'label=%s\n'   "$lbl"
  } > "$OWNER_FILE"
}

# Stale = owner records an epoch older than its TTL. A MISSING/empty owner is NOT stale: that is
# the sub-millisecond window between mkdir and the owner write, and breaking then would clobber a
# lock that is being legitimately created. A genuinely wedged empty lock is cleared with --force.
_am_is_stale() {
  local epoch ttl_sec age
  epoch="$(_am_read_owner_field epoch)"
  [ -n "$epoch" ] || return 1
  ttl_sec=$(( $(_am_int "$(_am_read_owner_field ttl_min)" "$DEFAULT_TTL_MIN") * 60 ))
  age=$(( $(_am_now) - $(_am_int "$epoch" 0) ))
  [ "$age" -ge "$ttl_sec" ]
}

# Break a stale/forced lock WITHOUT a rm+mkdir double-break race: rename the dir out of the way
# first (rename is atomic — only one racer's mv wins; the loser's mv fails because the source is
# already gone), then delete the tombstone. After this the caller loops back to mkdir, which is
# itself atomic, so at most one winner re-creates the lock.
_am_break_lock() {
  local tomb="${LOCK_DIR}.broken.$$.${RANDOM:-0}"
  mv "$LOCK_DIR" "$tomb" 2>/dev/null && rm -rf "$tomb" 2>/dev/null
}

# --- commands --------------------------------------------------------------------------------
cmd_acquire() {
  local wait_secs=0 ttl_min=$DEFAULT_TTL_MIN label="" force=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --wait)    wait_secs="$(_am_int "${2:-0}" 0)"; shift 2;;
      --ttl-min) ttl_min="$(_am_int "${2:-$DEFAULT_TTL_MIN}" "$DEFAULT_TTL_MIN")"; shift 2;;
      --label)   label="${2:-}"; shift 2;;
      --force|--steal) force=1; shift;;
      *) echo "[am-lock] acquire: unknown arg '$1'" >&2; return 2;;
    esac
  done
  mkdir -p "$LOCKS_DIR" 2>/dev/null
  local deadline=$(( $(_am_now) + wait_secs ))
  while :; do
    if mkdir "$LOCK_DIR" 2>/dev/null; then
      local token; token="$(_am_uuid)"
      _am_write_owner "$token" "$label" "$ttl_min"
      # Read-back guard: if a racing stale-breaker clobbered us between mkdir and write, the stored
      # token won't be ours — loop and retry rather than falsely believing we hold it.
      if [ "$(_am_read_owner_field token)" = "$token" ]; then
        echo "[am-lock] ACQUIRED $LOCK_NAME  token=$token  ttl=${ttl_min}m  label=\"$label\"" >&2
        printf '%s\n' "$token"
        return 0
      fi
      continue
    fi
    # Held. Break it if forced or stale, else wait / give up.
    if [ "$force" = 1 ] || _am_is_stale; then
      local reason=stale; [ "$force" = 1 ] && reason=forced
      echo "[am-lock] BREAKING ($reason) $LOCK_NAME — $(_am_owner_summary)" >&2
      _am_break_lock
      continue
    fi
    if [ "$(_am_now)" -ge "$deadline" ]; then
      echo "[am-lock] BUSY $LOCK_NAME — $(_am_owner_summary)" >&2
      return 3
    fi
    echo "[am-lock] waiting up to $(( deadline - $(_am_now) ))s for $LOCK_NAME — $(_am_owner_summary)" >&2
    _am_wait_sleep
  done
}

cmd_release() {
  local token="" force=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --token) token="${2:-}"; shift 2;;
      --force) force=1; shift;;
      *) echo "[am-lock] release: unknown arg '$1'" >&2; return 2;;
    esac
  done
  if [ ! -d "$LOCK_DIR" ]; then
    echo "[am-lock] $LOCK_NAME not held (nothing to release)" >&2
    return 0
  fi
  local stored; stored="$(_am_read_owner_field token)"
  if [ "$force" = 1 ] || [ -z "$stored" ] || [ "$token" = "$stored" ]; then
    _am_break_lock
    echo "[am-lock] RELEASED $LOCK_NAME" >&2
    return 0
  fi
  echo "[am-lock] REFUSED release of $LOCK_NAME: token mismatch (supplied=\"$token\"; $(_am_owner_summary)). Use --force to override." >&2
  return 4
}

cmd_refresh() {
  local token="" ttl_min=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --token)   token="${2:-}"; shift 2;;
      --ttl-min) ttl_min="${2:-}"; shift 2;;
      *) echo "[am-lock] refresh: unknown arg '$1'" >&2; return 2;;
    esac
  done
  [ -d "$LOCK_DIR" ] || { echo "[am-lock] $LOCK_NAME not held — cannot refresh" >&2; return 3; }
  local stored; stored="$(_am_read_owner_field token)"
  [ "$token" = "$stored" ] || { echo "[am-lock] refresh refused: token mismatch — $(_am_owner_summary)" >&2; return 4; }
  local lbl new_ttl
  lbl="$(_am_read_owner_field label)"
  new_ttl="$(_am_int "${ttl_min:-$(_am_read_owner_field ttl_min)}" "$DEFAULT_TTL_MIN")"
  _am_write_owner "$token" "$lbl" "$new_ttl"
  echo "[am-lock] REFRESHED $LOCK_NAME (ttl=${new_ttl}m)" >&2
}

cmd_status() {
  if [ -d "$LOCK_DIR" ]; then
    if _am_is_stale; then
      echo "[am-lock] $LOCK_NAME = HELD-BUT-STALE — $(_am_owner_summary)"
    else
      echo "[am-lock] $LOCK_NAME = HELD — $(_am_owner_summary)"
    fi
    return 0
  fi
  echo "[am-lock] $LOCK_NAME = FREE"
}

cmd_with() {
  local wait_secs=600 ttl_min=$DEFAULT_TTL_MIN label="locked-run"
  while [ $# -gt 0 ]; do
    case "$1" in
      --wait)    wait_secs="$(_am_int "${2:-600}" 600)"; shift 2;;
      --ttl-min) ttl_min="$(_am_int "${2:-$DEFAULT_TTL_MIN}" "$DEFAULT_TTL_MIN")"; shift 2;;
      --label)   label="${2:-}"; shift 2;;
      --) shift; break;;
      *) echo "[am-lock] with: unknown arg '$1'" >&2; return 2;;
    esac
  done
  [ $# -gt 0 ] || { echo "[am-lock] with: no command after '--'" >&2; return 2; }
  local token
  token="$(cmd_acquire --wait "$wait_secs" --ttl-min "$ttl_min" --label "$label")" || return 3
  "$@"; local rc=$?
  cmd_release --token "$token"
  return "$rc"
}

cmd_help() {
  sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//'
}

main() {
  local cmd="${1:-status}"; [ $# -gt 0 ] && shift
  case "$cmd" in
    acquire) cmd_acquire "$@";;
    release) cmd_release "$@";;
    refresh) cmd_refresh "$@";;
    status)  cmd_status  "$@";;
    with)    cmd_with    "$@";;
    -h|--help|help) cmd_help;;
    *) echo "[am-lock] usage: am-lock.sh {acquire|release|refresh|status|with} [opts]   (try --help)" >&2; return 2;;
  esac
}
main "$@"
