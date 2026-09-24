#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "finwo/resp.h"
#include "rxi/log.h"

#include "dataplane.h"
#include "dataplane/plugin.h"

#define MAX_ARGV 24

// Every supporter is called even after one fails, so plugins cannot end up
// disagreeing about what was applied. `optional` failures do not fail the op.
static int broadcast(const char **argv, int argc) {
  int failed = 0;
  int sent   = 0;

  for (struct dp_plugin *p = dp_plugin_list(); p; p = p->next) {
    // Retry a down plugin here: an operation is exactly when we want it back.
    if (!p->up) dp_plugin_up(p);
    if (!dp_plugin_supports(p, argv[0])) continue;

    sent++;
    resp_object *r = dp_plugin_call(p, argv, argc);
    if (!r) {
      if (!p->optional) failed = 1;
      continue;
    }
    if (r->type == RESPT_ERROR) {
      log_error("plugin %s: %s %s: %s", p->target, argv[0],
                argc > 1 ? argv[1] : "", r->u.s ? r->u.s : "error");
      if (!p->optional) failed = 1;
    }
    resp_free(r);
  }

  (void)sent;
  return failed ? DP_RET_ERROR : DP_RET_OK;
}

static const char *fec_str(enum dp_fec f) {
  switch (f) {
    case DP_FEC_OFF:   return "off";
    case DP_FEC_AUTO:  return "auto";
    case DP_FEC_RS:    return "rs";
    case DP_FEC_BASER: return "baser";
    default:           return NULL;
  }
}

// dp_addr -> "10.0.0.1/24"
static int addr_str(const struct dp_addr *a, char *out, size_t len) {
  char ip[INET6_ADDRSTRLEN];
  if (!inet_ntop(a->family, a->addr, ip, sizeof(ip))) return -1;
  snprintf(out, len, "%s/%u", ip, a->prefixlen);
  return 0;
}

static int addr_plain(const struct dp_addr *a, char *out, size_t len) {
  return inet_ntop(a->family, a->addr, out, (socklen_t)len) ? 0 : -1;
}

int dp_port_apply(const struct dp_port *port) {
  if (!dp_have_plugins()) return DP_RET_OK;

  const char *argv[MAX_ARGV];
  char speed[16], autoneg[12];
  int  argc = 0;

  argv[argc++] = "PORT";
  argv[argc++] = "APPLY";
  argv[argc++] = port->name;

  if (port->speed) {
    snprintf(speed, sizeof(speed), "%u", port->speed);
    argv[argc++] = "speed";
    argv[argc++] = speed;
  }
  const char *f = fec_str(port->fec);
  if (f) {
    argv[argc++] = "fec";
    argv[argc++] = f;
  }
  if (port->autoneg >= 0) {
    snprintf(autoneg, sizeof(autoneg), "%d", port->autoneg);
    argv[argc++] = "autoneg";
    argv[argc++] = autoneg;
  }
  return broadcast(argv, argc);
}

int dp_port_admin(const char *ifname, bool up) {
  if (!dp_have_plugins()) return DP_RET_OK;
  const char *argv[] = { "PORT", "ADMIN", ifname, up ? "up" : "down" };
  return broadcast(argv, 4);
}

int dp_port_mtu(const char *ifname, uint32_t mtu) {
  if (!dp_have_plugins()) return DP_RET_OK;
  char m[16];
  snprintf(m, sizeof(m), "%u", mtu);
  const char *argv[] = { "PORT", "MTU", ifname, m };
  return broadcast(argv, 4);
}

static int rif_op(const char *sub, const struct dp_rif *rif) {
  if (!dp_have_plugins()) return DP_RET_OK;
  char addr[INET6_ADDRSTRLEN + 8], table[16];
  if (addr_str(&rif->addr, addr, sizeof(addr)) != 0) return DP_RET_ERROR;
  snprintf(table, sizeof(table), "%u", rif->table);
  const char *argv[] = { "RIF", sub, rif->ifname, addr, "table", table };
  return broadcast(argv, 6);
}

int dp_rif_add(const struct dp_rif *rif) { return rif_op("ADD", rif); }
int dp_rif_del(const struct dp_rif *rif) { return rif_op("DEL", rif); }

static int neigh_op(const char *sub, const struct dp_neigh *n) {
  if (!dp_have_plugins()) return DP_RET_OK;
  char ip[INET6_ADDRSTRLEN], mac[18], table[16];
  if (addr_plain(&n->addr, ip, sizeof(ip)) != 0) return DP_RET_ERROR;
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
           n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
  snprintf(table, sizeof(table), "%u", n->table);
  const char *argv[] = { "NEIGH", sub, n->ifname, ip, mac, "table", table };
  return broadcast(argv, 7);
}

int dp_neigh_add(const struct dp_neigh *n) { return neigh_op("ADD", n); }
int dp_neigh_del(const struct dp_neigh *n) { return neigh_op("DEL", n); }

// ROUTE ADD <dst> [via <gw> dev <if>]... [metric N] [table N]
// Nexthops are flattened rather than nested so a shell plugin can parse them.
static int route_op(const char *sub, const struct dp_route *r) {
  if (!dp_have_plugins()) return DP_RET_OK;

  const char *argv[MAX_ARGV];
  char dst[INET6_ADDRSTRLEN + 8];
  char gw[4][INET6_ADDRSTRLEN];
  char metric[16], table[16];
  int  argc = 0;

  if (addr_str(&r->dst, dst, sizeof(dst)) != 0) return DP_RET_ERROR;

  argv[argc++] = "ROUTE";
  argv[argc++] = sub;
  argv[argc++] = dst;

  size_t nh = r->nh_count;
  if (nh > 4) nh = 4; // argv budget; ECMP wider than this is truncated
  for (size_t i = 0; i < nh; i++) {
    if (argc + 4 >= MAX_ARGV) break;
    if (addr_plain(&r->nh[i].gw, gw[i], sizeof(gw[i])) == 0) {
      argv[argc++] = "via";
      argv[argc++] = gw[i];
    }
    if (r->nh[i].ifname) {
      argv[argc++] = "dev";
      argv[argc++] = r->nh[i].ifname;
    }
  }
  if (r->metric) {
    snprintf(metric, sizeof(metric), "%u", r->metric);
    argv[argc++] = "metric";
    argv[argc++] = metric;
  }
  snprintf(table, sizeof(table), "%u", r->table);
  argv[argc++] = "table";
  argv[argc++] = table;

  return broadcast(argv, argc);
}

int dp_route_add(const struct dp_route *r) { return route_op("ADD", r); }
int dp_route_del(const struct dp_route *r) { return route_op("DEL", r); }

int dp_resync(void) {
  if (!dp_have_plugins()) return DP_RET_OK;
  const char *argv[] = { "RESYNC" };
  return broadcast(argv, 1);
}
