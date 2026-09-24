#ifndef __LINKD_DATAPLANE_H__
#define __LINKD_DATAPLANE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP_RET_ERROR -1
#define DP_RET_OK     0

// Plugins are separate processes speaking RESP, declared in /etc/linkd.cnf:
//
//   plugin /usr/lib/linkd/bcm          spawned, RESP over stdio
//   plugin tcp://user:pass@host:6789   connected to, RESP over the socket
//
// They advertise what they implement via COMMAND, and each operation is
// broadcast to every plugin advertising its verb. There is no in-process ABI
// and nothing is dlopen'd, so a plugin can be written in any language.

enum dp_fec {
  DP_FEC_UNSET = 0,
  DP_FEC_OFF,
  DP_FEC_AUTO,
  DP_FEC_RS,
  DP_FEC_BASER,
};

struct dp_addr {
  int     family;      // AF_INET | AF_INET6
  uint8_t addr[16];
  uint8_t prefixlen;
};

// Physical-layer properties of a switch port, from /etc/linkd/ports.cnf.
//
// Deliberately narrow: anything the kernel already models on a netdev (MTU,
// admin state, addresses, MAC) is owned by ifupdown and reaches us through
// netlink instead. Only properties with no working kernel path on a KNET stub
// netdev live here.
struct dp_port {
  const char *name;    // swp1, ...
  uint32_t    speed;   // Mbit/s, 0 = unset
  enum dp_fec fec;
  int         autoneg; // -1 unset, 0 off, 1 on
};

struct dp_rif {
  const char     *ifname;
  struct dp_addr  addr;
  uint32_t        table;   // kernel route-table id; RT_TABLE_MAIN = default VRF
};

struct dp_neigh {
  const char     *ifname;
  struct dp_addr  addr;
  uint8_t         mac[6];
  uint32_t        table;   // kernel route-table id; RT_TABLE_MAIN = default VRF
};

struct dp_nexthop {
  const char     *ifname;
  struct dp_addr  gw;
};

struct dp_route {
  struct dp_addr     dst;
  struct dp_nexthop *nh;
  size_t             nh_count;   // >1 = ECMP group
  uint32_t           metric;     // RTA_PRIORITY, 0 = unset
  uint32_t           table;      // kernel route-table id; RT_TABLE_MAIN = default VRF
};

// Start every plugin declared in the config. Plugins that fail to start are
// retried with backoff; this is not an error in itself.
int  dp_start(void);
void dp_stop(void);

// True when at least one plugin is configured. Without any, every operation
// below is a successful no-op -- the kernel has already done the work by the
// time these are called.
int  dp_have_plugins(void);

// Operations. Each is broadcast to every plugin advertising the verb; all of
// them are called even if an earlier one fails, so plugins cannot end up in
// states that disagree with each other. Returns DP_RET_ERROR if any
// non-optional plugin failed.
int dp_port_apply(const struct dp_port *port);
int dp_port_admin(const char *ifname, bool up);
int dp_port_mtu(const char *ifname, uint32_t mtu);

int dp_rif_add(const struct dp_rif *rif);
int dp_rif_del(const struct dp_rif *rif);

int dp_neigh_add(const struct dp_neigh *neigh);
int dp_neigh_del(const struct dp_neigh *neigh);

int dp_route_add(const struct dp_route *route);
int dp_route_del(const struct dp_route *route);

// Netlink drops messages under load (ENOBUFS), so following events alone
// drifts; plugins are told to rebuild from scratch periodically.
int dp_resync(void);

#endif // __LINKD_DATAPLANE_H__
