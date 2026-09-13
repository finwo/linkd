// Built-in dataplane backend.
//
// This is the "there is no switching ASIC" case: generic x86_64, a VM, or a
// soft-router. The Linux kernel is already the forwarding engine, so every
// entry point here is a genuine no-op rather than an unimplemented stub:
//
//   - addresses, MTU, MAC and admin state are applied by ifupdown directly to
//     the netdev, so there is nothing to replicate
//   - routes and neighbours are already in the FIB by the time we observe the
//     netlink message announcing them
//   - speed / FEC / autoneg are PHY properties of a real NIC, driven through
//     ethtool, and meaningless in a VM
//
// Mirroring any of it back into the kernel would be circular. unosd on generic
// hardware is therefore close to inert by design: it parses configuration,
// validates it, and lets Linux do the work.
//
// The table field on rif/neigh/route is accepted and ignored here: there is
// nothing per-VRF to replicate when the kernel itself forwards.

#include <stdio.h>

#include "dataplane.h"

static int kernel_probe(void) {
  // Always usable -- this is the fallback of last resort.
  return DP_RET_OK;
}

static int kernel_init(void) {
  fprintf(stderr, "dataplane/kernel: forwarding handled by the kernel\n");
  return DP_RET_OK;
}

static void kernel_fini(void) {
}

static int kernel_port_apply(const struct dp_port *port) { (void)port;            return DP_RET_OK; }
static int kernel_port_admin(const char *ifname, bool up) { (void)ifname; (void)up; return DP_RET_OK; }
static int kernel_port_mtu(const char *ifname, uint32_t mtu) { (void)ifname; (void)mtu; return DP_RET_OK; }

static int kernel_rif_add(const struct dp_rif *rif)       { (void)rif;   return DP_RET_OK; }
static int kernel_rif_del(const struct dp_rif *rif)       { (void)rif;   return DP_RET_OK; }
static int kernel_neigh_add(const struct dp_neigh *neigh) { (void)neigh; return DP_RET_OK; }
static int kernel_neigh_del(const struct dp_neigh *neigh) { (void)neigh; return DP_RET_OK; }
static int kernel_route_add(const struct dp_route *route) { (void)route; return DP_RET_OK; }
static int kernel_route_del(const struct dp_route *route) { (void)route; return DP_RET_OK; }

static int kernel_resync(void) {
  return DP_RET_OK;
}

static const struct dp_ops kernel_ops = {
  .abi        = UNOS_DATAPLANE_ABI,
  .name       = "kernel",
  .probe      = kernel_probe,
  .init       = kernel_init,
  .fini       = kernel_fini,
  .port_apply = kernel_port_apply,
  .port_admin = kernel_port_admin,
  .port_mtu   = kernel_port_mtu,
  .rif_add    = kernel_rif_add,
  .rif_del    = kernel_rif_del,
  .neigh_add  = kernel_neigh_add,
  .neigh_del  = kernel_neigh_del,
  .route_add  = kernel_route_add,
  .route_del  = kernel_route_del,
  .resync     = kernel_resync,
};

const struct dp_ops * dp_kernel_ops(void) {
  return &kernel_ops;
}
