#define _GNU_SOURCE

#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/fib_rules.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <arpa/inet.h>

#include "rxi/log.h"

#include "dataplane.h"
#include "dataplane/plugin.h"
#include "../dhcp.h"

#include "filter.h"
#include "netlink.h"
#include "../ipc.h"

#define NL_BUFSZ (64 * 1024)
#define NL_MAX_NH 16

// VRF masters: master ifindex -> table id + name. Single digits expected.
struct vrf_master {
  int ifindex;
  uint32_t table;
  char name[IF_NAMESIZE];
};

// Member enslavement: member ifindex -> master ifindex (0 = none).
struct vrf_member {
  int ifindex;
  int master;
};

#define VRF_MAX_MASTERS 64
#define VRF_MAX_MEMBERS 256

static struct vrf_master vrf_masters[VRF_MAX_MASTERS];
static int vrf_master_count = 0;
static struct vrf_member vrf_members[VRF_MAX_MEMBERS];
static int vrf_member_count = 0;

static volatile sig_atomic_t nl_stop = 0;
static uint32_t nl_seq = 0;

void nl_request_stop(void) {
  nl_stop = 1;
}

static void vrf_map_reset(void) {
  vrf_master_count = 0;
  vrf_member_count = 0;
}

static struct vrf_master *vrf_master_find(int ifindex) {
  int i;
  for (i = 0; i < vrf_master_count; i++) {
    if (vrf_masters[i].ifindex == ifindex) return &vrf_masters[i];
  }
  return NULL;
}

