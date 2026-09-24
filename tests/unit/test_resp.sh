#!/bin/sh
# RESP server: protocol, pipelining, concurrency and the authorization model.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_resp: RESP server"

BUILD_DIR=$(ensure_built "${ROOT}")
BIN="${BUILD_DIR}/linkd"

T=$(mktemp -d)
# Kill the daemon by pid on the way out. `kill %1` does not work here: job
# control is off in a non-interactive shell, so a failed test used to leave
# the daemon running and holding its port.
cleanup() {
  # `|| true` throughout: the daemon is normally already gone, and a failing
  # kill under `set -e` would abort the trap and fail an otherwise green run.
  if [ -f "${T}/daemon.pid" ]; then kill "$(cat "${T}/daemon.pid")" 2>/dev/null || true; fi
  rm -rf "${T}" || true
}
trap cleanup EXIT
mkdir -p "${T}/bin"

for l in linkd linkctl ifup ifdown ifquery ifreload; do ln -sf "${BIN}" "${T}/bin/${l}"; done

# No `auto`: the daemon applies nothing, so it needs no privileges and cannot
# touch the host's network.
cat > "${T}/interfaces" <<EOF
iface lo
    address 127.0.0.1/8
    address ::1/128
EOF

cat > "${T}/linkd.cnf" <<EOF
config_iface ${T}/interfaces
authfile ${T}/linkd.passwd
listen unix://${T}/linkd.sock
listen tcp://127.0.0.1:16793
EOF

# The suite may run as any uid, so it cannot rely on SO_PEERCRED granting
# access; it authenticates explicitly instead.
HASH=$(printf 'testpass\n' | "${T}/bin/linkctl" hash)
printf 'tester:%s:full\n' "${HASH}" > "${T}/linkd.passwd"
chmod 600 "${T}/linkd.passwd"

# Request comes from stdin, not argv: shell quoting mangles CRLF escapes.
cat > "${T}/raw.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char **argv) {
  (void)argc;
  char req[65536];
  size_t len = fread(req, 1, sizeof(req), stdin);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
  strncpy(a.sun_path, argv[1], sizeof(a.sun_path)-1);
  if (connect(fd, (struct sockaddr*)&a, sizeof(a))) return 1;
  write(fd, req, len);
  shutdown(fd, SHUT_WR);
  char b[65536]; ssize_t n, t = 0;
  while ((n = read(fd, b+t, sizeof(b)-t-1)) > 0) t += n;
  b[t] = 0; fwrite(b, 1, t, stdout);
  return 0;
}
EOF
cc -O2 -o "${T}/raw" "${T}/raw.c" 2>/dev/null || { echo "SKIP: no compiler for raw client"; exit 0; }

OUT="${T}/out"
timeout 60 sh -c "
  '${T}/bin/linkd' --config '${T}/linkd.cnf' --ready-file '${T}/ready' --resync-interval 0 >'${T}/daemon.log' 2>&1 &
  DPID=\$!
  echo \$DPID > '${T}/daemon.pid'
  for i in \$(seq 1 80); do [ -e '${T}/ready' ] && break; sleep 0.25; done
  export LINKD_CONFIG='${T}/linkd.cnf'
  export LINKD_AUTH='tester:testpass'

  echo '--ping--';     '${T}/bin/linkctl' PING
  echo '--echo--';     '${T}/bin/linkctl' PING hello
  echo '--query--';    '${T}/bin/ifquery' lo
  echo '--unknown--';  '${T}/bin/linkctl' FROBNICATE 2>&1 || true
  echo '--missing--';  '${T}/bin/ifquery' nosuch 2>&1 || true
  echo '--arity--';    '${T}/bin/linkctl' IFUP 2>&1 || true
  echo '--mutate--';   '${T}/bin/ifup' lo 2>&1 || true
  echo '--peercred--'; env -u LINKD_AUTH '${T}/bin/ifup' lo 2>&1 || true

  echo '--pipeline--'
  printf '*1\r\n\$4\r\nPING\r\n*2\r\n\$4\r\nPING\r\n\$3\r\ntwo\r\n*1\r\n\$4\r\nPING\r\n' \
    | '${T}/raw' '${T}/linkd.sock'

  echo '--concurrent--'
  pids=''
  for i in \$(seq 1 8); do '${T}/bin/linkctl' PING & pids=\"\$pids \$!\"; done
  for p in \$pids; do wait \$p; done

  echo '--tcp-ping--'
  printf '*1\r\n\$4\r\nPING\r\n' | timeout 5 nc 127.0.0.1 16793 2>/dev/null | head -1
  echo '--tcp-noauth--'
  printf '*2\r\n\$7\r\nIFQUERY\r\n\$2\r\nlo\r\n' | timeout 5 nc 127.0.0.1 16793 2>/dev/null | head -1
  echo '--end--'

  kill \$DPID 2>/dev/null; wait \$DPID 2>/dev/null
