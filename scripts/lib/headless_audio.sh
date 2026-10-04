# headless_audio.sh - the OpenAL backend of unattended headless runs.
# SOURCED, not executed.
#
# OpenAL Soft opens its device on the first backend in ALSOFT_DRIVERS that
# initialises (unset: every compiled backend in the library's order; the
# 1.19.1 Linux build tries jack, pulse, then alsa). A headless run plays to
# no listener, yet without this its device open depends on the host's audio
# server: when the PulseAudio server is absent or accepts no connection the
# pulse backend fails, the ALSA backend finds no card (WSL has none) and the
# open fails with ALC_OUT_OF_MEMORY, the code OpenAL Soft's ALSA backend
# sets on a failed card open. The null backend opens a device with no
# server, so every run takes the same device-open path on every host. The
# OPENPETE_AUDIO_DUMP capture renders through a loopback device, which no
# backend choice changes.
#
# HEADLESS_AUDIO_ENV holds the assignment for an `env` prefix. An
# ALSOFT_DRIVERS the caller already set is kept, so a run that measures
# audio on a real device chooses it explicitly (ALSOFT_DRIVERS=pulse).
# Never applied to a windowed launch or to anything shipped.
#
# Usage:
#   source scripts/lib/headless_audio.sh
#   env "${HEADLESS_AUDIO_ENV[@]}" "$BIN" --headless ...
HEADLESS_AUDIO_ENV=("ALSOFT_DRIVERS=${ALSOFT_DRIVERS:-null}")
