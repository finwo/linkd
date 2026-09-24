#!/bin/sh
# tests/run.sh - run the linkd test suite.
#
#   ./tests/run.sh            everything
#   ./tests/run.sh --unit     same (only unit tests exist here)
#
# linkd's own tests never boot a machine. Anything that needs a running system
# -- bridges, VLANs, VRFs, addressing against a real kernel -- is an
# integration test and lives in the consuming OS repo, because it needs a
# rootfs and a VM that this repo has no business building.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)

fail=0
run_one() {
  echo
  echo "=== $1 ==="
  if sh "$1"; then
    echo "--- PASS $1"
  else
    echo "--- FAIL $1" >&2
    fail=1
  fi
}

for t in "${HERE}"/unit/*.sh; do
  [ -e "$t" ] || continue
  run_one "$t"
done

echo
if [ "$fail" = "0" ]; then
  echo "ALL TESTS PASSED"
else
  echo "SOME TESTS FAILED" >&2
fi
exit "$fail"
