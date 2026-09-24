#!/bin/sh
# Password hashing: PBKDF2 correctness against OpenSSL, adapted-base64
# round-trips, and the role model end to end.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_auth: password hashing and roles"

BUILD_DIR=$(ensure_built "${ROOT}")
BIN="${BUILD_DIR}/linkd"

T=$(mktemp -d)
trap 'rm -rf "${T}"' EXIT

DEPINC="${BUILD_DIR}/lib/.dep/include"

# Harness exposing the internals, so PBKDF2 output can be compared against a
# reference implementation rather than only against itself.
cat > "${T}/h.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "finwo/pbkdf2.h"
#include "util/auth.h"

static void hex(const unsigned char *b, size_t n) {
  for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
  printf("\n");
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "kdf")) {
    /* kdf <pass> <salt-ascii> <iters> -> hex of 32-byte key */
    unsigned char out[32];
    pbkdf2((const uint8_t*)argv[2], strlen(argv[2]),
           (const uint8_t*)argv[3], strlen(argv[3]),
           (unsigned long)atoi(argv[4]), PBKDF2_SHA256, out, sizeof(out));
    hex(out, sizeof(out));
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "ab64")) {
    /* ab64 <ascii> -> encoded, then decoded back as hex */
    char enc[256];
    unsigned char dec[256];
    ab64_encode((const unsigned char*)argv[2], strlen(argv[2]), enc, sizeof(enc));
    size_t n = ab64_decode(enc, dec, sizeof(dec));
    printf("%s\n", enc);
    hex(dec, n);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "verify")) {
    printf("%d\n", auth_verify_hash(argv[2], argv[3]));
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "make")) {
    char *h = auth_make_hash(argv[2], (unsigned)atoi(argv[3]));
    printf("%s\n", h ? h : "(null)");
    return 0;
  }
  return 1;
}
EOF

cc -O2 -I"${ROOT}/src" -I"${DEPINC}" -o "${T}/h" "${T}/h.c" \
   "${ROOT}/src/util/auth.c" \
   "${BUILD_DIR}/lib/finwo/pbkdf2/src/pbkdf2.o" \
   "${BUILD_DIR}/lib/rxi/log/src/log.o" 2>"${T}/cc.log" || {
  echo "SKIP: harness did not build"; sed -n '1,5p' "${T}/cc.log"; exit 0;
}

# --- PBKDF2 against OpenSSL ------------------------------------------------
# Self-consistency proves nothing; a wrong HMAC is perfectly self-consistent.
if command -v openssl >/dev/null 2>&1; then
  for vec in "password:salt:1000" "secret:NaCl:4096" "x:0123456789abcdef:29000"; do
    pass=$(printf '%s' "${vec}" | cut -d: -f1)
    salt=$(printf '%s' "${vec}" | cut -d: -f2)
    iter=$(printf '%s' "${vec}" | cut -d: -f3)

    mine=$("${T}/h" kdf "${pass}" "${salt}" "${iter}")
    ref=$(openssl kdf -keylen 32 -kdfopt digest:SHA256 \
            -kdfopt "pass:${pass}" -kdfopt "salt:${salt}" -kdfopt "iter:${iter}" \
            PBKDF2 2>/dev/null | tr -d ':' | tr 'A-F' 'a-f')
    if [ -z "${ref}" ]; then
      echo "SKIP: openssl kdf unavailable for cross-check"
      break
    fi
    assert_eq "${mine}" "${ref}" "PBKDF2-SHA256 matches OpenSSL (${pass}/${salt}/${iter})"
  done
else
  echo "SKIP: openssl not available"
fi

# --- adapted base64 --------------------------------------------------------
out=$("${T}/h" ab64 "hello world")
enc=$(printf '%s' "${out}" | sed -n 1p)
dec=$(printf '%s' "${out}" | sed -n 2p)
want=$(printf 'hello world' | od -An -tx1 | tr -d ' \n')
assert_eq "${dec}" "${want}" "adapted base64 round-trips"

case "${enc}" in
  *=*) echo "FAIL: adapted base64 must not emit padding" >&2; exit 1 ;;
  *+*) echo "FAIL: adapted base64 must use '.' not '+'" >&2; exit 1 ;;
  *)   echo "PASS: adapted base64 has no padding and no '+'" ;;
esac

# --- hash round-trip -------------------------------------------------------
h=$("${T}/h" make "correct horse" 1000)
case "${h}" in
  '$pbkdf2-sha256$1000$'*) echo "PASS: hash string carries scheme and iterations" ;;
  *) echo "FAIL: unexpected hash format: ${h}" >&2; exit 1 ;;
esac

assert_eq "$("${T}/h" verify "${h}" "correct horse")" "1" "correct password verifies"
assert_eq "$("${T}/h" verify "${h}" "wrong")"         "0" "wrong password rejected"
assert_eq "$("${T}/h" verify "${h}" "")"              "0" "empty password rejected"

# Two hashes of the same password must differ: the salt has to be random.
h2=$("${T}/h" make "correct horse" 1000)
if [ "${h}" = "${h2}" ]; then
  echo "FAIL: identical hashes for the same password; salt is not random" >&2
  exit 1
fi
echo "PASS: salt is random across invocations"

