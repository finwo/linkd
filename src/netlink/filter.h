#ifndef __UNOS_NETLINK_FILTER_H__
#define __UNOS_NETLINK_FILTER_H__

#include <stdbool.h>
#include <stdint.h>

// Single policy point for what the mirror replicates into the dataplane.
//
// v1 policy: accept-all-except-LOCAL at the route-type level, intersected with
// the tracked-table set so a management VRF never leaks into the ASIC. Both
// the event path and the dump path call into here -- there is no second copy
// of the policy. A future config (allow/deny lists) only extends this file's
// backing store.

void nl_filter_reset(void);

// Called on VRF-master discovery / removal (RTM_NEWLINK / DELLINK).
void nl_filter_track(uint32_t table);
void nl_filter_untrack(uint32_t table);

// Hard deny regardless of tracking. Used for the management VRF.
void nl_filter_exclude(uint32_t table);

// True when ifname looks like a management VRF (placeholder until the
// exclusion is config-driven; verified name lands here).
bool nl_filter_mgmt_name(const char *ifname);

bool nl_table_allowed(uint32_t table);
bool nl_route_allowed(int family, uint8_t type, uint8_t scope, uint32_t table);

#endif // __UNOS_NETLINK_FILTER_H__