static void vrf_master_set(int ifindex, uint32_t table, const char *name) {
  struct vrf_master *m = vrf_master_find(ifindex);
  if (m) {
    m->table = table;
  } else {
    if (vrf_master_count >= VRF_MAX_MASTERS) {
      log_warn("netlink: vrf master table full, ignoring %s", name ? name : "?");
      return;
    }
    m = &vrf_masters[vrf_master_count++];
    m->ifindex = ifindex;
    m->table = table;
  }
  if (name) {
    strncpy(m->name, name, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = '\0';
  }
}

static void vrf_master_remove(int ifindex) {
  int i;
  for (i = 0; i < vrf_master_count; i++) {
    if (vrf_masters[i].ifindex == ifindex) {
      vrf_masters[i] = vrf_masters[--vrf_master_count];
      return;
    }
  }
}

static void vrf_member_set(int ifindex, int master) {
  int i;
  for (i = 0; i < vrf_member_count; i++) {
    if (vrf_members[i].ifindex == ifindex) {
      vrf_members[i].master = master;
      return;
    }
  }
  if (vrf_member_count >= VRF_MAX_MEMBERS) return;
  vrf_members[vrf_member_count].ifindex = ifindex;
  vrf_members[vrf_member_count].master = master;
  vrf_member_count++;
}

static void vrf_member_remove(int ifindex) {
  int i;
  for (i = 0; i < vrf_member_count; i++) {
    if (vrf_members[i].ifindex == ifindex) {
      vrf_members[i] = vrf_members[--vrf_member_count];
      return;
    }
  }
}

// Table owning an interface: master's table, or MAIN when unenslaved/unknown.
static uint32_t vrf_table_of_iface(int ifindex) {
  int i;
  int master = 0;
  for (i = 0; i < vrf_member_count; i++) {
    if (vrf_members[i].ifindex == ifindex) {
      master = vrf_members[i].master;
      break;
    }
  }
  if (!master) return RT_TABLE_MAIN;
  for (i = 0; i < vrf_master_count; i++) {
    if (vrf_masters[i].ifindex == master) return vrf_masters[i].table;
  }
  return RT_TABLE_MAIN;
}

// rtm_table is 8-bit; ids above 255 arrive via RTA_TABLE with rtm_table set to
// RT_TABLE_COMPAT. Getting this wrong silently aliases VRF tables onto MAIN.
static uint32_t route_table_of(const struct rtmsg *rtm, struct rtattr **tb) {
  uint32_t table = rtm->rtm_table;
  if (table == RT_TABLE_COMPAT && tb[RTA_TABLE]) {
    memcpy(&table, RTA_DATA(tb[RTA_TABLE]), sizeof(table));
  }
  return table;
}

static void parse_rtattr(struct rtattr **tb, int max, struct rtattr *rta, int len) {
  memset(tb, 0, sizeof(struct rtattr *) * (max + 1));
  while (RTA_OK(rta, len)) {
    if (rta->rta_type <= max) tb[rta->rta_type] = rta;
    rta = RTA_NEXT(rta, len);
  }
}

// No NDMSG_RTA helper in the UAPI headers; attrs follow the fixed struct.
static struct rtattr *ndmsg_rta(const struct ndmsg *ndm) {
  return (struct rtattr *)(((char *)ndm) + NLMSG_ALIGN(sizeof(*ndm)));
}

static int handle_link(struct nlmsghdr *nh) {
  struct ifinfomsg *ifi = NLMSG_DATA(nh);
  struct rtattr *tb[IFLA_MAX + 1];
  char ifname[IF_NAMESIZE] = "";
  uint32_t mtu = 0;
  int master = 0;
  int is_vrf = 0;
  uint32_t vrf_table = 0;
  int len;

  len = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi)));
  if (len < 0) return 0;
  parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

  if (!tb[IFLA_IFNAME]) return 0;
  strncpy(ifname, RTA_DATA(tb[IFLA_IFNAME]), sizeof(ifname) - 1);
  if (tb[IFLA_MTU]) memcpy(&mtu, RTA_DATA(tb[IFLA_MTU]), sizeof(mtu));
  if (tb[IFLA_MASTER]) memcpy(&master, RTA_DATA(tb[IFLA_MASTER]), sizeof(master));

  // VRF masters carry linkinfo kind "vrf" with the table id nested inside
  // IFLA_INFO_DATA as IFLA_VRF_TABLE.
  if (tb[IFLA_LINKINFO]) {
    struct rtattr *li[IFLA_INFO_MAX + 1];
    struct rtattr *rta = RTA_DATA(tb[IFLA_LINKINFO]);
    int lilen = (int)RTA_PAYLOAD(tb[IFLA_LINKINFO]);
    parse_rtattr(li, IFLA_INFO_MAX, rta, lilen);
    if (li[IFLA_INFO_KIND] && !strcmp(RTA_DATA(li[IFLA_INFO_KIND]), "vrf")) {
      is_vrf = 1;
      if (li[IFLA_INFO_DATA]) {
        struct rtattr *vd[IFLA_VRF_MAX + 1];
        struct rtattr *vrta = RTA_DATA(li[IFLA_INFO_DATA]);
        int vlen = (int)RTA_PAYLOAD(li[IFLA_INFO_DATA]);
        parse_rtattr(vd, IFLA_VRF_MAX, vrta, vlen);
        if (vd[IFLA_VRF_TABLE]) {
          memcpy(&vrf_table, RTA_DATA(vd[IFLA_VRF_TABLE]), sizeof(vrf_table));
        }
      }
    }
  }

  if (nh->nlmsg_type == RTM_DELLINK) {
    vrf_member_remove(ifi->ifi_index);
    if (is_vrf || vrf_master_find(ifi->ifi_index)) {
      // Removal drops tracking; a standing exclusion survives re-creation.
      if (vrf_table) nl_filter_untrack(vrf_table);
      vrf_master_remove(ifi->ifi_index);
      log_info("netlink: vrf %s removed", ifname);
    }
    if (dp_port_admin(ifname, false) != DP_RET_OK) {
      log_error("netlink: port_admin %s down failed", ifname);
    }
    return 0;
  }

  vrf_member_set(ifi->ifi_index, master);
  if (is_vrf && vrf_table) {
    vrf_master_set(ifi->ifi_index, vrf_table, ifname);
    if (nl_filter_mgmt_name(ifname)) {
      nl_filter_exclude(vrf_table);
      log_info("netlink: vrf %s table %u excluded (management)", ifname, vrf_table);
    } else {
      nl_filter_track(vrf_table);
      log_info("netlink: vrf %s table %u tracked", ifname, vrf_table);
    }
  }

  log_debug("netlink: link %s %s mtu %u", ifname, (ifi->ifi_flags & IFF_UP) ? "up" : "down", mtu);
  if (dp_port_admin(ifname, (ifi->ifi_flags & IFF_UP) ? true : false) != DP_RET_OK) {
    log_error("netlink: port_admin %s failed", ifname);
  }
  if (mtu && dp_port_mtu(ifname, mtu) != DP_RET_OK) {
    log_error("netlink: port_mtu %s %u failed", ifname, mtu);
  }
  return 0;
}

