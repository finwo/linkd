#!/bin/sh
# A linkd plugin in POSIX shell: RESP over stdin/stdout.
#
# Exists to prove a plugin needs no C, no library and no linking -- and to give
# the test suite something to drive. It logs what it receives to
# $PLUGIN_LOG so tests can assert on the commands linkd actually sent.
#
# Only the subset of RESP linkd sends is parsed: arrays of bulk strings.

LOG="${PLUGIN_LOG:-/dev/null}"
HARDWARE="${PLUGIN_HARDWARE:-1}"
FAIL_VERB="${PLUGIN_FAIL_VERB:-}"

# RESP replies. printf with \r\n; the protocol is CRLF-delimited.
ok()    { printf '+OK\r\n'; }
err()   { printf -- '-ERR %s\r\n' "$1"; }
bulk()  { printf '$%s\r\n%s\r\n' "${#1}" "$1"; }

array() {
  printf '*%s\r\n' "$#"
  for a in "$@"; do printf '$%s\r\n%s\r\n' "${#a}" "$a"; done
}

# Read one RESP array into $ARGV (newline-separated). Returns 1 at EOF.
read_command() {
  ARGV=''
  read -r line || return 1
  line=${line%$(printf '\r')}
  case "$line" in
    '*'*) count=${line#\*} ;;
    *) return 1 ;;
  esac

  i=0
  while [ "$i" -lt "$count" ]; do
    read -r hdr || return 1        # $<len>
    read -r val || return 1        # payload
    val=${val%$(printf '\r')}
    ARGV="${ARGV}${val}
"
    i=$((i + 1))
  done
  return 0
}

while read_command; do
  verb=$(printf '%s' "$ARGV" | sed -n '1p' | tr '[:lower:]' '[:upper:]')
  sub=$(printf '%s' "$ARGV"  | sed -n '2p' | tr '[:lower:]' '[:upper:]')
  printf '%s' "$ARGV" | tr '\n' ' ' | sed 's/ *$//' >> "$LOG"
  printf '\n' >> "$LOG"

  if [ -n "$FAIL_VERB" ] && [ "$verb" = "$FAIL_VERB" ]; then
    err "deliberate failure"
    continue
  fi

  case "$verb" in
    COMMAND) array PORT RIF NEIGH ROUTE RESYNC CONFIG INFO ;;
    INFO)    bulk "# Server
name:echo
version:0.1.0

# Stats
hardware_detected:${HARDWARE}
" ;;
    CONFIG)
      case "$sub" in
        LIST) array echo-setting echo-other ;;
        SET)  ok ;;
        *)    err "unknown CONFIG subcommand" ;;
      esac
      ;;
    PORT|RIF|NEIGH|ROUTE|RESYNC) ok ;;
    AUTH) ok ;;
    *)    err "unknown command $verb" ;;
  esac
done
