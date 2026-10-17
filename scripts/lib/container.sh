#!/usr/bin/env bash
# scripts/lib/container.sh - the container plumbing shared by
# scripts/container.sh, scripts/release-container.sh and
# scripts/golden-capture-container.sh. SOURCED, not executed.
#
#   ctr_engine PREFIX          set CTR_ENGINE (OPENPETE_CONTAINER_ENGINE, else
#                              podman, else docker); exit 64 when none is found;
#                              set CTR_ENGINE_ARGS, the global options every
#                              `"$CTR_ENGINE" "${CTR_ENGINE_ARGS[@]}" run`
#                              passes: under rootless podman with no session
#                              bus at $XDG_RUNTIME_DIR/bus, --conmon
#                              scripts/lib/conmon-no-session-bus, so the
#                              container's cleanup autolaunches no dbus-daemon
#   ctr_engine_mode            one line naming the engine, its version, its
#                              rootless or rootful mode and storage driver
#   ctr_uidmap                 set CTR_UIDMAP: the host uid inside the
#                              container (--userns=keep-id under podman,
#                              --user uid:gid under docker), so files written
#                              into a bind-mounted tree belong to the host user
#   ctr_mount HOST CTR [ro]    append a bind mount to CTR_MOUNTS, relabelled
#                              (:z) when SELinux is enforcing
#   ctr_tree_mounts TREE [CTREE]
#                              the tree read-write at CTREE (default its own
#                              absolute path); for a git worktree also the
#                              main checkout read-only and the common git
#                              directory read-write, so the worktree's links
#                              and .git file resolve as on the host
#   ctr_home_env HOME_HOST HOME_CTR
#                              create HOME_HOST and append HOME and the XDG
#                              directories under HOME_CTR to CTR_ENV
#   ctr_image_exists IMAGE
#   ctr_build_image DOCKERFILE CONTEXT IMAGE [STAGE [BASE_STAGE=IMAGE...]]
#                              build one stage; under podman with the vfs
#                              storage driver, replay it with run + commit
#
# Rootless podman on a filesystem without overlay support (btrfs) stores
# images with the vfs driver, configured by the user in
# ~/.config/containers/storage.conf ([storage] driver = "vfs"); this file
# reads the driver and never writes the configuration. `podman build`
# overlay-mounts its context even there, so ctr_build_image replays the
# Dockerfile instead: per stage, its RUN, COPY and WORKDIR steps run in one
# container of the stage's FROM image and the container is committed with
# --change for every ENV and the last WORKDIR. Any other instruction is
# refused by name.

# The arrays are read by the scripts that source this file.
# shellcheck disable=SC2034
CTR_MOUNTS=()
CTR_ENV=()
CTR_UIDMAP=()
CTR_ENGINE_ARGS=()
CTR_ENGINE="${CTR_ENGINE:-}"
CTR_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CTR_PREFIX="${CTR_PREFIX:-container}"

ctr_die() { echo "[$CTR_PREFIX] $*" >&2; exit "${CTR_DIE_CODE:-1}"; }