static int handle_addr(struct nlmsghdr *nh) {
  struct ifaddrmsg *ifa = NLMSG_DATA(nh);
  struct rtattr *tb[IFA_MAX + 1];
  char ifname[IF_NAMESIZE] = "";
  struct dp_rif rif;
  struct dp_addr addr;
  void *bytes = NULL;
  size_t want = 0;
  int len;
  int del;

  len = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa)));
  if (len < 0) return 0;
  parse_rtattr(tb, IFA_MAX, IFA_RTA(ifa), len);

  if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6) return 0;
  // IFA_LOCAL is the local address on point-to-point v4; IFA_ADDRESS would be
  // the peer. Prefer LOCAL whenever present.
  if (tb[IFA_LOCAL]) bytes = RTA_DATA(tb[IFA_LOCAL]);
  else if (tb[IFA_ADDRESS]) bytes = RTA_DATA(tb[IFA_ADDRESS]);
  else return 0;
  want = (ifa->ifa_family == AF_INET) ? 4 : 16;

  if (!if_indextoname(ifa->ifa_index, ifname)) return 0;

  memset(&addr, 0, sizeof(addr));
  addr.family = ifa->ifa_family;
  addr.prefixlen = ifa->ifa_prefixlen;
  memcpy(addr.addr, bytes, want);

  memset(&rif, 0, sizeof(rif));
  rif.ifname = ifname;
  rif.addr = addr;
  rif.table = vrf_table_of_iface(ifa->ifa_index);
  if (!nl_table_allowed(rif.table)) {
    log_debug("netlink: rif %s skipped (table %u not tracked)", ifname, rif.table);
    return 0;
  }

  del = (nh->nlmsg_type == RTM_DELADDR);
  log_debug("netlink: rif %s %s table %u", ifname, del ? "del" : "add", rif.table);
  if (del) {
    if (dp_rif_del(&rif) != DP_RET_OK) log_error("netlink: rif_del %s failed", ifname);
  } else {
    if (dp_rif_add(&rif) != DP_RET_OK) log_error("netlink: rif_add %s failed", ifname);
  }
  return 0;
}

static int handle_neigh(struct nlmsghdr *nh) {
  struct ndmsg *ndm = NLMSG_DATA(nh);
  struct rtattr *tb[NDA_MAX + 1];
  char ifname[IF_NAMESIZE] = "";
  struct dp_neigh neigh;
  int len;
  int del;

  len = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*ndm)));
  if (len < 0) return 0;
  parse_rtattr(tb, NDA_MAX, ndmsg_rta(ndm), len);

  if (ndm->ndm_family != AF_INET && ndm->ndm_family != AF_INET6) return 0;
  // Valid-only: incomplete/failed resolutions never reach the dataplane.
  del = (nh->nlmsg_type == RTM_DELNEIGH);
  if (!del) {
    if (ndm->ndm_state & (NUD_FAILED | NUD_INCOMPLETE)) del = 1;
    else if (!(ndm->ndm_state & (NUD_PERMANENT | NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE))) {
      return 0;
    }
  }
  if (!tb[NDA_DST]) return 0;
  if (!del && (!tb[NDA_LLADDR] || RTA_PAYLOAD(tb[NDA_LLADDR]) != 6)) {
    log_debug("netlink: neigh skipped (no MAC)");
    return 0;
  }
  if (!if_indextoname(ndm->ndm_ifindex, ifname)) return 0;

  memset(&neigh, 0, sizeof(neigh));
  neigh.ifname = ifname;
  neigh.addr.family = ndm->ndm_family;
  neigh.addr.prefixlen = (ndm->ndm_family == AF_INET) ? 32 : 128;
  memcpy(neigh.addr.addr, RTA_DATA(tb[NDA_DST]), (ndm->ndm_family == AF_INET) ? 4 : 16);
  if (!del) memcpy(neigh.mac, RTA_DATA(tb[NDA_LLADDR]), 6);
  neigh.table = vrf_table_of_iface(ndm->ndm_ifindex);
  if (!nl_table_allowed(neigh.table)) {
    log_debug("netlink: neigh %s skipped (table %u not tracked)", ifname, neigh.table);
    return 0;
  }

  log_debug("netlink: neigh %s %s table %u", ifname, del ? "del" : "add", neigh.table);
  if (del) {
    if (dp_neigh_del(&neigh) != DP_RET_OK) log_error("netlink: neigh_del failed");
  } else {
    if (dp_neigh_add(&neigh) != DP_RET_OK) log_error("netlink: neigh_add failed");
  }
  return 0;
}

