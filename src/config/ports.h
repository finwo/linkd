#ifndef __LINKD_CONFIG_PORTS_H__
#define __LINKD_CONFIG_PORTS_H__

#include <stdint.h>
#include <stdio.h>

#include "finwo/cnfparse.h"

#include "dataplane.h"

// Switch port physical-layer configuration.
//
// Scope is deliberately narrow. Addresses, MTU, MAC and admin state are all
// handled by ifupdown via /etc/network/interfaces{,.d}; re-implementing them
// here would mean two sources of truth for the same value. What remains is
// what ifupdown cannot reach on a KNET stub netdev.
struct linkd_port {
  char             *name;
  uint32_t          speed;    // Mbit/s, 0 = unset
  enum dp_fec       fec;
  int               autoneg;  // -1 unset, 0 off, 1 on
  char             *breakout; // e.g. "4x25g", NULL = none
  struct linkd_port *next;
};

// Registers the `port` directive with the config parser
void ports_register(void);

struct cnf_directive * cfg_parse_port(FILE *fd, struct cnf_directive *dir, void *user);

struct linkd_port * ports_list(void);
struct linkd_port * ports_find(const char *name);
void ports_free(void);

#endif // __LINKD_CONFIG_PORTS_H__
