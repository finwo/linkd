#!/bin/sh
# Plugin subsystem: handshake, config delivery, broadcast, failure semantics.
#
# Driven by tests/fixtures/plugin-echo.sh, a plugin written in POSIX shell.
# That is deliberate: if the suite passes with a shell plugin, the claim that
# plugins need no particular language is tested rather than asserted.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${HERE}/../.." && pwd)
. "${HERE}/../helpers.sh"

echo "==> test_plugin: RESP plugin subsystem"

BUILD_DIR=$(ensure_built "${ROOT}")
BIN="${BUILD_DIR}/linkd"
FIXTURE="${ROOT}/tests/fixtures/plugin-echo.sh"
assert_path_exists "${FIXTURE}" "shell plugin fixture present"

T=$(mktemp -d)
trap 'rm -rf "${T}"' EXIT
mkdir -p "${T}/net"

# No `auto`: nothing is applied, so the daemon needs no CAP_NET_ADMIN and
# cannot modify the host. The plugin still sees real PORT/RIF/NEIGH events,
# because the startup resync dumps the interfaces that already exist -- and
# reading them is unprivileged.
cat > "${T}/net/interfaces" <<EOF
iface lo
    address 127.0.0.1/8
EOF

cat > "${T}/net/ports" <<EOF
port swp1
    speed 25000
    fec rs
    autoneg 0
EOF

# Runs linkd to completion against a config, returning its log.
run_linkd() {
  # run_linkd <config> [env assignments...]
  _cfg=$1; shift
  env "$@" PLUGIN_LOG="${T}/plugin.log" \
    timeout 15 "${BIN}" --config "${_cfg}" --resync-interval 0 \
    >"${T}/linkd.log" 2>&1 || true
  cat "${T}/linkd.log"
}

# --- handshake + config delivery -------------------------------------------
cat > "${T}/full.cnf" <<EOF
config_iface ${T}/net/interfaces
config_ports ${T}/net/ports
listen unix://${T}/linkd.sock
plugin ${FIXTURE}
    echo-setting value1 value2
EOF

: > "${T}/plugin.log"
run_linkd "${T}/full.cnf" >/dev/null

assert_file_contains "${T}/plugin.log" "COMMAND"     "plugin asked for COMMAND"
assert_file_contains "${T}/plugin.log" "INFO"        "plugin asked for INFO"
assert_file_contains "${T}/plugin.log" "CONFIG LIST" "plugin asked for CONFIG LIST"
assert_file_contains "${T}/plugin.log" "CONFIG SET echo-setting value1 value2" \
  "unknown sub-directive delivered as CONFIG SET"

# Operations reach the plugin in the documented subcommand form.
assert_file_contains "${T}/plugin.log" "PORT APPLY swp1 speed 25000 fec rs autoneg 0" \
  "ports config broadcast as PORT APPLY"
assert_file_contains "${T}/plugin.log" "PORT ADMIN lo up" "netlink link event broadcast"
assert_file_contains "${T}/plugin.log" "RIF ADD lo 127.0.0.1/8" "address event broadcast as RIF ADD"

# --- hardware_detected gates the plugin ------------------------------------
: > "${T}/plugin.log"
out=$(run_linkd "${T}/full.cnf" PLUGIN_HARDWARE=0)
assert_contains "${out}" "no supported hardware" "plugin reporting no hardware is refused"

# --- failure semantics ------------------------------------------------------
# A required plugin failing an operation fails that operation.
: > "${T}/plugin.log"
out=$(run_linkd "${T}/full.cnf" PLUGIN_FAIL_VERB=PORT)
assert_contains "${out}" "port_admin lo failed" "required plugin failure fails the operation"

# The same plugin marked optional does not.
cat > "${T}/opt.cnf" <<EOF
config_iface ${T}/net/interfaces
listen unix://${T}/linkd.sock
plugin ${FIXTURE}
    optional
EOF
: > "${T}/plugin.log"
out=$(run_linkd "${T}/opt.cnf" PLUGIN_FAIL_VERB=PORT)
case "${out}" in
  *"port_admin lo failed"*)
    echo "FAIL: optional plugin failure still failed the operation" >&2; exit 1 ;;
  *"deliberate failure"*)
    echo "PASS: optional plugin failure logged but not fatal" ;;
  *)
    echo "FAIL: optional plugin was not consulted at all" >&2; exit 1 ;;
esac

# --- no plugins configured --------------------------------------------------
cat > "${T}/none.cnf" <<EOF
config_iface ${T}/net/interfaces
listen unix://${T}/linkd.sock
EOF
out=$(run_linkd "${T}/none.cnf")
case "${out}" in
  *"ERROR"*plugin*) echo "FAIL: plugin errors without any plugin configured" >&2; exit 1 ;;
  *) echo "PASS: no plugins configured is not an error" ;;
esac

echo "==> test_plugin done"