static int handle_route(struct nlmsghdr *nh) {
  struct rtmsg *rtm = NLMSG_DATA(nh);
  struct rtattr *tb[RTA_MAX + 1];
  struct dp_route route;
  struct dp_nexthop nhops[NL_MAX_NH];
  char nhnames[NL_MAX_NH][IF_NAMESIZE];
  uint32_t table;
  uint32_t metric = 0;
  int len;
  int del;
  size_t nh_count = 0;

  len = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*rtm)));
  if (len < 0) return 0;
  parse_rtattr(tb, RTA_MAX, RTM_RTA(rtm), len);

  table = route_table_of(rtm, tb);
  if (!nl_route_allowed(rtm->rtm_family, rtm->rtm_type, rtm->rtm_scope, table)) {
    return 0;
  }
  if (tb[RTA_PRIORITY]) memcpy(&metric, RTA_DATA(tb[RTA_PRIORITY]), sizeof(metric));

  memset(&route, 0, sizeof(route));
  memset(nhops, 0, sizeof(nhops));
  memset(nhnames, 0, sizeof(nhnames));
  route.dst.family = rtm->rtm_family;
  route.dst.prefixlen = rtm->rtm_dst_len;
  if (tb[RTA_DST]) {
    size_t want = (rtm->rtm_family == AF_INET) ? 4 : 16;
    memcpy(route.dst.addr, RTA_DATA(tb[RTA_DST]), want);
  }
  route.metric = metric;
  route.table = table;

  if (tb[RTA_MULTIPATH]) {
    // ECMP: nexthops nest gateways inside each rtnexthop, not at top level.
    struct rtnexthop *rtnh = RTA_DATA(tb[RTA_MULTIPATH]);
    int mplen = (int)RTA_PAYLOAD(tb[RTA_MULTIPATH]);
    while (mplen >= (int)sizeof(*rtnh) && RTNH_OK(rtnh, mplen) && nh_count < NL_MAX_NH) {
      struct rtattr *hops[RTA_MAX + 1];
      int cur = RTNH_ALIGN(rtnh->rtnh_len);
      int hlen = (int)rtnh->rtnh_len - (int)sizeof(*rtnh);
      memset(hops, 0, sizeof(hops));
      if (hlen > 0) parse_rtattr(hops, RTA_MAX, RTNH_DATA(rtnh), hlen);
      if (if_indextoname(rtnh->rtnh_ifindex, nhnames[nh_count])) {
        nhops[nh_count].ifname = nhnames[nh_count];
        nhops[nh_count].gw.family = rtm->rtm_family;
        if (hops[RTA_GATEWAY]) {
          size_t want = (rtm->rtm_family == AF_INET) ? 4 : 16;
          memcpy(nhops[nh_count].gw.addr, RTA_DATA(hops[RTA_GATEWAY]), want);
        } else {
          nhops[nh_count].gw.family = AF_UNSPEC;
        }
        nh_count++;
      }
      rtnh = RTNH_NEXT(rtnh);
      mplen -= cur;
    }
    if (mplen > 0 && nh_count >= NL_MAX_NH) {
      log_warn("netlink: ECMP group truncated to %d nexthops", NL_MAX_NH);
    }
  } else {
    uint32_t oif = 0;
    if (tb[RTA_OIF]) memcpy(&oif, RTA_DATA(tb[RTA_OIF]), sizeof(oif));
    // Directly-connected routes carry OIF without a gateway; represent with
    // an empty (AF_UNSPEC) gw rather than inventing one.
    if (oif || tb[RTA_GATEWAY]) {
      if (oif && !if_indextoname(oif, nhnames[0])) return 0;
      nhops[0].ifname = oif ? nhnames[0] : NULL;
      nhops[0].gw.family = rtm->rtm_family;
      if (tb[RTA_GATEWAY]) {
        size_t want = (rtm->rtm_family == AF_INET) ? 4 : 16;
        memcpy(nhops[0].gw.addr, RTA_DATA(tb[RTA_GATEWAY]), want);
      } else {
        nhops[0].gw.family = AF_UNSPEC;
      }
      nh_count = 1;
    }
  }

  route.nh = nh_count ? nhops : NULL;
  route.nh_count = nh_count;

  del = (nh->nlmsg_type == RTM_DELROUTE);
  log_debug("netlink: route %s table %u nh %zu", del ? "del" : "add", table, nh_count);
  if (del) {
    if (dp_route_del(&route) != DP_RET_OK) log_error("netlink: route_del table %u failed", table);
  } else {
    if (dp_route_add(&route) != DP_RET_OK) log_error("netlink: route_add table %u failed", table);
  }
  return 0;
}