" > "${OUT}" 2>&1 || true

section() { sed -n "/^--$1--\$/,/^--/p" "${OUT}" | sed '1d;$d'; }

assert_contains "$(section ping)"   "PONG"  "PING replies PONG"
assert_contains "$(section echo)"   "hello" "PING <msg> echoes it"
assert_contains "$(section query)"  "iface lo"          "IFQUERY renders as config syntax"
assert_contains "$(section query)"  "address 127.0.0.1/8" "IFQUERY returns addresses"
assert_contains "$(section query)"  "address ::1/128"     "IFQUERY returns IPv6 addresses"
assert_contains "$(section unknown)" "unknown command"    "unknown command rejected"
assert_contains "$(section missing)" "no such interface"  "missing interface rejected"
assert_contains "$(section arity)"   "wrong number of arguments" "arity is enforced"

# Authenticated: the command must get past authorization. Whether netlink then
# succeeds depends on privileges, which is not what this asserts.
case "$(section mutate)" in
  *NOAUTH*|*NOPERM*)
    echo "FAIL: authenticated client denied: $(section mutate)" >&2; exit 1 ;;
  *) echo "PASS: authenticated client is authorized to mutate" ;;
esac

# Unauthenticated: SO_PEERCRED alone decides, so the answer depends on uid.
# Both directions matter -- root must get in without credentials, others must
# not.
peercred=$(section peercred)
if [ "$(id -u)" = "0" ]; then
  case "${peercred}" in
    *NOAUTH*|*NOPERM*)
      echo "FAIL: root over unix was not pre-authenticated: ${peercred}" >&2; exit 1 ;;
    *) echo "PASS: root over unix is pre-authenticated" ;;
  esac
else
  assert_contains "${peercred}" "NOAUTH" "non-root over unix is not pre-authenticated"
fi

pipeline=$(section pipeline)
n=$(printf '%s' "${pipeline}" | grep -c 'PONG' || true)
assert_eq "${n}" "2" "pipelined PINGs answered (2 bare PONG + 1 echo)"
assert_contains "${pipeline}" "two" "pipelined echo answered in order"

n=$(section concurrent | grep -c 'PONG' || true)
assert_eq "${n}" "8" "8 concurrent clients all served"

assert_contains "$(section tcp-ping)"   "PONG"   "PING is allowed before AUTH"
assert_contains "$(section tcp-noauth)" "NOAUTH" "other commands require AUTH over tcp"

cat > "${T}/noauth.cnf" <<EOF
config_iface ${T}/interfaces
listen unix://${T}/linkd2.sock
listen tcp://127.0.0.1:16794
EOF
out=$(timeout 15 "${BIN}" --config "${T}/noauth.cnf" 2>&1 || true)
assert_contains "${out}" "refusing to listen" "tcp without authfile is refused"

echo "==> test_resp done"
