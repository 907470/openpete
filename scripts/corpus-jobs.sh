#!/usr/bin/env bash
#
# corpus-jobs.sh — job-slot helper shared by the corpus capture scripts.
# SOURCED, not executed.
#
# Every corpus capture is an independent headless process writing its own
# filenames, so they parallelize with nothing to synchronise. Serially the two
# corpora take ~24 minutes together, which made the presentation golden too
# expensive to run per change; that cost is scheduling, not work.
#
# CORPUS_JOBS caps concurrency (default nproc/2). The captures render under
# software Vulkan, which is ITSELF multithreaded, so the useful ceiling is well
# below core count — past it the jobs just contend and wall-clock stops
# improving. CORPUS_JOBS=1 restores strictly serial execution, which is the
# reference any parallel run must reproduce byte-for-byte.
#
# LP_NUM_THREADS (llvmpipe's rasterizer threads per process) defaults to
# max(1, nproc / CORPUS_JOBS), so jobs x threads stays at the core count: the
# default of one llvmpipe thread per core in every job oversubscribes the CPU
# CORPUS_JOBS-fold. The thread count does not change a captured pixel.
#
# Usage:
#   source scripts/corpus-jobs.sh
#   cj_run some_function arg1 arg2      # queues, blocking only when full
#   cj_wait || fail=1                   # drains; non-zero if ANY job failed
#
# cj_run backgrounds the call, so a job cannot report failure by assigning to a
# variable — the caller's `fail=1` would be set in a subshell and lost. Failures
# are recorded in a status file instead, and surface from cj_wait.

CORPUS_JOBS="${CORPUS_JOBS:-$(( $(nproc) / 2 ))}"
[[ "$CORPUS_JOBS" -lt 1 ]] && CORPUS_JOBS=1
_cj_lp=$(( $(nproc) / CORPUS_JOBS ))
export LP_NUM_THREADS="${LP_NUM_THREADS:-$(( _cj_lp < 1 ? 1 : _cj_lp ))}"

_cj_status="$(mktemp -t corpus-jobs.XXXXXX)"
trap 'rm -f "$_cj_status"' EXIT

# Block until a slot frees. `wait -n` returns as soon as ANY child exits.
_cj_slot() {
  while (( $(jobs -rp 2>/dev/null | wc -l) >= CORPUS_JOBS )); do
    wait -n 2>/dev/null || break
  done
}

cj_run() {
  _cj_slot
  { "$@" || echo x >> "$_cj_status"; } &
}

cj_wait() {
  wait
  [[ -s "$_cj_status" ]] && { : > "$_cj_status"; return 1; }
  return 0
}
