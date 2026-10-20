# wslenv.sh - forward environment knobs to a Windows .exe launched from WSL.
#
# WSL interop does not pass the Linux environment to a Windows child. Only the
# names listed in WSLENV cross, and a name that is missing produces no error:
# the exe starts, reads the variable as unset, and runs a DIFFERENT
# configuration from the one the harness asked for. Every result it then
# produces is real, self-consistent, and about the wrong thing.
#
# Usage:
#   . scripts/lib/wslenv.sh
#   wslenv_forward OPENPETE_FORCE_PACING=1 OPENPETE_PACE_INJECT_US=3000
#   env "${WSLENV_ARGS[@]}" ./build/windows-mingw/.../foo.exe ...
#
# WSLENV_ARGS holds the assignments plus the WSLENV entry that makes them
# actually arrive. OPENPETE_THREAD_ROLE_FATAL and OPENPETE_THREAD_ROLE_REPORT,
# when set in the caller's environment, are forwarded too. Safe on native Linux: WSLENV is simply ignored there.

wslenv_forward() {
  WSLENV_ARGS=()
  local names="" a
  for a in "$@"; do
    WSLENV_ARGS+=("$a")
    names="${names}${names:+:}${a%%=*}/w"
  done
  # The thread-role knobs a gate harness exports cross with every forward:
  # the fatal flag as is, the report path translated (/p).
  [ -n "${OPENPETE_THREAD_ROLE_FATAL:-}" ] && [[ ":$names:" != *":OPENPETE_THREAD_ROLE_FATAL/"* ]] &&
    names="${names}${names:+:}OPENPETE_THREAD_ROLE_FATAL/w"
  [ -n "${OPENPETE_THREAD_ROLE_REPORT:-}" ] && [[ ":$names:" != *":OPENPETE_THREAD_ROLE_REPORT/"* ]] &&
    names="${names}${names:+:}OPENPETE_THREAD_ROLE_REPORT/wp"
  [ -n "$names" ] && WSLENV_ARGS+=("WSLENV=${WSLENV:+$WSLENV:}${names}")
}

# win_tree_exe BUILD GAME: the Windows executable that runs GAME (spyro-2) in
# the MinGW build tree BUILD. A shared-linkage tree builds one executable,
# openpete.exe at its root, whose disc argument selects the game; a static
# tree builds games/<game>/openpete-<tag>.exe. The file present decides; with
# neither built, the root name.
win_tree_exe() {
  local own="$1/games/$2/openpete-${2//-/}.exe"
  if [ ! -e "$1/openpete.exe" ] && [ -e "$own" ]; then echo "$own"; else echo "$1/openpete.exe"; fi
}