static int handle_nlmsg(struct nlmsghdr *nh) {
  switch (nh->nlmsg_type) {
  case RTM_NEWLINK:
  case RTM_DELLINK:
    return handle_link(nh);
  case RTM_NEWADDR:
  case RTM_DELADDR:
    return handle_addr(nh);
  case RTM_NEWNEIGH:
  case RTM_DELNEIGH:
    return handle_neigh(nh);
  case RTM_NEWROUTE:
  case RTM_DELROUTE:
    return handle_route(nh);
  default:
    return 0;
  }
}

int nl_open(void) {
  struct sockaddr_nl addr;
  int fd;
  int sndbuf = 256 * 1024;
  int rcvbuf = 2 * 1024 * 1024;

  fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
  if (fd < 0) {
    log_error("netlink: socket failed: %s", strerror(errno));
    return -1;
  }
  // Best effort: large receive buffer so bursts become resyncs, not losses.
  // May fail without privilege in dev environments; event flow still works.
  if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) != 0) {
    log_warn("netlink: SO_RCVBUFFORCE failed: %s", strerror(errno));
  }
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  // RTMGRP_* are group bitmasks; RTNLGRP_* are group numbers. Do not mix up.
  addr.nl_groups = RTMGRP_LINK | RTMGRP_NEIGH
    | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
    | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    log_error("netlink: bind failed: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

// Send one GET*+DUMP and dispatch matching replies through handle_nlmsg.
int nl_dump(int fd, uint16_t type, int family) {
  struct {
    struct nlmsghdr nh;
    struct rtgenmsg gen;
  } req;
  char buf[NL_BUFSZ];
  int done = 0;
  int rc = 0;

  memset(&req, 0, sizeof(req));
  req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.gen));
  req.nh.nlmsg_type = type;
  req.nh.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
  req.nh.nlmsg_seq = ++nl_seq;
  req.nh.nlmsg_pid = 0;
  req.gen.rtgen_family = (uint8_t)family;

  if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
    log_error("netlink: dump send %u failed: %s", type, strerror(errno));
    return -1;
  }

  while (!done) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct nlmsghdr *h;
    int len;
    int pr = poll(&pfd, 1, 5000);
    if (pr <= 0) {
      log_error("netlink: dump recv %u timed out", type);
      return -1;
    }
    len = recv(fd, buf, sizeof(buf), 0);
    if (len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      log_error("netlink: dump recv %u failed: %s", type, strerror(errno));
      return -1;
    }
    h = (struct nlmsghdr *)buf;
    while (NLMSG_OK(h, len)) {
      if (h->nlmsg_seq && h->nlmsg_seq != nl_seq) goto next;
      if (h->nlmsg_type == NLMSG_DONE) {
        done = 1;
        break;
      }
      if (h->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *e = NLMSG_DATA(h);
        if (e->error) {
          log_error("netlink: dump %u error: %s", type, strerror(-e->error));
          rc = -1;
          done = 1;
          break;
        }
        goto next;
      }
      handle_nlmsg(h);
    next:
      h = NLMSG_NEXT(h, len);
    }
  }
  return rc;
}

// Full converge: VRF map first (routes resolve against it), then RIFs,
// neighbours, routes, then the backend's own resync backstop.
int nl_resync(int fd) {
  int fails = 0;

  nl_filter_reset();
  vrf_map_reset();

  if (nl_dump(fd, RTM_GETLINK, AF_UNSPEC) != 0) fails++;
  if (nl_dump(fd, RTM_GETADDR, AF_INET) != 0) fails++;
  if (nl_dump(fd, RTM_GETADDR, AF_INET6) != 0) fails++;
  if (nl_dump(fd, RTM_GETNEIGH, AF_INET) != 0) fails++;
  if (nl_dump(fd, RTM_GETNEIGH, AF_INET6) != 0) fails++;
  if (nl_dump(fd, RTM_GETROUTE, AF_INET) != 0) fails++;
  if (nl_dump(fd, RTM_GETROUTE, AF_INET6) != 0) fails++;

  if (dp_resync() != DP_RET_OK) {
    log_error("netlink: plugin resync failed");
    fails++;
  }
  if (fails) {
    log_warn("netlink: resync finished with %d failures", fails);
    return -1;
  }
  log_info("netlink: resync complete");
  return 0;
}