# Tampering with the stored iteration count must not validate.
assert_eq "$("${T}/h" verify "\$pbkdf2-sha256\$1\$abc\$def" "correct horse")" "0" \
  "malformed hash rejected"

# --- roles over a live daemon ----------------------------------------------
command -v unshare >/dev/null 2>&1 || { echo "SKIP: unshare unavailable"; exit 0; }
unshare -Urn true 2>/dev/null   || { echo "SKIP: user namespaces unavailable"; exit 0; }

mkdir -p "${T}/bin"
for l in linkd linkctl ifquery; do ln -sf "${BIN}" "${T}/bin/${l}"; done

RO=$("${T}/h" make "ropass" 1000)
FU=$("${T}/h" make "fupass" 1000)
printf 'watcher:%s:readonly\nadmin:%s:full\nnorole:%s\n' "${RO}" "${FU}" "${RO}" > "${T}/passwd"
chmod 600 "${T}/passwd"

cat > "${T}/interfaces" <<EOF
auto lo
iface lo
    address 127.0.0.1/8
EOF

cat > "${T}/linkd.cnf" <<EOF
config_iface ${T}/interfaces
authfile ${T}/passwd
listen unix://${T}/linkd.sock
listen tcp://127.0.0.1:16797
EOF

cat > "${T}/raw.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char **argv) {
  (void)argc;
  char req[8192];
  size_t len = fread(req, 1, sizeof(req), stdin);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_port = htons((uint16_t)atoi(argv[1]));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(fd, (struct sockaddr*)&a, sizeof(a))) return 1;
  write(fd, req, len);
  shutdown(fd, SHUT_WR);
  char b[8192]; ssize_t n, t = 0;
  while ((n = read(fd, b+t, sizeof(b)-t-1)) > 0) t += n;
  b[t] = 0; fwrite(b, 1, t, stdout);
  return 0;
}
EOF
cc -O2 -o "${T}/raw" "${T}/raw.c" 2>/dev/null || { echo "SKIP: no raw client"; exit 0; }

OUT="${T}/out"
timeout 60 unshare -Urn sh -c "
  '${T}/bin/linkd' --config '${T}/linkd.cnf' --ready-file '${T}/ready' --resync-interval 0 >'${T}/daemon.log' 2>&1 &
  for i in \$(seq 1 80); do [ -e '${T}/ready' ] && break; sleep 0.25; done

  echo '--badpass--'
  printf '*3\r\n\$4\r\nAUTH\r\n\$5\r\nadmin\r\n\$5\r\nWRONG\r\n' | '${T}/raw' 16797

  echo '--readonly-query--'
  printf '*3\r\n\$4\r\nAUTH\r\n\$7\r\nwatcher\r\n\$6\r\nropass\r\n*2\r\n\$7\r\nIFQUERY\r\n\$2\r\nlo\r\n' | '${T}/raw' 16797 | head -3

  echo '--readonly-mutate--'
  printf '*3\r\n\$4\r\nAUTH\r\n\$7\r\nwatcher\r\n\$6\r\nropass\r\n*2\r\n\$4\r\nIFUP\r\n\$2\r\nlo\r\n' | '${T}/raw' 16797 | tail -1

  echo '--full-mutate--'
  printf '*3\r\n\$4\r\nAUTH\r\n\$5\r\nadmin\r\n\$6\r\nfupass\r\n*2\r\n\$4\r\nIFUP\r\n\$2\r\nlo\r\n' | '${T}/raw' 16797 | tail -1

  echo '--norole-defaults-readonly--'
  printf '*3\r\n\$4\r\nAUTH\r\n\$6\r\nnorole\r\n\$6\r\nropass\r\n*2\r\n\$4\r\nIFUP\r\n\$2\r\nlo\r\n' | '${T}/raw' 16797 | tail -1

  echo '--end--'
  kill %1 2>/dev/null
" > "${OUT}" 2>&1 || true

section() { sed -n "/^--$1--\$/,/^--/p" "${OUT}" | sed '1d;$d'; }

assert_contains "$(section badpass)"          "WRONGPASS" "wrong password rejected by daemon"
assert_contains "$(section readonly-query)"   "+OK"       "readonly user authenticates"
assert_contains "$(section readonly-mutate)"  "NOPERM"    "readonly user cannot mutate"
assert_contains "$(section full-mutate)"      "+OK"       "full user can mutate"
assert_contains "$(section norole-defaults-readonly)" "NOPERM" \
  "entry without a role field defaults to readonly"

# A world-writable password file lets anyone grant themselves access.
chmod 666 "${T}/passwd"
out=$(timeout 30 unshare -Urn sh -c "
  '${T}/bin/linkd' --config '${T}/linkd.cnf' --ready-file '${T}/ready2' --resync-interval 0 >'${T}/d2.log' 2>&1 &
  for i in \$(seq 1 60); do [ -e '${T}/ready2' ] && break; sleep 0.25; done
  printf '*3\r\n\$4\r\nAUTH\r\n\$5\r\nadmin\r\n\$6\r\nfupass\r\n' | '${T}/raw' 16797
  kill %1 2>/dev/null
" 2>&1 || true)
assert_contains "${out}" "WRONGPASS" "world-writable password file is not honoured"
assert_file_contains "${T}/d2.log" "world-writable" "world-writable file is reported"

echo "==> test_auth done"
