#!/usr/bin/env bash
# packaging/dev-image.sh - the install steps of the developer image
# (packaging/dev-image.Dockerfile), one stage per call, run as root inside
# an ubuntu:22.04 container.
#
#   dev-image.sh base          apt (snapshot.ubuntu.com), clang-19, pip
#   dev-image.sh mingw         the MinGW cross toolchain and nasm
#   dev-image.sh --list STAGE  print STAGE's apt package names, one per line
#
# Every input is pinned so a rebuild installs the same files: the apt
# sources point at snapshot.ubuntu.com at SNAPSHOT, clang-19 is installed by
# version from apt.llvm.org (whose signing key is checked against its
# fingerprint), and every pip package is pinned to the release image's
# version (packaging/linux-release.Dockerfile) or, for Pillow and numpy, the
# capture image's (tests/harness/linux-capture.Dockerfile). The package names live in
# dev-image-packages.txt beside this script, which building.md includes as
# the native apt line.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIST="$HERE/dev-image-packages.txt"

SNAPSHOT=20261001T000000Z
LLVM_VERSION="1:19.1.7~++20250114103320+cd708029e0b2-1~exp1~20250114103432.75"
LLVM_KEY_FPR=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
PIP_PINS=(
    mkdocs-material==9.7.6 babel==2.18.0 backrefs==6.2 certifi==2026.7.22
    charset-normalizer==3.5.1 click==8.1.8 colorama==0.4.6 ghp-import==2.1.0
    idna==3.19 importlib-metadata==8.7.1 jinja2==3.1.6 markdown==3.9
    markupsafe==3.0.3 mergedeep==1.3.4 mkdocs-get-deps==0.2.2
    mkdocs-material-extensions==1.3.1 mkdocs==1.6.1 packaging==26.3
    paginate==0.5.7 pathspec==1.1.1 platformdirs==4.4.0 pygments==2.21.0
    pymdown-extensions==10.21.3 python-dateutil==2.9.0.post0
    pyyaml-env-tag==1.1 pyyaml==6.0.3 requests==2.32.5 six==1.17.0
    urllib3==2.6.3 watchdog==6.0.0 zipp==3.23.1
    # The presentation-golden tooling and its fence self-tests (PNG decode,
    # the credits leg), at the capture image's pins.
    Pillow==12.0.0 numpy==2.2.6
)

# Print the apt names of one `[stage]` section of the list file.
stage_packages() {
    awk -v want="[$1]" '
        /^\[/ { cur = $0; next }
        /^[[:space:]]*(#|$)/ { next }
        cur == want { print $1 }' "$LIST"
}

apt_install() {
    apt-get install -y --no-install-recommends "$@"
}

# The snapshot is served over https only; the base image has no CA bundle,
# so ca-certificates is fetched with peer verification off. apt verifies
# every index and package against the archive's GPG signature either way.
stage_base() {
    local suite
    : >/etc/apt/sources.list
    for suite in jammy jammy-updates jammy-security; do
        echo "deb [check-valid-until=no] https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT $suite main universe" \
            >>/etc/apt/sources.list
    done
    apt-get -o Acquire::https::Verify-Peer=false update
    apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates
    apt-get update
    # shellcheck disable=SC2046
    apt_install $(stage_packages base)

    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.asc
    local fpr
    fpr="$(gpg --show-keys --with-colons /tmp/llvm.asc | awk -F: '/^fpr/ { print $10; exit }')"
    [[ "$fpr" == "$LLVM_KEY_FPR" ]] \
        || { echo "dev-image: apt.llvm.org key fingerprint $fpr, expected $LLVM_KEY_FPR" >&2; exit 1; }
    gpg --dearmor -o /usr/share/keyrings/llvm.gpg /tmp/llvm.asc
    rm /tmp/llvm.asc
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-19 main" \
        >/etc/apt/sources.list.d/llvm-19.list
    apt-get update
    apt_install "clang-19=$LLVM_VERSION" "llvm-19=$LLVM_VERSION" \
        "libclang-cpp19=$LLVM_VERSION" "libllvm19=$LLVM_VERSION" \
        "llvm-19-linker-tools=$LLVM_VERSION" "llvm-19-runtime=$LLVM_VERSION" \
        "llvm-19-tools=$LLVM_VERSION" "libclang-common-19-dev=$LLVM_VERSION"

    pip3 install --no-cache-dir "${PIP_PINS[@]}"

    # The tree is bind-mounted and owned by the host uid.
    git config --system --add safe.directory '*'
    rm -rf /var/lib/apt/lists/*
}

stage_mingw() {
    apt-get update
    # shellcheck disable=SC2046
    apt_install $(stage_packages mingw)
    rm -rf /var/lib/apt/lists/*
}

case "${1:-}" in
    base)  stage_base ;;
    mingw) stage_mingw ;;
    --list)
        [[ "${2:-}" == base || "${2:-}" == mingw ]] \
            || { echo "usage: $0 --list base|mingw" >&2; exit 64; }
        stage_packages "$2" ;;
    *) echo "usage: $0 base | mingw | --list base|mingw" >&2; exit 64 ;;
esac
