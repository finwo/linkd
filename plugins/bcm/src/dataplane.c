// Broadcom XGS dataplane backend for unosd.
//
// Built and shipped by the `openbcm` package, NOT by the unosd build. It is a
// plugin precisely so that a single UNOS image runs unchanged on hardware with
// and without a switching ASIC: on a Broadcom box the openbcm package drops
// this object into UNOS_DATAPLANE_DIR, everywhere else the directory stays
// empty and unosd falls back to the built-in kernel backend.
//
// Building this requires the OpenBCM SDK headers and libbcm. See the README
// next to this file.

#include <stdio.h>
#include <unistd.h>

#include "dataplane.h"

// The BDE character device is created by the openbcm kernel modules once they
// have enumerated a switch ASIC over PCIe. Its presence is the cheapest
// reliable signal that this backend can drive the box.
#define BCM_BDE_DEV "/dev/linux-kernel-bde"

static int bcm_probe(void) {
  if (access(BCM_BDE_DEV, F_OK) != 0) return DP_RET_ERROR;
  return DP_RET_OK;
}

static int bcm_init(void) {
  // TODO: SDK bring-up.
  //   - read the board config (config.bcm) for the port/SerDes map
  //   - soc_cm_device_create / attach unit 0
  //   - run the board init sequence
  //   - create KNET netdevs (swpN) for each front-panel port
  fprintf(stderr, "dataplane/bcm: SDK bring-up not implemented\n");
  return DP_RET_ERROR;
}

static void bcm_fini(void) {
  // TODO: detach unit, tear down KNET netdevs
}

static int bcm_port_apply(const struct dp_port *port) {
  // TODO: bcm_port_speed_set / bcm_port_autoneg_set, and
  //       bcm_port_phy_control_set for FEC
  (void)port;
  return DP_RET_ERROR;
}

static int bcm_port_admin(const char *ifname, bool up) {
  // TODO: bcm_port_enable_set
  (void)ifname;
  (void)up;
  return DP_RET_ERROR;
}

static int bcm_port_mtu(const char *ifname, uint32_t mtu) {
  // TODO: bcm_port_frame_max_set. Driven by the netlink mirror so the ASIC
  //       frame limit tracks whatever ifupdown put on the netdev.
  (void)ifname;
  (void)mtu;
  return DP_RET_ERROR;
}

static int bcm_rif_add(const struct dp_rif *rif) {
  // TODO: bcm_l3_intf_create
  (void)rif;
  return DP_RET_ERROR;
}

static int bcm_rif_del(const struct dp_rif *rif) {
  // TODO: bcm_l3_intf_delete
  (void)rif;
  return DP_RET_ERROR;
}

static int bcm_neigh_add(const struct dp_neigh *neigh) {
  // TODO: bcm_l3_host_add
  (void)neigh;
  return DP_RET_ERROR;
}

static int bcm_neigh_del(const struct dp_neigh *neigh) {
  // TODO: bcm_l3_host_delete
  (void)neigh;
  return DP_RET_ERROR;
}

static int bcm_route_add(const struct dp_route *route) {
  // TODO: bcm_l3_route_add; for nh_count > 1 build a bcm_l3_egress_ecmp
  //       group and refcount it across routes sharing the same nexthop set
  (void)route;
  return DP_RET_ERROR;
}

static int bcm_route_del(const struct dp_route *route) {
  // TODO: bcm_l3_route_delete, release ECMP group when refcount hits zero
  (void)route;
  return DP_RET_ERROR;
}

static int bcm_resync(void) {
  // TODO: walk the ASIC tables, diff against the kernel, converge.
  // Netlink drops messages under load, so this is the correctness backstop,
  // not an optimisation.
  return DP_RET_ERROR;
}

struct dp_ops unos_dataplane_ops = {
  .abi        = UNOS_DATAPLANE_ABI,
  .name       = "bcm",
  .probe      = bcm_probe,
  .init       = bcm_init,
  .fini       = bcm_fini,
  .port_apply = bcm_port_apply,
  .port_admin = bcm_port_admin,
  .port_mtu   = bcm_port_mtu,
  .rif_add    = bcm_rif_add,
  .rif_del    = bcm_rif_del,
  .neigh_add  = bcm_neigh_add,
  .neigh_del  = bcm_neigh_del,
  .route_add  = bcm_route_add,
  .route_del  = bcm_route_del,
  .resync     = bcm_resync,
};
