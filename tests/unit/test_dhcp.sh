#!/bin/sh
# `method dhcp`: command rendering, and the drop-in config fallback.
#
# PATH is pinned to an empty directory so nothing can reach a real dhcp client
# on the developer's host.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_dhcp: method dhcp"

BUILD_DIR=$(ensure_built "${ROOT}")
BIN="${BUILD_DIR}/linkd"
FIXTURE="${ROOT}/tests/fixtures/dhcp-fake.sh"
assert_path_exists "${FIXTURE}" "fake dhcp client fixture present"

T=$(mktemp -d)
trap 'rm -rf "${T}"' EXIT
mkdir -p "${T}/net" "${T}/emptybin"

# `auto` or nothing is applied. swp-test does not exist, so the link-up fails
# and is logged; dhcp_start still runs, which is what is under test.
cat > "${T}/net/interfaces" <<EOF
auto swp-test
iface swp-test inet dhcp

auto swp-static
iface swp-static inet static
    address 10.9.9.9/24
EOF

# An empty netns, because reaping is driven by SIGCHLD waking poll(2): on a
# host with ambient netlink traffic the loop spins anyway and a broken reap
# still passes. 3s for the same reason -- the wake is causal, not timed.
if unshare --user --map-root-user --net true 2>/dev/null; then
  in_ns() { unshare --user --map-root-user --net "$@"; }
else
  in_ns() { "$@"; }
fi

run_linkd() {
  in_ns timeout 3 env PATH="${T}/emptybin" DHCP_LOG="${T}/dhcp.log" \
    "${BIN}" --config "$1" --resync-interval 0 \
    >"${T}/linkd.log" 2>&1 || true
  cat "${T}/linkd.log"
}

# --- rendering --------------------------------------------------------------
cat > "${T}/render.cnf" <<EOF
config_iface ${T}/net/interfaces
config_ports /nonexistent
listen unix://${T}/linkd.sock
dhcp_client ${FIXTURE} --dev {{interface}} --pair {{interface}}:{{interface}} --literal plain
EOF

: > "${T}/dhcp.log"
run_linkd "${T}/render.cnf" >/dev/null

assert_file_contains "${T}/dhcp.log" "--dev swp-test" \
  "{{interface}} renders the interface name"
assert_file_contains "${T}/dhcp.log" "--pair swp-test:swp-test" \
  "every occurrence in one argument is substituted"
assert_file_contains "${T}/dhcp.log" "--literal plain" \
  "non-template arguments pass through"

n=$(wc -l < "${T}/dhcp.log" | tr -d ' ')
assert_eq "${n}" "1" "only the dhcp interface spawns a client"

out=$(cat "${T}/linkd.log")
assert_contains "${out}" "lease will not be renewed" \
  "a client that exits is reaped and reported"

# --- nothing configured -----------------------------------------------------
cat > "${T}/noclient.cnf" <<EOF
config_iface ${T}/net/interfaces
config_ports /nonexistent
listen unix://${T}/linkd.sock
EOF

out=$(run_linkd "${T}/noclient.cnf")
assert_contains "${out}" "no dhcp_client configured" \
  "an unconfigured dhcp interface is an error, not a silent no-op"

# --- drop-in supplies the client --------------------------------------------
# Real fallback path, with /etc bind-mounted in a user namespace so the
# hardcoded /etc/linkd is exercised rather than a stand-in. No --config: the
# fallback only fires when the path was not given explicitly.
mkdir -p "${T}/etc/linkd"
cat > "${T}/etc/linkd/udhcpc.cnf" <<EOF
config_iface ${T}/net/interfaces
config_ports /nonexistent
listen unix://${T}/linkd.sock
dhcp_client ${FIXTURE} --from-dropin {{interface}}
EOF

if unshare --user --map-root-user --mount true 2>/dev/null; then
  : > "${T}/dhcp.log"
  unshare --user --map-root-user --mount sh -c "
    mount --bind '${T}/etc' /etc || exit 1
    timeout 15 env PATH='${T}/emptybin' DHCP_LOG='${T}/dhcp.log' \
      '${BIN}' --resync-interval 0
  " >"${T}/linkd.log" 2>&1 || true

  assert_file_contains "${T}/dhcp.log" "--from-dropin swp-test" \
    "/etc/linkd/*.cnf is read when /etc/linkd.cnf is absent"
else
  echo "SKIP: user namespaces unavailable, cannot bind-mount /etc"
fi

# --- unresolvable command ---------------------------------------------------
cat > "${T}/missing.cnf" <<EOF
config_iface ${T}/net/interfaces
config_ports /nonexistent
listen unix://${T}/linkd.sock
dhcp_client ${T}/emptybin/definitely-not-here {{interface}}
EOF

out=$(run_linkd "${T}/missing.cnf")
assert_contains "${out}" "not found or not executable" \
  "a configured client that does not exist fails loudly"

echo "==> test_dhcp done"