ctr_engine() {
    CTR_PREFIX="${1:-$CTR_PREFIX}"
    CTR_ENGINE="${OPENPETE_CONTAINER_ENGINE:-}"
    if [[ -z "$CTR_ENGINE" ]]; then
        local e
        for e in podman docker; do
            command -v "$e" >/dev/null && { CTR_ENGINE=$e; break; }
        done
    fi
    [[ -n "$CTR_ENGINE" ]] || { echo "[$CTR_PREFIX] no podman or docker found" >&2; exit 64; }
    command -v "$CTR_ENGINE" >/dev/null \
        || { echo "[$CTR_PREFIX] OPENPETE_CONTAINER_ENGINE=$CTR_ENGINE is not on PATH" >&2; exit 64; }
    CTR_ENGINE_ARGS=()
    if [[ "${CTR_ENGINE##*/}" == podman && "$(id -u)" != 0 \
          && ! -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus" ]]; then
        CTR_ENGINE_ARGS=(--conmon "$CTR_LIB_DIR/conmon-no-session-bus")
    fi
}

ctr_storage_driver() {
    [[ "$CTR_ENGINE" == podman ]] || { echo ""; return; }
    podman info --format '{{.Store.GraphDriverName}}' 2>/dev/null || true
}

ctr_engine_mode() {
    local ver mode driver
    ver="$("$CTR_ENGINE" --version 2>/dev/null | head -1)"
    if [[ "$CTR_ENGINE" == podman ]]; then
        mode="$(podman info --format '{{.Host.Security.Rootless}}' 2>/dev/null)"
        [[ "$mode" == true ]] && mode=rootless || mode=rootful
        driver="$(ctr_storage_driver)"
    else
        mode="$(docker info --format '{{.SecurityOptions}}' 2>/dev/null)"
        [[ "$mode" == *rootless* ]] && mode=rootless || mode=rootful
        driver="$(docker info --format '{{.Driver}}' 2>/dev/null)"
    fi
    echo "$ver, $mode, storage driver ${driver:-unknown}"
}

# shellcheck disable=SC2034
ctr_uidmap() {
    CTR_UIDMAP=(--user "$(id -u):$(id -g)")
    [[ "$CTR_ENGINE" == podman ]] && CTR_UIDMAP=(--userns=keep-id)
    return 0
}

ctr_selinux_enforcing() {
    command -v getenforce >/dev/null && [[ "$(getenforce 2>/dev/null)" == Enforcing ]]
}

ctr_mount() {
    local host="$1" ctr="$2" opts=()
    [[ "${3:-}" == ro ]] && opts+=(ro)
    ctr_selinux_enforcing && opts+=(z)
    local suffix=""
    (( ${#opts[@]} )) && suffix=":$(IFS=,; echo "${opts[*]}")"
    CTR_MOUNTS+=(-v "$host:$ctr$suffix")
}

ctr_tree_mounts() {
    local tree="$1" ctree="${2:-$1}" common main
    common="$(git -C "$tree" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
    main="${common%/.git}"
    if [[ "$ctree" == "$tree" && -n "$common" && "$main" != "$common" && "$main" != "$tree" ]]; then
        # The common git directory stays writable beneath the read-only
        # checkout: git in the worktree writes its index and objects there.
        ctr_mount "$main" "$main" ro
        ctr_mount "$common" "$common"
    fi
    ctr_mount "$tree" "$ctree"
}

ctr_home_env() {
    local host="$1" ctr="$2"
    mkdir -p "$host/run"
    chmod 700 "$host/run"
    CTR_ENV+=(-e "HOME=$ctr" -e "XDG_CONFIG_HOME=$ctr/.config"
              -e "XDG_CACHE_HOME=$ctr/.cache" -e "XDG_DATA_HOME=$ctr/.local/share"
              -e "XDG_RUNTIME_DIR=$ctr/run")
}

ctr_image_exists() {
    "$CTR_ENGINE" image inspect "$1" >/dev/null 2>&1
}

# ctr_build_image DOCKERFILE CONTEXT IMAGE [STAGE [NAME=IMAGE...]]
# STAGE names the `FROM ... AS STAGE` to build (empty: the last stage);
# NAME=IMAGE pairs give the image of an earlier stage a later FROM names.
ctr_build_image() {
    local dockerfile="$1" context="$2" image="$3" stage="${4:-}"
    shift 4 2>/dev/null || shift $#
    if [[ "$(ctr_storage_driver)" == vfs ]]; then
        echo "[$CTR_PREFIX] podman stores images with the vfs driver" \
             "(storage.conf: driver = \"vfs\"); replaying $dockerfile${stage:+ stage $stage} with run + commit"
        ctr_replay_dockerfile "$dockerfile" "$context" "$image" "$stage" "$@"
        return
    fi
    local target=()
    [[ -n "$stage" ]] && target=(--target "$stage")
    "$CTR_ENGINE" build "${target[@]}" -f "$dockerfile" -t "$image" "$context"
}

# Join continuation lines and drop comments: one instruction per line.
ctr_dockerfile_lines() {
    awk '
        /^[[:space:]]*#/ && cont == "" { next }
        {
            line = $0
            if (line ~ /\\[[:space:]]*$/) { sub(/\\[[:space:]]*$/, "", line); cont = cont line " "; next }
            line = cont line; cont = ""
            if (line ~ /^[[:space:]]*$/) next
            print line
        }
        END { if (cont != "") print cont }' "$1"
}

ctr_replay_dockerfile() {
    local dockerfile="$1" context="$2" image="$3" want="$4"
    shift 4
    local -A stage_image=()
    local pair
    for pair in "$@"; do stage_image["${pair%%=*}"]="${pair#*=}"; done

    local -a lines=()
    mapfile -t lines < <(ctr_dockerfile_lines "$dockerfile")
    # Locate the wanted stage: the FROM line whose AS name matches, else the last.
    local i start=-1 end=${#lines[@]} name base
    for i in "${!lines[@]}"; do
        read -r instr rest <<<"${lines[$i]}"
        [[ "${instr^^}" == FROM ]] || continue
        if (( start >= 0 )) && [[ -n "$want" ]]; then end=$i; break; fi
        name=""
        [[ "$rest" =~ [[:space:]][Aa][Ss][[:space:]]+([^[:space:]]+)[[:space:]]*$ ]] && name="${BASH_REMATCH[1]}"
        if [[ -z "$want" || "$name" == "$want" ]]; then start=$i; fi
    done
    (( start >= 0 )) || ctr_die "no stage ${want:-<last>} in $dockerfile"
    read -r _ base _ <<<"${lines[$start]}"
    [[ -n "${stage_image[$base]:-}" ]] && base="${stage_image[$base]}"

    local work script n=0 wd=/ changes=() envs=()
    work="$(mktemp -d)"
    script="$work/replay.sh"
    echo "set -e" >"$script"
    for (( i = start + 1; i < end; i++ )); do
        local instr rest
        read -r instr rest <<<"${lines[$i]}"
        case "${instr^^}" in
            RUN)
                n=$((n + 1))
                printf '%s\n' "$rest" >"$work/run-$n.sh"
                printf 'mkdir -p %q && cd %q && /bin/sh /replay/run-%d.sh\n' "$wd" "$wd" "$n" >>"$script" ;;
            WORKDIR)
                wd="$rest"
                changes+=(--change "WORKDIR $wd") ;;
            ENV)
                local kv
                if [[ "$rest" == *=* ]]; then
                    for kv in $rest; do envs+=("$kv"); done
                else
                    read -r k v <<<"$rest"; envs+=("$k=$v")
                fi
                for kv in "${envs[@]}"; do printf 'export %q\n' "$kv" >>"$script"; done ;;
            COPY)
                local -a parts=()
                read -ra parts <<<"$rest"
                local dest="${parts[-1]}"
                unset 'parts[-1]'
                printf 'mkdir -p %q\n' "$dest" >>"$script"
                local p
                for p in "${parts[@]}"; do
                    [[ "$p" == --* ]] && ctr_die "COPY option $p is not replayed ($dockerfile)"
                    printf 'cp -a /replay-context/%q %q\n' "$p" "$dest" >>"$script"
                done ;;
            *) ctr_die "instruction ${instr^^} is not replayed under vfs ($dockerfile); build the image elsewhere or add it to ctr_replay_dockerfile" ;;
        esac
    done
    local kv
    for kv in "${envs[@]}"; do changes+=(--change "ENV $kv"); done
    local cname="openpete-replay-$$-$RANDOM"
    echo "[$CTR_PREFIX] replay: $base -> $image ($n RUN steps)"
    if ! "$CTR_ENGINE" "${CTR_ENGINE_ARGS[@]}" run --name "$cname" -v "$work:/replay:ro" -v "$(cd "$context" && pwd):/replay-context:ro" \
            "$base" /bin/sh /replay/replay.sh; then
        "$CTR_ENGINE" rm -f "$cname" >/dev/null 2>&1 || true
        rm -rf "$work"
        ctr_die "replay of $dockerfile${want:+ stage $want} failed"
    fi
    "$CTR_ENGINE" commit "${changes[@]}" "$cname" "$image"
    "$CTR_ENGINE" rm "$cname" >/dev/null
    rm -rf "$work"
}
