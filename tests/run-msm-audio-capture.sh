#!/bin/sh
# Build the real capture parser and ioctl glue against synthetic ARM32 buffers.
set -eu
if [ "$#" -ne 1 ]; then
    printf '%s\n' 'usage: run-msm-audio-capture.sh ABSOLUTE_OUTPUT_DIRECTORY' >&2
    exit 2
fi
case "$1" in /*) ;; *) printf '%s\n' 'output directory must be absolute' >&2; exit 2 ;; esac
source_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_directory=$1
mkdir -p "$output_directory"
: "${CC:=cc}"
"$CC" --version > "$output_directory/compiler-version.txt"
set -- -std=c11 -Wall -Wextra -Werror -g
if [ "${SANITIZE:-0}" = 1 ]; then
    set -- "$@" -fsanitize=address,undefined -fno-omit-frame-pointer
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
    UBSAN_OPTIONS=halt_on_error=1
    export ASAN_OPTIONS UBSAN_OPTIONS
fi
"$CC" "$@" -I "$source_directory" "$source_directory/tests/msm_audio_capture_test.c" \
    "$source_directory/msm_audio_capture.c" -o "$output_directory/core"
"$output_directory/core"
"$CC" "$@" -DARM -DMSM_AUDIO_GLUE_TEST -I "$source_directory" \
    "$source_directory/tests/msm_audio_glue_test.c" "$source_directory/msm_audio.c" \
    "$source_directory/msm_audio_capture.c" -o "$output_directory/glue"
"$output_directory/glue"
"$CC" "$@" -DMSM_AUDIO_GLUE_TEST -I "$source_directory" -c \
    "$source_directory/msm_audio.c" -o "$output_directory/non-arm.o"
