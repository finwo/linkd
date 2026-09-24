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

# --- 2. accepted connections must be non-blocking -------------------------
# This is the inverse of what it once asserted. Under the line protocol the
# server read a connection to EOF with stdio, so a non-blocking fd returned
# EAGAIN, stdio reported EOF, and the reply came back empty.
#
# RESP is length-prefixed, so a partial read is detectable rather than
# indistinguishable from end-of-input: resp_read_buf reports "incomplete" and
# the connection waits for more data. Blocking accepts would now let one
# client stall netlink for the whole daemon.
IPC="${SRC}/ipc.c"
assert_path_exists "${IPC}" "ipc.c present"
if ! grep -q 'accept4(.*SOCK_NONBLOCK' "${IPC}"; then
  echo "FAIL: accepted connections are blocking; one slow client stalls netlink" >&2
  exit 1
fi
echo "PASS: accepted connections are non-blocking"

# Non-blocking is only correct with incremental parsing behind it.
if ! grep -q 'resp_read_buf' "${IPC}"; then
  echo "FAIL: no resp_read_buf; non-blocking reads need incremental parsing" >&2
  exit 1
fi
echo "PASS: reads are buffered through resp_read_buf"

# Partial writes must be buffered too, or a slow reader stalls the daemon
# just as effectively as a slow writer would.
if ! grep -q 'POLLOUT' "${IPC}"; then
  echo "FAIL: no POLLOUT handling; a slow reader would block the daemon" >&2
  exit 1
fi
echo "PASS: writes are buffered and drained on POLLOUT"

# --- 3. one client must not be able to wedge the daemon -------------------
# The old server serviced a single connection per poll wakeup, synchronously.
if grep -q 'IPC_MAX_CONNS' "${SRC}/ipc.h"; then
  echo "PASS: server keeps a bounded connection table"
else
  echo "FAIL: no connection limit; an unbounded fd table is a DoS" >&2
  exit 1
fi

echo "==> test_netlink done"
