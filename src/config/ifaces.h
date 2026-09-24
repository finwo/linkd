#ifndef __LINKD_CONFIG_IFACES_H__
#define __LINKD_CONFIG_IFACES_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "finwo/cnfparse.h"

// Network interface configuration from /etc/network/interfaces
// IPv6 is first-class: address may be v4 or v6, gateway may be v4 or v6.

struct iface_addr {
  char              *address; // "192.168.1.1/24" or "2001:db8::1/64"
  char              *netmask; // v4 only, optional
  struct iface_addr *next;
};

struct linkd_iface {
  char               *name;    // e.g., "eth0", "swp1.100" -- cumulus style `iface <name>` preferred
  char               *method;  // "static", "dhcp", "loopback", "manual" -- optional, defaults to "static" when `iface <name>` used without `inet`
  char               *family;  // "inet", "inet6" -- optional, NULL when cumulus style
  struct iface_addr  *addrs;
  char               *gateway;
  char               *broadcast;
  int                 mtu;     // 0 = unset
  char               *hwaddress;
  char               *vlan_raw_device;
  int                 vlan_id; // -1 = unset, 1..4094
  char               *pre_up;
  char               *post_up;
  char               *pre_down;
  char               *post_down;
  char               *bridge_ports; // space-separated, e.g. "swp1 swp2"
  char               *bridge_stp; // "on"/"off"
  int                 bridge_vlan_aware; // -1 unset, 0 no, 1 yes
  // VRF. Two distinct roles, deliberately separate fields:
  //   vrf_table >= 0  -> THIS interface is a VRF device with that table id
  //                      (`vrf-table <id|auto>`)
  //   vrf_master      -> this interface is a MEMBER of the named VRF
  //                      (`vrf <name>`)
  // Both appear in every production Cumulus config we captured; a mgmt VRF is
  // how management traffic is kept out of the table BGP populates.
  int                 vrf_table;  // -1 unset, otherwise 1..2^31-1
  char               *vrf_master; // NULL unset
  int                 auto_flag; // 1 if listed in auto
  struct linkd_iface  *next;
};

// Shared classification. Previously duplicated in cli/linkd.c and ipc.c, which
// let the two drift; keep exactly one definition.
bool iface_is_bridge(const struct linkd_iface *ifc);
bool iface_is_vrf(const struct linkd_iface *ifc);

void ifaces_register(void);

struct cnf_directive * cfg_parse_auto(FILE *fd, struct cnf_directive *dir, void *user);
struct cnf_directive * cfg_parse_iface(FILE *fd, struct cnf_directive *dir, void *user);

struct linkd_iface * ifaces_list(void);
struct linkd_iface * ifaces_find(const char *name);
void ifaces_free(void);
void ifaces_set_auto(const char *name, int flag);

#endif // __LINKD_CONFIG_IFACES_H__
