#ifndef __LINKD_DHCP_H__
#define __LINKD_DHCP_H__

// Spawns `dhcp_client`, substituting {{interface}} per argv element, so no
// shell. No client is known by name: whoever ships one ships the config for
// it. The lease is applied by the client, not linkd.

// Idempotent. -1 when unconfigured or the spawn failed, which fails the ifup.
int dhcp_start(const char *ifname);

void dhcp_stop(const char *ifname);
void dhcp_stop_all(void);

// Non-blocking sweep; an unexpected exit is logged at error level.
void dhcp_reap(void);

#endif // __LINKD_DHCP_H__
