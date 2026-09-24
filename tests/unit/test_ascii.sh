#!/bin/sh
# Sources and docs stay pure ASCII.
#
# Typographic characters (em-dashes, curly quotes) arrive invisibly via copy
# paste and editor autocorrect, then show up as mojibake on a serial console
# with no locale. Checking untracked files too is deliberate: the usual way
# this regresses is a file that has not been committed yet.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)

echo "==> test_ascii: source and docs are pure ASCII"
cd "${ROOT}"

check() {
  # check <label> <file-list-command>
  bad=$(eval "$2" | while read -r f; do
    [ -f "$f" ] || continue
    case "$f" in build/*|*.png|*.gz|*.img|*.bin) continue ;; esac
    if LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" 2>/dev/null | head -3 | sed "s|^|${f}:|"; then :; fi
  done)
  if [ -n "${bad}" ]; then
    echo "FAIL: non-ASCII bytes in $1:" >&2
    printf '%s\n' "${bad}" | head -10 >&2
    return 1
  fi
  echo "PASS: no non-ASCII in $1"
}

check "tracked sources" "git ls-files"
check "untracked sources" "git ls-files --others --exclude-standard"

echo "==> test_ascii done"
