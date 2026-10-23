#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Runs the dialog-echo probe (README.md) from Git Bash and checks each case: run.sh [build.cmd's output folder]
# (default Q:/tmp/dialog-echo-probe). Every dialog opens on a HIDDEN desktop and answers itself; nothing reaches the
# user's screen. Every program is called by its FULL path: shim/Agentmaster.exe is a stand-in, and a bare
# `Agentmaster.exe` can find the INSTALLED app on PATH.
# Exit code: the number of failed checks (0 = all passed).
set -u
OUT="$(cygpath -u "${1:-Q:/tmp/dialog-echo-probe}")"
win() { cygpath -w "$1"; }
L="$OUT/launcher.exe"
GUI="$(win "$OUT/dialoggui.exe")"
SHIM="$(win "$OUT/shim/wtd.exe")"
if [ ! -x "$L" ] || [ ! -x "$OUT/dialoggui.exe" ] || [ ! -x "$OUT/shim/wtd.exe" ] || [ ! -x "$OUT/shim/Agentmaster.exe" ]; then
    echo "build first: cmd //c \"$(win "$(dirname "$0")")\\build.cmd $(win "$OUT")\""
    exit 100
fi

failed=0
check() { # check <0 = ok> <description>
    if [ "$1" = "0" ]; then echo "PASS  $2"; else echo "FAIL  $2"; failed=$((failed + 1)); fi
}
has() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }
all() { # all <text> <needle>... : every needle is in the text
    local text="$1"; shift
    for n in "$@"; do has "$text" "$n" || return 1; done
    return 0
}

# 1. A pipe caller (bash, Python, Claude Code's Bash tool): the box, line by line, its choices, its answer.
out=$("$L" pipe "$GUI" msgbox)
all "$out" "[agentmaster dialog] opened: Probe — profile in use (dialoggui.exe, pid " "waits for an answer on the desktop)" \
    "[agentmaster dialog] |     C:\\probe\\profile" "[agentmaster dialog] | Continue anyway?" "[agentmaster dialog] | choices: Yes | No (default)" \
    "[agentmaster dialog] answered: Probe — profile in use: No" "LAUNCHER child exit=7"
check $? "pipe caller: a message box is echoed before it shows, then its answer (No)"

# 2. The update prompt's shape: buttons, radio options, the footer link, the answer with its option.
out=$("$L" pipe "$GUI" taskdialog)
all "$out" "opened: Probe update" "| Agentmaster 9.9.9 is available" "| What's new (https://example.invalid/notes)" "| button: Update now (default)" \
    "| (o) Remind me next restart (default)" "| ( ) Remind me in 3 days" "answered: Probe update: Postpone; option: Remind me in 3 days" "LAUNCHER child exit=2002"
check $? "pipe caller: a task dialog with every choice, then the button and the option picked"

# 3. The --help box path: the release is requested while the box waits on another thread; the answer still arrives.
out=$("$L" pipe "$GUI" deferred)
all "$out" "opened: Probe help" "| Usage: probe [options]" "answered: Probe help: OK" "LAUNCHER pipe ended" "LAUNCHER child exit=1"
check $? "pipe caller: a dialog open at the startup's release keeps the sink until its answer"

# 4. No dialog: a captured sink prints nothing.
out=$("$L" pipe "$GUI" none)
! has "$out" "[agentmaster dialog]" && has "$out" "LAUNCHER child exit=0"
check $? "pipe caller: no dialog, no output"

# 5. A console caller (an interactive cmd.exe): the GUI gets no standard handles and attaches to its parent's console
#    for each write.
out=$("$L" console "$GUI" msgbox)
all "$out" "CONSOLE [agentmaster dialog] opened: Probe — profile in use" "CONSOLE [agentmaster dialog] | choices: Yes | No (default)" \
    "CONSOLE [agentmaster dialog] answered: Probe — profile in use: No"
check $? "console caller: the echo lands in the caller's console"

# 6. A caller that passes the console's handles explicitly: a GUI process cannot use them (Windows does not hand a
#    console handle to a GUI process), so the sink must fall back to the parent's console, not write into the void.
out=$("$L" conhandles "$GUI" msgbox)
all "$out" "CONSOLE [agentmaster dialog] opened: Probe — profile in use" "CONSOLE [agentmaster dialog] answered: Probe — profile in use: No"
check $? "console handles passed explicitly to the GUI: refused, and the echo still lands in the console"

# 6b. PowerShell 7 with the output piped on (`& x | Out-String`): stderr is the console (unusable in a GUI process),
#     stdout PowerShell's pipe. The echo goes into the pipe, where the pipeline reads it, and nowhere else.
out=$("$L" pspiped "$GUI" msgbox)
all "$out" "[agentmaster dialog] opened: Probe — profile in use" "[agentmaster dialog] answered: Probe — profile in use: No" "LAUNCHER pipe ended" &&
    ! has "$out" "CONSOLE [agentmaster dialog]"
check $? "PowerShell 7 piped (stderr = the console, stdout = a pipe): the echo goes into the pipe"

# 7. Through the REAL launcher shim with a pipe: the shim exits at once; the app it started writes into the caller's pipe
#    through the handed-over handle, and the pipe ends only when the app released it.
out=$("$L" pipe "$SHIM" msgbox)
all "$out" "opened: Probe — profile in use (Agentmaster.exe, pid " "| choices: Yes | No (default)" "answered: Probe — profile in use: No" "LAUNCHER pipe ended"
check $? "launcher shim, pipe caller: the started app's dialog reaches the caller's pipe (AGENTMASTER_CALLER_SINK=handle)"

# 8. Through the REAL launcher shim with a console: the app attaches to the console of the shim's caller.
out=$("$L" console "$SHIM" msgbox)
all "$out" "CONSOLE [agentmaster dialog] opened: Probe — profile in use (Agentmaster.exe, pid " "CONSOLE [agentmaster dialog] answered: Probe — profile in use: No"
check $? "launcher shim, console caller: the started app's dialog reaches the caller's console (AGENTMASTER_CALLER_SINK=console)"

# 9. `cmd /c agentmaster ...`: the shim's parent is a cmd.exe that exits as soon as the shim did, usually before the app
#    it started is up. The app must still reach the console, through another process attached to it.
CMD="${COMSPEC:-C:\\Windows\\System32\\cmd.exe}"
out=$(MSYS_NO_PATHCONV=1 "$L" console "$CMD" /c "$SHIM" msgbox)
all "$out" "CONSOLE [agentmaster dialog] opened: Probe — profile in use (Agentmaster.exe, pid " "CONSOLE [agentmaster dialog] answered: Probe — profile in use: No"
check $? "cmd /c through the launcher shim: the echo outlives the cmd.exe that ran the shim"

# 10. Nothing is left running.
sleep 1
left=$(powershell -NoProfile -Command "(Get-CimInstance Win32_Process | Where-Object { \$_.ExecutablePath -like '$(win "$OUT")\\*' }).Count" 2>/dev/null | tr -d '\r')
[ "${left:-0}" = "0" ]
check $? "no probe process is left running"

echo "failed checks: $failed"
exit "$failed"
