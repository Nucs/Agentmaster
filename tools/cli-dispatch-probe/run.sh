#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Runs the CLI-dispatch probe (README.md) from Git Bash and checks each case: run.sh [build.cmd's output folder]
# (default Q:/tmp/cli-dispatch-probe; it must hold no space, for the cmd.exe case's quoting).
# ⚠ Every program here is called by its FULL path. A bare name can find the INSTALLED Agentmaster on PATH: cmd.exe with
# NoDefaultCurrentDirectoryInExePath=1 does not look in the current folder (it started the real all-users app once).
# Exit code: the number of failed checks (0 = all passed). INFO lines are observations, not checks.
set -u
OUT="$(cygpath -u "${1:-Q:/tmp/cli-dispatch-probe}")"
OUT_WIN="$(cygpath -w "$OUT")"
GUI="$OUT/fakegui.exe"
GUI_WIN="$(cygpath -w "$GUI")"
SHIM="$OUT/shim/wtd.exe" # the REAL launcher shim, beside shim/Agentmaster.exe (fakegui) and shim/agentmaster-cli.exe (fakecli)
SHIM_WIN="$(cygpath -w "$SHIM")"
if [ ! -x "$GUI" ] || [ ! -x "$OUT/agentmaster-cli.exe" ] || [ ! -x "$OUT/launcher.exe" ] || [ ! -x "$SHIM" ] ||
    [ ! -x "$OUT/shim/Agentmaster.exe" ] || [ ! -x "$OUT/shim/agentmaster-cli.exe" ]; then
    echo "build first: cmd //c \"$(cygpath -w "$(dirname "$0")")\\build.cmd $OUT_WIN\""
    exit 100
fi
rm -f "$OUT/fakecli.log" "$OUT/app-would-start.txt" "$OUT/out-file.txt" "$OUT/out-detached.txt" "$OUT/nocli/app-would-start.txt" \
    "$OUT/shim/fakecli.log" "$OUT/shim/app-would-start.txt" "$OUT/shim/out-detached.txt"

failed=0
check() { # check <0 = ok> <description>
    if [ "$1" = "0" ]; then echo "PASS  $2"; else echo "FAIL  $2"; failed=$((failed + 1)); fi
}
has() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }
# wait_for <file>: up to 5 s, in 0.1 s steps, for a marker the stand-in writes when the shim FORWARDED a line to it: the
# shim starts the app and exits at once, so the marker can land after the shim returned. Status 0 = it appeared.
wait_for() {
    local i=0
    while [ ! -f "$1" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    [ -f "$1" ]
}

# 1-6: bash passes standard handles (pipes or a file) and waits for the GUI exe.
out=$("$GUI" show a77419cf --json 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli show a77419cf --json]"; check $(( $? || rc != 42 )) "bash: a verb runs the CLI with the arguments unchanged; its exit code 42 comes back"
"$GUI" sessions exit=3 >/dev/null 2>&1; rc=$?
check $(( rc != 3 )) "bash: the CLI's own exit code (3) passes through"
out=$("$GUI" --instance dev show x 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli --instance dev show x]"; check $(( $? || rc != 42 )) "bash: a CLI-only flag first routes too"
out=$("$GUI" show "My title" --json 2>/dev/null)
has "$out" 'cmdline=[agentmaster-cli show "My title" --json]'; check $? "bash: a quoted argument arrives quoted"
err=$("$GUI" tabs 2>&1 >/dev/null)
has "$err" "FAKECLI stderr line"; check $? "bash: the CLI's stderr reaches the caller's stderr"
"$GUI" tabs --json > "$OUT/out-file.txt" 2>/dev/null
has "$(cat "$OUT/out-file.txt" 2>/dev/null)" "stdout=file"; check $? "bash: a redirect to a file receives the CLI's output"

# 7-8: command lines that are not a CLI call go on to the app (the stand-in's marker, exit 7).
"$GUI" -w new; rc=$?
has "$(cat "$OUT/app-would-start.txt" 2>/dev/null)" "-w new"; check $(( $? || rc != 7 )) "not a CLI call: '-w new' starts the app"
rm -f "$OUT/app-would-start.txt"
"$GUI" Show x; rc=$?
[ -f "$OUT/app-would-start.txt" ]; check $(( $? || rc != 7 )) "not a CLI call: a verb in capitals starts the app (exact match)"
rm -f "$OUT/app-would-start.txt"

# 9: Python's subprocess with captured output waits.
out=$(python -c "import subprocess, sys; r = subprocess.run([sys.argv[1], 'list', '--json'], capture_output=True, text=True); print(r.returncode, r.stdout.strip())" "$GUI_WIN")
has "$out" "42 FAKECLI cmdline=[agentmaster-cli list --json] stdout=pipe"; check $? "python: subprocess.run captures the output and the exit code"

# 10: cmd /c waits and passes the exit code on (the CLI writes to cmd's console, here a hidden one).
case "$OUT_WIN" in
*" "*) echo "INFO  cmd /c case skipped: the output folder holds a space" ;;
*)
    MSYS_NO_PATHCONV=1 cmd /c "$GUI_WIN show viaCmd exit=4"; rc=$?
    check $(( rc != 4 )) "cmd /c: waits for the GUI exe and returns the CLI's exit code"
    ;;
