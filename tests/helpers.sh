#!/bin/sh
# tests/helpers.sh - shared assertions, source with `. tests/helpers.sh`
#
# Assertions only. linkd is a standalone daemon and its tests build and run it
# directly, so none of the QEMU/SSH plumbing from the OS-assembly repo belongs
# here -- anything needing a booted system is an integration test and lives in
# a consuming OS repo.
set -eu

assert_eq() {
  # assert_eq <got> <expected> <msg>
  if [ "$1" != "$2" ]; then
    echo "FAIL: $3: expected '$2', got '$1'" >&2
    return 1
  fi
  echo "PASS: $3"
}

assert_contains() {
  # assert_contains <haystack> <needle> <msg>
  case "$1" in
    *"$2"*) echo "PASS: $3" ;;
    *) echo "FAIL: $3: expected to contain '$2', got '$1'" >&2; return 1 ;;
  esac
}

assert_file_contains() {
  # assert_file_contains <file> <needle> <msg>
  # `--` matters: without it a needle starting with '-' (any command-line flag)
  # is parsed by grep as options and never matches.
  if grep -qF -- "$2" "$1" 2>/dev/null; then
    echo "PASS: $3"
  else
    echo "FAIL: $3: $1 does not contain '$2'" >&2
    return 1
  fi
}

# Build linkd once, on demand. Every test needs the objects, and rebuilding per
# test would dominate the runtime.
ensure_built() {
  # ensure_built <repo-root>
  _root=$1
  _bd="${_root}/build/${LINKD_TARGET:-linux-glibc-amd64}"
  if [ ! -x "${_bd}/linkd" ]; then
    echo "building linkd" >&2
    make -C "${_root}" -j"$(nproc)" >/dev/null
  fi
  [ -x "${_bd}/linkd" ] || { echo "FAIL: linkd did not build" >&2; return 1; }
  printf '%s\n' "${_bd}"
}

# Guard against greps that silently stop matching. A pattern that finds nothing
# because the file moved is indistinguishable from one that finds nothing
# because the code is correct, and the second is the only one we want to pass.
assert_path_exists() {
  # assert_path_exists <path> <msg>
  if [ -e "$1" ]; then
    echo "PASS: $2"
  else
    echo "FAIL: $2: no such path: $1" >&2
    return 1
  fi
}
