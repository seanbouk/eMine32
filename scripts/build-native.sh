#!/usr/bin/env bash
# Build eMine32 (EDuke32 fork) for Windows with the llvm-mingw toolchain.
# USE_MIMALLOC=0  - mimalloc's malloc overrides clash with libc++
# HAVE_FLAC=0     - the bundled prebuilt libFLAC.a needs the old msvcrt (__iob_func)
# STARTUP_WINDOW=0 - skip the launcher dialog; go straight into the game
set -e
cd "$(dirname "$0")/../engine"
make -j"$(nproc)" duke3d RELEASE=1 USE_MIMALLOC=0 HAVE_FLAC=0 STARTUP_WINDOW=0 "$@"
