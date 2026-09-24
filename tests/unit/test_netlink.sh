#!/bin/sh
# Regression guards for the three defects that made the integration suite flaky.
#
# Every check here greps the source tree, which means a check can stop finding
# anything for two very different reasons: the code is correct, or the file
# moved. Only the first should pass, so each grep is preceded by an explicit
# existence assertion. Without that, this whole file degrades into a silent
# pass the moment the layout changes -- which is exactly what would have
# happened when these tests moved into their own repo.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_netlink: no ip shell-out, blocking accept, client half-close"

SRC="${ROOT}/src"
assert_path_exists "${SRC}" "source tree present"

# --- 1. linkd must never shell out to `ip` --------------------------------
# All link/addr/route/vlan/bridge/VRF work goes through src/netlink/rtnl.c. A
# system("ip ...") reintroduces both the PATH dependency and the asynchronous
# behaviour that made tests need sleeps.
#
# Strip the grep file:line: prefix before deciding whether a hit is a comment,
# otherwise every match looks like code and the check cries wolf.
code_hits() {
  grep -rn "$1" "${SRC}" 2>/dev/null \
    | sed 's/^[^:]*:[0-9]*://' \
    | sed 's/^[[:space:]]*//' \
    | grep -v '^//' \
    | grep -v '^\*' || true
}

if [ -n "$(code_hits 'system(.*"[^"]*ip ')" ]; then
  echo "FAIL: linkd shells out to ip:" >&2
  code_hits 'system(.*"[^"]*ip ' | head -5 >&2
  exit 1
fi
echo "PASS: no ip shell-out in linkd sources"

if [ -n "$(code_hits '/sbin/ip')" ]; then
  echo "FAIL: /sbin/ip referenced in linkd sources" >&2
  code_hits '/sbin/ip' | head -5 >&2
  exit 1
fi
echo "PASS: no /sbin/ip reference in linkd sources"

# Prove the grep above is actually looking at real code. If rtnl.c ever stops
# containing netlink sends, the two checks above are meaningless.
assert_path_exists "${SRC}/netlink/rtnl.c" "rtnl.c present (the netlink path exists)"

# --- 2. the accepted IPC connection must be blocking ----------------------
# A non-blocking accepted fd made the server read EAGAIN, stdio report EOF, and
# cfg_parse see an empty stream -> empty reply. That was the root cause of the
# IPC flakiness, not timing.
IPC="${SRC}/ipc.c"
assert_path_exists "${IPC}" "ipc.c present"
if grep -q 'accept4(ipc_sock.*SOCK_NONBLOCK' "${IPC}"; then
  echo "FAIL: accepted IPC conn is non-blocking (reintroduces empty-reply race)" >&2
  exit 1
fi
echo "PASS: accepted IPC connection is blocking"

# --- 3. clients must half-close after writing -----------------------------
# Without shutdown(SHUT_WR) the server blocks waiting for a second command
# until its receive timeout expires.
for c in ifup ifdown ifquery ifreload linkctl; do
  f="${SRC}/cli/${c}.c"
  assert_path_exists "${f}" "cli/${c}.c present"
  if ! grep -q 'shutdown(sock, SHUT_WR)' "${f}"; then
    echo "FAIL: ${c} does not shutdown(SHUT_WR) after write" >&2
    exit 1
  fi
done
echo "PASS: all clients half-close after sending the command"

echo "==> test_netlink done"
