#!/usr/bin/env bash
#
# own_children.sh - a harness owns the processes it starts: a TERM, INT or
# HUP to the harness stops every process below it before the harness exits.
# SOURCED, not executed; sourcing it installs the traps.
#
# Without this, a signal to the harness (an outer `timeout`, a `kill` of its
# PID) ends the shell and leaves its game process running: openpete takes
# the first TERM or INT as a graceful-shutdown request and exits only when
# its loop next checks the flag, and an inner `timeout` puts itself and its
# child in a process group of their own, out of reach of a signal sent to
# the harness's group.
#
# Bash runs a trap only between commands or while the `wait` builtin
# blocks, never while a foreground command runs. A harness therefore starts
# each game process through op_own_run, which runs it as a background job
# and waits on it, so the trap runs the moment the signal arrives. A
# harness that already runs its legs in the background and waits on them
# needs only the source: the trap stops every descendant of the shell.
#
# The stop: TERM to every descendant of the harness shell, then KILL to
# those still alive after OP_OWN_GRACE seconds (default 10). The harness
# then exits 128 + the signal number, which runs its EXIT trap.

OP_OWN_GRACE="${OP_OWN_GRACE:-10}"
# The script's stderr at source time: the trap's messages go there, not to
# the log a redirected op_own_run call is writing when the signal arrives.
[[ -n "${OP_OWN_ERR:-}" ]] || exec {OP_OWN_ERR}>&2

# _op_own_descendants: print the PID of every live descendant of this
# shell, one per line, from one process-table snapshot. The command
# substitution running the snapshot and its own children are left out.
_op_own_descendants() {
    local self="$BASHPID"
    ps -e -o pid=,ppid= | awk -v root="$$" -v self="$self" '
        { kids[$2] = kids[$2] " " $1 }
        END {
            n = split(kids[root], q, " ")
            for (i = 1; i <= n; i++) {
                p = q[i]
                if (p == self) continue
                print p
                m = split(kids[p], c, " ")
                for (j = 1; j <= m; j++) q[++n] = c[j]
            }
        }'
}

# _op_own_alive PID...: status 0 when any named process exists and is not
# a zombie (a dead child of this shell stays a zombie until reaped, and
# `kill -0` succeeds on it).
_op_own_alive() {
    local p st
    for p in "$@"; do
        st="$(awk '{ print $3 }' "/proc/$p/stat" 2>/dev/null)" || continue
        [[ -n "$st" && "$st" != Z ]] && return 0
    done
    return 1
}

# op_own_reap: stop every descendant of this shell, TERM first and KILL
# after OP_OWN_GRACE seconds. The PID list is taken before the first
# signal, so a process that its dying parent hands to init is still killed.
op_own_reap() {
    local -a pids
    mapfile -t pids < <(_op_own_descendants)
    (( ${#pids[@]} )) || return 0
    kill -TERM "${pids[@]}" 2>/dev/null
    local t=0 limit=$(( OP_OWN_GRACE * 5 ))
    while (( t < limit )) && _op_own_alive "${pids[@]}"; do
        sleep 0.2
        t=$(( t + 1 ))
    done
    if _op_own_alive "${pids[@]}"; then
        echo "own_children: KILL after ${OP_OWN_GRACE}s: $(
            for p in "${pids[@]}"; do _op_own_alive "$p" && printf '%s ' "$p"; done)" >&"$OP_OWN_ERR"
        kill -KILL "${pids[@]}" 2>/dev/null
    fi
    return 0
}

# _op_own_on_signal SIGNAL CODE: the trap body; stop the descendants and
# exit CODE, which runs the EXIT trap.
_op_own_on_signal() {
    trap - TERM INT HUP
    echo "own_children: $1 received; stopping child processes" >&"$OP_OWN_ERR"
    op_own_reap
    exit "$2"
}

# op_own_run CMD [ARGS...]: run CMD as a background job of this shell and
# wait for it, so a signal to the shell runs the trap at once. Returns
# CMD's exit status. The caller's redirections apply; stdin is passed
# through (a background job's stdin is otherwise /dev/null). Called from a
# subshell (a command substitution, a pipeline stage, a backgrounded
# function), which a signal to the harness does not reach, CMD runs in the
# foreground as written.
op_own_run() {
    if [[ "$BASHPID" != "$$" ]]; then
        "$@"
        return
    fi
    local pid
    { "$@" <&3 3<&- {OP_OWN_ERR}>&- & } 3<&0
    pid=$!
    wait "$pid"
}

trap '_op_own_on_signal TERM 143' TERM
trap '_op_own_on_signal INT 130' INT
trap '_op_own_on_signal HUP 129' HUP
