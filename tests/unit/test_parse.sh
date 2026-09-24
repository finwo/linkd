#!/bin/sh
# Parse the real config grammar through the real parser objects.
#
# This links the actual ifaces/ports/config objects out of the build tree
# rather than re-implementing the grammar, so it fails when the parser changes
# behaviour rather than when a test fixture drifts.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_parse: ports + interfaces, source, vlan, bridge, hooks, IPv6"

BUILD_DIR=$(ensure_built "${ROOT}")

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Config lives under the temp dir, and the harness is told where via argv.
# The original version hardcoded a fixed /tmp path, which two
# concurrent runs would silently share.
CFGDIR="${TMPDIR}/etc/network"
mkdir -p "${CFGDIR}/interfaces.d" "${CFGDIR}/ports.d"

cat > "${CFGDIR}/interfaces" <<'EOF'
source interfaces.d/*.cnf
auto lo
iface lo
    address 127.0.0.1/8
    address ::1/128

auto br0
iface br0
    address 10.20.0.1/24
    bridge-ports eth0
    bridge-stp off
    pre-up echo pre-up-br0
    post-up echo post-up-br0

auto eth0.100
iface eth0.100
    address 10.100.0.1/24
    address 2001:db8:100::1/64
    vlan-raw-device eth0
EOF

cat > "${CFGDIR}/interfaces.d/dummy.cnf" <<'EOF'
iface dummy1
    address 10.99.0.1/24
    pre-up touch /tmp/pre
    post-up touch /tmp/post
EOF

cat > "${TMPDIR}/test_parse.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/config.h"
#include "config/ports.h"
#include "config/ifaces.h"

/* argv[1] = interfaces file, argv[2] = its directory (for `source`) */
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: test_parse <file> <dir>\n"); return 2; }
  ports_register();
  ifaces_register();
  FILE *f = fopen(argv[1], "r");
  if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  int rc = cfg_parse(cfg_ns_get("interfaces"), argv[2], f, NULL);
  fclose(f);
  if (rc != 0) { fprintf(stderr, "cfg_parse rc=%d\n", rc); return 1; }

  struct linkd_iface *lo   = ifaces_find("lo");
  struct linkd_iface *br0  = ifaces_find("br0");
  struct linkd_iface *vlan = ifaces_find("eth0.100");
  struct linkd_iface *dum  = ifaces_find("dummy1");

  if (!lo)          { fprintf(stderr, "lo not found\n"); return 1; }
  if (!lo->addrs)   { fprintf(stderr, "lo addrs missing\n"); return 1; }

  int addrc = 0;
  for (struct iface_addr *a = lo->addrs; a; a = a->next) addrc++;
  if (addrc < 2) { fprintf(stderr, "lo addrc %d expected 2 (IPv6 first-class)\n", addrc); return 1; }

  if (!br0)                { fprintf(stderr, "br0 not found\n"); return 1; }
  if (!br0->bridge_ports)  { fprintf(stderr, "br0 bridge-ports missing\n"); return 1; }
  if (!br0->pre_up || !br0->post_up) { fprintf(stderr, "br0 hooks missing\n"); return 1; }

  if (!vlan || vlan->vlan_id != 100) { fprintf(stderr, "vlan missing or id != 100\n"); return 1; }

  /* proves `source interfaces.d/*.cnf` was followed */
  if (!dum) { fprintf(stderr, "dummy1 not found: source directive did not load interfaces.d\n"); return 1; }

  printf("parse ok: lo addrs %d, br0 ports %s, vlan %d, sourced dummy1\n",
         addrc, br0->bridge_ports, vlan->vlan_id);
  return 0;
}
C

cc -I"${ROOT}/src" -I"${BUILD_DIR}/lib/.dep/include" -I"${BUILD_DIR}" \
  "${TMPDIR}/test_parse.c" \
  "${BUILD_DIR}/src/config/ifaces.o" \
  "${BUILD_DIR}/src/config/ports.o" \
  "${BUILD_DIR}/src/util/config.o" \
  "${BUILD_DIR}/lib/finwo/cnfparse/src/cnfparse.o" \
  "${BUILD_DIR}/lib/finwo/buf/src/buf.o" \
  "${BUILD_DIR}/lib/rxi/log/src/log.o" \
  -o "${TMPDIR}/test_parse" 2>&1 | head -20

[ -x "${TMPDIR}/test_parse" ] || { echo "FAIL: test_parse compile failed" >&2; exit 1; }

if "${TMPDIR}/test_parse" "${CFGDIR}/interfaces" "${CFGDIR}"; then
  echo "PASS: config parsing, source, vlan, bridge, hooks, IPv6"
else
  echo "FAIL: config parsing" >&2; exit 1
fi

echo "==> test_parse done"
