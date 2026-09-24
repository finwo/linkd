#!/bin/sh
# linkd is a single binary that dispatches on basename(argv[0]).
# The package ships ifup/ifdown/ifquery/ifreload/linkctl as symlinks to it, so
# dispatch breaking is indistinguishable from the commands not existing.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_multicall: argv[0] dispatch"

BUILD_DIR=$(ensure_built "${ROOT}")
BIN="${BUILD_DIR}/linkd"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

for l in ifup ifdown ifquery ifreload linkctl; do
  ln -s "$BIN" "${TMPDIR}/${l}"
done

# ifup with no daemon: either usage, or a connect failure naming linkd. Both
# prove dispatch worked; what must not happen is linkd's own daemon output.
out=$("${TMPDIR}/ifup" 2>&1 || true)
case "${out}" in
  *"usage: ifup"*)              echo "PASS: ifup dispatches (usage)" ;;
  *"Is linkd running"*)         echo "PASS: ifup dispatches (connect refused, daemon not running)" ;;
  *)
    echo "FAIL: ifup via symlink did not dispatch" >&2
    printf '%s\n' "${out}" | head -5 >&2
    exit 1 ;;
esac

# linkctl is a thin shim: it takes argv[1] and must NOT dispatch on argv[0].
out=$("${TMPDIR}/linkctl" 2>&1 || true)
assert_contains "${out}" "usage:" "linkctl shows usage with no arguments"

out=$("$BIN" --help 2>&1 || true)
assert_contains "${out}" "linkd" "linkd --help"

# Every dispatch target must be reachable from the one binary.
for l in ifup ifdown ifquery ifreload linkctl; do
  if ! strings "$BIN" | grep -qx "$l"; then
    echo "FAIL: multicall binary has no '${l}' dispatch entry" >&2
    exit 1
  fi
done
echo "PASS: all five dispatch targets present in the binary"

echo "==> test_multicall done"
