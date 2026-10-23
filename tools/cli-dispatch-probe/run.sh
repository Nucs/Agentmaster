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
if [ ! -x "$GUI" ] || [ ! -x "$OUT/agentmaster-cli.exe" ] || [ ! -x "$OUT/launcher.exe" ]; then
    echo "build first: cmd //c \"$(cygpath -w "$(dirname "$0")")\\build.cmd $OUT_WIN\""
    exit 100
fi
rm -f "$OUT/fakecli.log" "$OUT/app-would-start.txt" "$OUT/out-file.txt" "$OUT/out-detached.txt" "$OUT/nocli/app-would-start.txt"

failed=0
check() { # check <0 = ok> <description>
    if [ "$1" = "0" ]; then echo "PASS  $2"; else echo "FAIL  $2"; failed=$((failed + 1)); fi
}
has() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

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

# 16: across every CLI call above, the app never started and no console window was ever made.
[ ! -f "$OUT/app-would-start.txt" ]; check $? "no CLI call started the app"
has "$(cat "$OUT/fakecli.log")" "consoleWindow=yes"; check $(( ! $? )) "no CLI run got a console window"

echo "failed checks: $failed"
exit "$failed"
