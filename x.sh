#!/usr/bin/env bash
# x.sh -- project driver. Commands: build [debug|release], run [...], clean.
set -eu
cd "$(dirname "$0")"

# cc resolves to gcc on Linux and clang on macOS; override with CC=... if needed
CC="${CC:-cc}"
# _DEFAULT_SOURCE: strict c11 hides POSIX/BSD prototypes (mmap's MAP_ANONYMOUS,
# madvise, ...) on glibc; macOS ignores it harmlessly. Must be a build flag,
# not a #define in a .c file: in a unity build the first system header is
# included long before the os backend appears, and feature macros lock at that
# point.
# the -Werror= set promotes C's historically-tolerated type holes to hard
# errors: implicit declarations, pointer type mismatches, int<->pointer mixing
COMMON="-std=c11 -D_DEFAULT_SOURCE -Isrc -Wall -Wextra -Wundef -Wno-unused-function"
COMMON="$COMMON -Werror=implicit-function-declaration -Werror=implicit-int"
COMMON="$COMMON -Werror=incompatible-pointer-types -Werror=int-conversion"
COMMON="$COMMON -Werror=pointer-sign -Werror=return-type"
DEBUG="-g -O0 -DBUILD_DEBUG=1"
RELEASE="-g -O2 -DBUILD_DEBUG=0"

# system libraries per OS: everything we wrote rides in the unity TU; only
# these link.
case "$(uname -s)" in
  Linux)  LIBS="-lX11" ;;
  Darwin) LIBS="-framework Cocoa -framework OpenGL" ;;
  *)      LIBS="" ;;
esac

cmd_build() {
  local mode="${1:-debug}" flags
  case "$mode" in
    debug)   flags="$COMMON $DEBUG" ;;
    release) flags="$COMMON $RELEASE" ;;
    *) echo "usage: $0 build [debug|release]" >&2; exit 1 ;;
  esac
  mkdir -p build
  # shellcheck disable=SC2086
  case "$(uname -s)" in
    Darwin)
      # mac/cocoa.m is the one TU outside the unity build: Cocoa wants ObjC,
      # so it lives behind the C API in mac/cocoa.h and links in here
      $CC $flags -c src/gfx/mac/cocoa.m -o build/cocoa.o
      $CC $flags src/main.c build/cocoa.o -o build/app $LIBS
      ;;
    *)
      $CC $flags src/main.c -o build/app $LIBS
      ;;
  esac
  echo "built build/app ($mode)"
}

cmd_run() {
  # unity build is fast enough to always rebuild before running
  local mode=debug
  case "${1:-}" in
    --release) mode=release; shift ;;
    --debug)   mode=debug; shift ;;
  esac
  cmd_build "$mode"
  ./build/app "$@"
}

cmd_clean() {
  rm -rf build
  echo "removed build/"
}

cmd="${1:-build}"
shift || true
case "$cmd" in
  build) cmd_build "$@" ;;
  run)   cmd_run "$@" ;;
  clean) cmd_clean ;;
  *) echo "usage: $0 {build [debug|release] | run [args...] | clean}" >&2; exit 1 ;;
esac
