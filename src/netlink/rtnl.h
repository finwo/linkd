#ifndef __UNOS_RTRNL_H__
#define __UNOS_RTRNL_H__

#include <stdbool.h>

// Netlink helpers that replace system("ip ...").
// All return 0 on success, -1 on failure (and log via rxi/log).

int rtnl_link_up(const char *ifname);
int rtnl_link_down(const char *ifname);
int rtnl_link_set_mtu(const char *ifname, int mtu);
int rtnl_link_set_hwaddr(const char *ifname, const char *hwaddr);

// address may be "10.0.0.1/24" or "2001:db8::1/64"; netmask is optional second arg for compat
int rtnl_addr_add(const char *ifname, const char *address, const char *netmask);
int rtnl_addr_del(const char *ifname, const char *address, const char *netmask);

int rtnl_route_add_default(const char *gateway, const char *ifname);
int rtnl_route_del_default(const char *gateway, const char *ifname);

// VLAN: create name type vlan id vlan_id with link rawdev (raw may be inferred from dot)
int rtnl_vlan_create(const char *name, const char *rawdev, int vlan_id);

// Bridge: create br with stp/vlan_aware, and enslave ports (space-separated)
// VRF: create an L3 master device bound to a routing table, and enslave
// interfaces to it. `table` is mandatory -- it is what actually separates the
// VRF's routes from main.
int rtnl_vrf_create(const char *name, uint32_t table);
int rtnl_vrf_add_port(const char *vrf, const char *port);

int rtnl_bridge_create(const char *name);
int rtnl_bridge_set_stp(const char *name, bool on);
int rtnl_bridge_set_vlan_aware(const char *name, bool on);
int rtnl_bridge_add_port(const char *br, const char *port);
int rtnl_bridge_del_port(const char *br, const char *port);

#endif // __UNOS_RTRNL_H__
