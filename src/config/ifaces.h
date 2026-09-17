#ifndef __UNOS_CONFIG_IFACES_H__
#define __UNOS_CONFIG_IFACES_H__

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

struct unos_iface {
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
  int                 auto_flag; // 1 if listed in auto
  struct unos_iface  *next;
};

void ifaces_register(void);

struct cnf_directive * cfg_parse_auto(FILE *fd, struct cnf_directive *dir, void *user);
struct cnf_directive * cfg_parse_iface(FILE *fd, struct cnf_directive *dir, void *user);

struct unos_iface * ifaces_list(void);
struct unos_iface * ifaces_find(const char *name);
void ifaces_free(void);
void ifaces_set_auto(const char *name, int flag);

#endif // __UNOS_CONFIG_IFACES_H__