static long elapsed_ms(const struct timespec *a, const struct timespec *b) {
  return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

int nl_run(int fd, int resync_interval_s) {
  static char buf[NL_BUFSZ];
  struct timespec last_resync;
  clock_gettime(CLOCK_MONOTONIC, &last_resync);

  while (!nl_stop) {
    dhcp_reap();

    // Netlink occupies slot 0; the IPC server contributes its listeners and
    // every live connection after it.
    struct pollfd pfds[1 + IPC_MAX_POLLFDS];
    int nfds = 1;
    struct timespec now;
    long wait_ms = 30000;
    int pr;

    pfds[0].fd = fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    nfds += ipc_pollfds(&pfds[1], IPC_MAX_POLLFDS);

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (resync_interval_s > 0) {
      long remain = resync_interval_s * 1000L - elapsed_ms(&last_resync, &now);
      if (remain <= 0) {
        if (nl_resync(fd) != 0) {
          // Log-and-continue with a single immediate retry; the next timer
          // converges anything still missing.
          log_warn("netlink: periodic resync failed, retrying once");
          if (nl_resync(fd) != 0) log_error("netlink: periodic resync retry failed");
        }
        clock_gettime(CLOCK_MONOTONIC, &last_resync);
        continue;
      }
      if (remain < wait_ms) wait_ms = remain;
    }

    pr = poll(pfds, nfds, (int)wait_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      log_error("netlink: poll failed: %s", strerror(errno));
      return -1;
    }
    if (!pr) continue;

    if (nfds > 1) ipc_dispatch(&pfds[1], nfds - 1);
    if (!(pfds[0].revents & POLLIN)) continue;

    for (;;) {
      struct nlmsghdr *h;
      struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
      struct msghdr msg;
      struct sockaddr_nl nladdr;
      ssize_t len;

      memset(&msg, 0, sizeof(msg));
      msg.msg_name = &nladdr;
      msg.msg_namelen = sizeof(nladdr);
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;

      len = recvmsg(fd, &msg, 0);
      if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
        if (errno == ENOBUFS) {
          // Expected under load, not exceptional: re-converge from dumps.
          log_warn("netlink: ENOBUFS, resyncing");
          if (nl_resync(fd) != 0) {
            log_warn("netlink: ENOBUFS resync failed, retrying once");
            nl_resync(fd);
          }
          clock_gettime(CLOCK_MONOTONIC, &last_resync);
          break;
        }
        log_error("netlink: recvmsg failed: %s", strerror(errno));
        return -1;
      }
      if (len == 0) break;
      if (msg.msg_flags & MSG_TRUNC) {
        log_warn("netlink: message truncated, resyncing");
        if (nl_resync(fd) != 0) nl_resync(fd);
        clock_gettime(CLOCK_MONOTONIC, &last_resync);
        break;
      }

      h = (struct nlmsghdr *)buf;
      while (NLMSG_OK(h, (int)len)) {
        if (h->nlmsg_type == NLMSG_ERROR) {
          struct nlmsgerr *e = NLMSG_DATA(h);
          if (e->error == -ENOBUFS) {
            log_warn("netlink: ENOBUFS via NLMSG_ERROR, resyncing");
            if (nl_resync(fd) != 0) nl_resync(fd);
            clock_gettime(CLOCK_MONOTONIC, &last_resync);
          } else if (e->error) {
            log_error("netlink: NLMSG_ERROR: %s", strerror(-e->error));
          }
        } else if (h->nlmsg_type != NLMSG_DONE) {
          handle_nlmsg(h);
        }
        h = NLMSG_NEXT(h, len);
      }
      // Non-blocking drain: keep reading until EAGAIN.
      if ((size_t)len < sizeof(buf)) break;
    }
  }
  return 0;
}
