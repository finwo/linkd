#ifndef __UNOS_DATAPLANE_H__
#define __UNOS_DATAPLANE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP_RET_ERROR -1
#define DP_RET_OK     0

// Bumped whenever struct dp_ops changes shape once a real plugin has shipped.
// Until then the structs below are fluid: extend freely without bumping.
// Plugins declaring a different ABI are refused at load time rather than
// crashing us later.
#define UNOS_DATAPLANE_ABI 1

// Where dlopen'd backends live. The openbcm package drops its backend here;
// on hardware without a switching ASIC the directory is simply empty and we
// fall back to the built-in kernel dataplane.
#define UNOS_DATAPLANE_DIR "/usr/lib/unos/dataplane"

// Symbol every plugin must export
#define UNOS_DATAPLANE_SYM "unos_dataplane_ops"

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

// Physical-layer properties of a switch port, from /etc/unos/ports.cnf.
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

struct dp_ops {
  uint32_t    abi;
  const char *name;

  // Return DP_RET_OK if this backend can drive the hardware in this box.
  // Must be cheap and must not have side effects; it is called on every
  // backend before one is chosen.
  int  (*probe)(void);

  int  (*init)(void);
  void (*fini)(void);

  // Applies ports.cnf properties.
  int (*port_apply)(const struct dp_port *port);

  // Driven by the netlink mirror, not by configuration: ifupdown sets these
  // on the netdev and we replicate them into the hardware.
  int (*port_admin)(const char *ifname, bool up);
  int (*port_mtu)(const char *ifname, uint32_t mtu);

  int (*rif_add)(const struct dp_rif *rif);
  int (*rif_del)(const struct dp_rif *rif);

  int (*neigh_add)(const struct dp_neigh *neigh);
  int (*neigh_del)(const struct dp_neigh *neigh);

  int (*route_add)(const struct dp_route *route);
  int (*route_del)(const struct dp_route *route);

  // Drop all state and rebuild it from the kernel. Netlink can and does drop
  // messages under load (ENOBUFS), so event-following alone drifts; every
  // backend needs to support a full resync.
  int (*resync)(void);
};

// Built-in backend. Always available, always probes successfully.
const struct dp_ops * dp_kernel_ops(void);

// Load plugins from UNOS_DATAPLANE_DIR. Safe to call when the directory is
// absent or empty.
int dp_plugins_load(const char *dir);

// Pick a backend. `force` may name one explicitly (skips probing), or be NULL
// to probe every registered backend and take the first that claims the box.
int dp_select(const char *force);

const struct dp_ops * dp_active(void);

int  dp_init(void);
void dp_fini(void);

#endif // __UNOS_DATAPLANE_H__
