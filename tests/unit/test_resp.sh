#!/bin/sh
# RESP server: protocol, pipelining, concurrency and the authorization model.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_resp: RESP server"

BUILD_DIR=$(ensure_built "${ROOT}")
BIN="${BUILD_DIR}/linkd"

command -v unshare >/dev/null 2>&1 || { echo "SKIP: unshare unavailable"; exit 0; }
unshare -Urn true 2>/dev/null   || { echo "SKIP: user namespaces unavailable"; exit 0; }

T=$(mktemp -d)
trap 'rm -rf "${T}"' EXIT
mkdir -p "${T}/bin"

for l in linkd linkctl ifup ifdown ifquery ifreload; do ln -sf "${BIN}" "${T}/bin/${l}"; done

cat > "${T}/interfaces" <<EOF
auto lo
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
: > "${T}/linkd.passwd"

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
timeout 60 unshare -Urn sh -c "
  '${T}/bin/linkd' --config '${T}/linkd.cnf' --ready-file '${T}/ready' --resync-interval 0 >'${T}/daemon.log' 2>&1 &
  for i in \$(seq 1 80); do [ -e '${T}/ready' ] && break; sleep 0.25; done
  export LINKD_CONFIG='${T}/linkd.cnf'

  echo '--ping--';     '${T}/bin/linkctl' PING
  echo '--echo--';     '${T}/bin/linkctl' PING hello
  echo '--query--';    '${T}/bin/ifquery' lo
  echo '--unknown--';  '${T}/bin/linkctl' FROBNICATE 2>&1 || true
  echo '--missing--';  '${T}/bin/ifquery' nosuch 2>&1 || true
  echo '--arity--';    '${T}/bin/linkctl' IFUP 2>&1 || true
  echo '--mutate--';   '${T}/bin/ifup' lo

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

  kill %1 2>/dev/null
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
assert_contains "$(section mutate)"  "OK"                 "root over unix may mutate"

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
out=$(timeout 15 unshare -Urn "${BIN}" --config "${T}/noauth.cnf" 2>&1 || true)
assert_contains "${out}" "refusing to listen" "tcp without authfile is refused"

echo "==> test_resp done"