esac

# 11-12: PowerShell waits only when the output is piped on.
PS=$(command -v pwsh || echo "/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
out=$("$PS" -NoProfile -Command "\$o = & '$GUI_WIN' windows viaPipe | Out-String; \$o.Trim(); exit \$LASTEXITCODE" 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli windows viaPipe]"; check $(( $? || rc != 42 )) "powershell: '& exe ... | Out-String' waits, captures and sets LASTEXITCODE"
out=$("$PS" -NoProfile -Command "& '$GUI_WIN' external viaPwsh; 'LASTEXITCODE=[' + \$LASTEXITCODE + ']'" 2>/dev/null)
echo "INFO  powershell plain call (does not wait for a GUI program): $out"

# 13-14: the two launches a shell cannot make here.
out=$("$OUT/launcher.exe" console "$GUI_WIN" show viaConsole exit=5)
has "$out" "child exit=5"; r1=$?
has "$(cat "$OUT/fakecli.log")" "[agentmaster-cli show viaConsole exit=5] stdout=console"; check $(( r1 || $? )) "no handles + a console (cmd.exe-like): the CLI writes to the caller's console"
out=$("$OUT/launcher.exe" detached "$GUI_WIN" sessions viaDetached)
has "$out" "child exit=42"; r1=$?
has "$(cat "$OUT/out-detached.txt" 2>/dev/null)" "FAKECLI stderr line"; r2=$?
has "$(cat "$OUT/out-detached.txt" 2>/dev/null)" "stdout=file"; check $(( r1 || r2 || $? )) "handles + no console (pythonw-like): the CLI writes to the caller's file, no window"

# 15: no CLI beside the exe: one line on stderr, exit 1, and the app is NOT started.
err=$("$OUT/nocli/fakegui.exe" show x 2>&1 >/dev/null); rc=$?
has "$err" "could not start"; r1=$?
[ ! -f "$OUT/nocli/app-would-start.txt" ]; check $(( r1 || $? || rc != 1 )) "no agentmaster-cli.exe: a message, exit 1, and the app is not started"

# 17-23: help and the version (2026-10-31: `agentmaster --help` through the kept all-users copy opened "profile in use").
# Git Bash passes handles, so it reads text: the CLI answers each token. '-?' is quoted (a glob otherwise), and '/?' runs
# with MSYS_NO_PATHCONV=1 (Git Bash would turn it into a path).
for t in --help -h '-?' --version -v; do
    out=$("$GUI" "$t" 2>/dev/null); rc=$?
    has "$out" "cmdline=[agentmaster-cli $t]"; check $(( $? || rc != 42 )) "bash: '$t' first runs the CLI (help and the version are the CLI's for a caller that reads text)"
done
out=$(MSYS_NO_PATHCONV=1 "$GUI" '/?' 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli /?]"; check $(( $? || rc != 42 )) "bash: '/?' first runs the CLI too"
out=$("$GUI" --archived sessions 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli --archived sessions]"; check $(( $? || rc != 42 )) "bash: --archived first is a CLI-only flag"

# 24-25: still the app's: Windows Terminal's split-pane flag -H (an exact match decides) and a help asked after a verb of
# the app (a new tab's help box, asked for by name).
"$GUI" -H; rc=$?
[ -f "$OUT/app-would-start.txt" ]; check $(( $? || rc != 7 )) "not a CLI call: '-H' (split-pane, not help) starts the app"
rm -f "$OUT/app-would-start.txt"
"$GUI" nt --help; rc=$?
[ -f "$OUT/app-would-start.txt" ]; check $(( $? || rc != 7 )) "not a CLI call: 'nt --help' starts the app"
rm -f "$OUT/app-would-start.txt"

# 26-28: the launches a shell cannot make. A console without handles and a file without a console both read text, so the
# CLI answers; with neither (the Run box, a shortcut) the app's message box is the only answer that caller can read.
out=$("$OUT/launcher.exe" console "$GUI_WIN" --version)
has "$out" "child exit=42"; r1=$?
has "$(cat "$OUT/fakecli.log")" "[agentmaster-cli --version] stdout=console"; check $(( r1 || $? )) "no handles + a console: '--version' runs the CLI on that console"
out=$("$OUT/launcher.exe" detached "$GUI_WIN" -h)
has "$out" "child exit=42"; r1=$?
has "$(cat "$OUT/out-detached.txt" 2>/dev/null)" "cmdline=[agentmaster-cli -h] stdout=file"; check $(( r1 || $? )) "handles + no console: '-h' runs the CLI into the caller's file, no window"
out=$("$OUT/launcher.exe" none "$GUI_WIN" --help)
has "$out" "child exit=7"; r1=$?
has "$(cat "$OUT/app-would-start.txt" 2>/dev/null)" "--help"; check $(( r1 || $? )) "no handles + no console (the Run box): '--help' goes to the app, whose box is the only readable answer"
rm -f "$OUT/app-would-start.txt"

# 29-35: the REAL launcher shim (shim/wtd.exe, the execution alias's target). It runs the CLI beside it and waits; it
# forwards anything else to shim/Agentmaster.exe (fakegui) and exits at once, so a forwarded line is seen by its marker.
out=$("$SHIM" --help 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli --help]"; check $(( $? || rc != 42 )) "shim, bash: '--help' runs the CLI and returns its exit code"
out=$("$SHIM" sessions --json 2>/dev/null); rc=$?
has "$out" "cmdline=[agentmaster-cli sessions --json]"; check $(( $? || rc != 42 )) "shim, bash: a verb runs the CLI, as before"
[ ! -f "$OUT/shim/app-would-start.txt" ]; check $? "shim, bash: neither started the app"
"$SHIM" -w new >/dev/null 2>&1
wait_for "$OUT/shim/app-would-start.txt"; r1=$?
has "$(cat "$OUT/shim/app-would-start.txt" 2>/dev/null)" "-w new"; check $(( r1 || $? )) "shim, bash: '-w new' is forwarded to the app, as before"
rm -f "$OUT/shim/app-would-start.txt"
out=$("$OUT/launcher.exe" none "$SHIM_WIN" --help)
wait_for "$OUT/shim/app-would-start.txt"; r1=$?
has "$(cat "$OUT/shim/app-would-start.txt" 2>/dev/null)" "--help"; check $(( r1 || $? )) "shim, no handles + no console (the Run box): '--help' is forwarded to the app"
rm -f "$OUT/shim/app-would-start.txt"
out=$("$OUT/launcher.exe" detached "$SHIM_WIN" -v)
has "$out" "child exit=42"; r1=$?
has "$(cat "$OUT/shim/out-detached.txt" 2>/dev/null)" "cmdline=[agentmaster-cli -v] stdout=file"; check $(( r1 || $? )) "shim, handles + no console: '-v' runs the CLI into the caller's file"
# The shim SHARES the launcher's console (its decision: callerSharesConsole) and is passed no handles. Unlike the GUI
# stand-in it opens no CONOUT$: a console child keeps the standard handles it inherited, here the launcher's own, which
# Git Bash made pipes (measured: stdout=pipe). So the check is the routing, not where the text lands.
out=$("$OUT/launcher.exe" console "$SHIM_WIN" '-?' 2>/dev/null)
has "$out" "child exit=42"; r1=$?
has "$(cat "$OUT/shim/fakecli.log" 2>/dev/null)" "[agentmaster-cli -?]"; check $(( r1 || $? )) "shim, a shared console without handles: '-?' runs the CLI"
echo "INFO  shim CLI runs: $(tr '\n' ' ' < "$OUT/shim/fakecli.log" 2>/dev/null)"

# 36-37: across every CLI call above, the app never started (each app-path check removed its own marker) and no console
# window was ever made.
[ ! -f "$OUT/app-would-start.txt" ]; check $? "no CLI call started the app"
has "$(cat "$OUT/fakecli.log")" "consoleWindow=yes"; check $(( ! $? )) "no CLI run got a console window"

echo "failed checks: $failed"
exit "$failed"
