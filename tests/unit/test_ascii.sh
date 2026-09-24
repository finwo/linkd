#!/bin/sh
# Sources and docs stay pure ASCII. Em-dashes and curly quotes arrive via copy
# paste and show up as mojibake on a serial console with no locale.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)

echo "==> test_ascii: source and docs are pure ASCII"
cd "${ROOT}"

# Walks the tree rather than asking git: CI delivers a tarball of the commit,
# with no repository, so `git ls-files` would fail there and report nothing
# wrong.
files=$(find . \
  -path ./build -prune -o \
  -path ./.git -prune -o \
  -name lib -prune -o \
  -type f \( -name '*.c' -o -name '*.h' -o -name '*.sh' -o -name '*.md' \
             -o -name 'Makefile' -o -name '*.mk' -o -name '*.cnf' \
             -o -name '*.yml' -o -name '.dep' \) -print)

[ -n "${files}" ] || { echo "FAIL: found no files to check" >&2; exit 1; }
echo "checking $(printf '%s\n' "${files}" | wc -l) files"

bad=''
for f in ${files}; do
  hit=$(LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" 2>/dev/null | head -3 | sed "s|^|${f}:|" || true)
  [ -n "${hit}" ] && bad="${bad}${hit}
"
done

if [ -n "${bad}" ]; then
  echo "FAIL: non-ASCII bytes found:" >&2
  printf '%s' "${bad}" | head -10 >&2
  exit 1
fi
echo "PASS: no non-ASCII in sources"

echo "==> test_ascii done"
