#!/bin/sh
# Stands in for a DHCP client. Records the argv linkd rendered, then exits,
# which also exercises the reap path.
set -eu
printf '%s\n' "$*" >> "${DHCP_LOG:?DHCP_LOG unset}"
exit 0
