#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rxi/log.h"

#include "rtnl.h"

#define NL_BUFSZ 8192

static int rtnl_open(void) {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) {
    log_error("rtnl: socket failed: %s", strerror(errno));
    return -1;
  }
  struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    log_error("rtnl: bind failed: %s", strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

static int rtnl_talk(struct nlmsghdr *nh) {
  int fd = rtnl_open();
  if (fd < 0) return -1;

  struct sockaddr_nl nladdr = { .nl_family = AF_NETLINK };
  struct iovec iov = { .iov_base = nh, .iov_len = nh->nlmsg_len };
  struct msghdr msg = { .msg_name = &nladdr, .msg_namelen = sizeof(nladdr), .msg_iov = &iov, .msg_iovlen = 1 };

  if (sendmsg(fd, &msg, 0) < 0) {
    log_error("rtnl: sendmsg failed: %s", strerror(errno));
    close(fd);
    return -1;
  }

  // Wait for ACK (NLMSGERR with error 0) or NLMSG_DONE
  char buf[NL_BUFSZ];
  struct pollfd pfd = { .fd = fd, .events = POLLIN };
  if (poll(&pfd, 1, 5000) <= 0) {
    log_error("rtnl: poll timeout");
    close(fd);
    return -1;
  }
  ssize_t len = recv(fd, buf, sizeof(buf), 0);
  if (len < 0) {
    log_error("rtnl: recv failed: %s", strerror(errno));
    close(fd);
    return -1;
  }
  struct nlmsghdr *h = (struct nlmsghdr*)buf;
  while (NLMSG_OK(h, len)) {
    if (h->nlmsg_type == NLMSG_ERROR) {
      struct nlmsgerr *err = NLMSG_DATA(h);
      if (err->error == 0) {
        close(fd);
        return 0; // ACK
      } else {
        // EEXIST for addr add is not fatal for ifup idempotence -- treat as success
        if (err->error == -EEXIST) {
          log_debug("rtnl: NLMSGERR EEXIST (already exists, treating as success)");
          close(fd);
          return 0;
        }
        log_error("rtnl: NLMSGERR %s", strerror(-err->error));
        close(fd);
        return -1;
      }
    }
    h = NLMSG_NEXT(h, len);
  }
  close(fd);
  return 0;
}

static int addattr_l(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen) {
  int len = RTA_LENGTH(alen);
  struct rtattr *rta;
  if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len) > (unsigned)maxlen) {
    log_error("rtnl: addattr_l overflow");
    return -1;
  }
  rta = (struct rtattr*)(((char*)n) + NLMSG_ALIGN(n->nlmsg_len));
  rta->rta_type = type;
  rta->rta_len = len;
  if (alen) memcpy(RTA_DATA(rta), data, alen);
  n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
  return 0;
}

static int addattr_nested_start(struct nlmsghdr *n, int maxlen, int type) {
  struct rtattr *rta = (struct rtattr*)(((char*)n) + NLMSG_ALIGN(n->nlmsg_len));
  rta->rta_type = type;
  rta->rta_len = RTA_LENGTH(0);
  n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(sizeof(*rta));
  return (char*)rta - (char*)n;
}

static void addattr_nested_end(struct nlmsghdr *n, int start) {
  struct rtattr *rta = (struct rtattr*)((char*)n + start);
  rta->rta_len = (char*)n + NLMSG_ALIGN(n->nlmsg_len) - (char*)rta;
}

int rtnl_link_up(const char *ifname) {
  unsigned ifindex = if_nametoindex(ifname);
  if (!ifindex) {
    log_error("rtnl: link_up %s: if_nametoindex failed: %s", ifname, strerror(errno));
    return -1;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  nh->nlmsg_seq = 0;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  ifi->ifi_change = IFF_UP;
  ifi->ifi_flags = IFF_UP;
  return rtnl_talk(nh);
}

int rtnl_link_down(const char *ifname) {
  unsigned ifindex = if_nametoindex(ifname);
  if (!ifindex) {
    log_error("rtnl: link_down %s: if_nametoindex failed", ifname);
    return -1;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  ifi->ifi_change = IFF_UP;
  ifi->ifi_flags = 0;
  return rtnl_talk(nh);
}

int rtnl_link_set_mtu(const char *ifname, int mtu) {
  unsigned ifindex = if_nametoindex(ifname);
  if (!ifindex) return -1;
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  addattr_l(nh, sizeof(buf), IFLA_MTU, &mtu, sizeof(mtu));
  return rtnl_talk(nh);
}

int rtnl_link_set_hwaddr(const char *ifname, const char *hwaddr) {
  unsigned ifindex = if_nametoindex(ifname);
  if (!ifindex) return -1;
  unsigned char mac[6];
  if (sscanf(hwaddr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
    log_error("rtnl: invalid hwaddr %s", hwaddr);
    return -1;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  addattr_l(nh, sizeof(buf), IFLA_ADDRESS, mac, 6);
  return rtnl_talk(nh);
}

// Parse "10.0.0.1/24" or "10.0.0.1" + netmask, returns family, addr, prefixlen
static int parse_addr(const char *address, const char *netmask, int *family, unsigned char *addr, int *prefixlen) {
  char ip[128];
  char *slash = strchr(address, '/');
  if (slash) {
    size_t iplen = slash - address;
    if (iplen >= sizeof(ip)) return -1;
    memcpy(ip, address, iplen);
    ip[iplen] = '\0';
    *prefixlen = atoi(slash+1);
  } else {
    strncpy(ip, address, sizeof(ip)-1);
    ip[sizeof(ip)-1] = '\0';
    if (netmask) {
      // netmask like 255.255.255.0 -> prefixlen
      struct in_addr nm;
      if (inet_pton(AF_INET, netmask, &nm) != 1) return -1;
      uint32_t m = ntohl(nm.s_addr);
      int len = 0;
      while (m & 0x80000000) { len++; m <<= 1; }
      *prefixlen = len;
    } else {
      *prefixlen = -1;
    }
  }
  // Try v4 then v6
  struct in_addr a4;
  struct in6_addr a6;
  if (inet_pton(AF_INET, ip, &a4) == 1) {
    *family = AF_INET;
    memcpy(addr, &a4, 4);
    if (*prefixlen < 0) *prefixlen = 32;
    return 0;
  }
  if (inet_pton(AF_INET6, ip, &a6) == 1) {
    *family = AF_INET6;
    memcpy(addr, &a6, 16);
    if (*prefixlen < 0) *prefixlen = 128;
    return 0;
  }
  return -1;
}

int rtnl_addr_add(const char *ifname, const char *address, const char *netmask) {
  unsigned ifindex = if_nametoindex(ifname);
  if (!ifindex) {
    log_error("rtnl: addr_add %s: ifindex failed", ifname);
    return -1;
  }
  int family, prefixlen;
  unsigned char addr[16];
  if (parse_addr(address, netmask, &family, addr, &prefixlen) != 0) {
    log_error("rtnl: addr_add %s: parse %s failed", ifname, address);
    return -1;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifaddrmsg *ifa;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));
  nh->nlmsg_type = RTM_NEWADDR;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
  ifa = NLMSG_DATA(nh);
  ifa->ifa_family = family;
  ifa->ifa_prefixlen = prefixlen;
  ifa->ifa_flags = 0;
  ifa->ifa_scope = 0;
  ifa->ifa_index = ifindex;
  int alen = (family == AF_INET) ? 4 : 16;
  addattr_l(nh, sizeof(buf), IFA_LOCAL, addr, alen);
  addattr_l(nh, sizeof(buf), IFA_ADDRESS, addr, alen);
  int rc = rtnl_talk(nh);
  if (rc != 0) {
    // If EEXIST, treat as success (idempotent)
    log_debug("rtnl: addr_add %s %s/%d rc=%d", ifname, address, prefixlen, rc);
  }
  return rc;
}

int rtnl_addr_del(const char *ifname, const char *address, const char *netmask) {
  unsigned ifindex = if_nametoindex(ifname);
  if (!ifindex) return -1;
  int family, prefixlen;
  unsigned char addr[16];
  if (parse_addr(address, netmask, &family, addr, &prefixlen) != 0) return -1;
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifaddrmsg *ifa;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));
  nh->nlmsg_type = RTM_DELADDR;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifa = NLMSG_DATA(nh);
  ifa->ifa_family = family;
  ifa->ifa_prefixlen = prefixlen;
  ifa->ifa_index = ifindex;
  int alen = (family == AF_INET) ? 4 : 16;
  addattr_l(nh, sizeof(buf), IFA_LOCAL, addr, alen);
  return rtnl_talk(nh);
}

int rtnl_route_add_default(const char *gateway, const char *ifname) {
  int family;
  unsigned char gw[16];
  int prefixlen;
  // gateway may be without prefix, default to /32 or /128
  if (parse_addr(gateway, NULL, &family, gw, &prefixlen) != 0) return -1;
  // For default route, we ignore prefixlen and use 0
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct rtmsg *rtm;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*rtm));
  nh->nlmsg_type = RTM_NEWROUTE;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
  rtm = NLMSG_DATA(nh);
  rtm->rtm_family = family;
  rtm->rtm_dst_len = 0;
  rtm->rtm_src_len = 0;
  rtm->rtm_tos = 0;
  rtm->rtm_table = RT_TABLE_MAIN;
  rtm->rtm_protocol = RTPROT_BOOT;
  rtm->rtm_scope = RT_SCOPE_UNIVERSE;
  rtm->rtm_type = RTN_UNICAST;
  int alen = (family == AF_INET) ? 4 : 16;
  addattr_l(nh, sizeof(buf), RTA_GATEWAY, gw, alen);
  if (ifname) {
    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex) addattr_l(nh, sizeof(buf), RTA_OIF, &ifindex, sizeof(ifindex));
  }
  return rtnl_talk(nh);
}

int rtnl_route_del_default(const char *gateway, const char *ifname) {
  int family;
  unsigned char gw[16];
  int prefixlen;
  if (parse_addr(gateway, NULL, &family, gw, &prefixlen) != 0) return -1;
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct rtmsg *rtm;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*rtm));
  nh->nlmsg_type = RTM_DELROUTE;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  rtm = NLMSG_DATA(nh);
  rtm->rtm_family = family;
  rtm->rtm_dst_len = 0;
  rtm->rtm_table = RT_TABLE_MAIN;
  int alen = (family == AF_INET) ? 4 : 16;
  addattr_l(nh, sizeof(buf), RTA_GATEWAY, gw, alen);
  if (ifname) {
    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex) addattr_l(nh, sizeof(buf), RTA_OIF, &ifindex, sizeof(ifindex));
  }
  return rtnl_talk(nh);
}

int rtnl_vlan_create(const char *name, const char *rawdev, int vlan_id) {
  unsigned rawidx = if_nametoindex(rawdev);
  if (!rawidx) {
    log_error("rtnl: vlan %s: rawdev %s not found", name, rawdev);
    return -1;
  }
  // Check if already exists
  if (if_nametoindex(name) != 0) {
    log_info("rtnl: vlan %s already exists", name);
    return 0;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  addattr_l(nh, sizeof(buf), IFLA_IFNAME, name, strlen(name)+1);
  addattr_l(nh, sizeof(buf), IFLA_LINK, &rawidx, sizeof(rawidx));
  int nest = addattr_nested_start(nh, sizeof(buf), IFLA_LINKINFO);
  addattr_l(nh, sizeof(buf), IFLA_INFO_KIND, "vlan", 5);
  int nest2 = addattr_nested_start(nh, sizeof(buf), IFLA_INFO_DATA);
  addattr_l(nh, sizeof(buf), IFLA_VLAN_ID, &vlan_id, sizeof(vlan_id));
  addattr_nested_end(nh, nest2);
  addattr_nested_end(nh, nest);
  return rtnl_talk(nh);
}

int rtnl_bridge_create(const char *name) {
  if (if_nametoindex(name) != 0) {
    log_info("rtnl: bridge %s already exists", name);
    return 0;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  addattr_l(nh, sizeof(buf), IFLA_IFNAME, name, strlen(name)+1);
  int nest = addattr_nested_start(nh, sizeof(buf), IFLA_LINKINFO);
  addattr_l(nh, sizeof(buf), IFLA_INFO_KIND, "bridge", 7);
  addattr_nested_end(nh, nest);
  return rtnl_talk(nh);
}

// Create a VRF device bound to a routing table.
//
// A VRF is an L3 master device: enslaved interfaces have their routes looked
// up in IFLA_VRF_TABLE instead of main. The table id is not decoration -- it
// IS the separation, which is why it is mandatory here rather than defaulted.
// Requires CONFIG_NET_VRF (shipped as a module, see the kernel templates).
int rtnl_vrf_create(const char *name, uint32_t table) {
  if (!name || !table) return -1;
  if (if_nametoindex(name) != 0) {
    log_info("rtnl: vrf %s already exists", name);
    return 0;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  addattr_l(nh, sizeof(buf), IFLA_IFNAME, name, strlen(name)+1);
  int nest = addattr_nested_start(nh, sizeof(buf), IFLA_LINKINFO);
  addattr_l(nh, sizeof(buf), IFLA_INFO_KIND, "vrf", 4);
  int nest2 = addattr_nested_start(nh, sizeof(buf), IFLA_INFO_DATA);
  addattr_l(nh, sizeof(buf), IFLA_VRF_TABLE, &table, sizeof(table));
  addattr_nested_end(nh, nest2);
  addattr_nested_end(nh, nest);
  log_info("rtnl: vrf %s table %u", name, table);
  return rtnl_talk(nh);
}

// Enslave an interface to a VRF. Same IFLA_MASTER mechanism as bridge
// enslavement -- the kernel distinguishes them by the master's kind.
int rtnl_vrf_add_port(const char *vrf, const char *port) {
  unsigned vidx = if_nametoindex(vrf);
  unsigned pidx = if_nametoindex(port);
  if (!vidx || !pidx) {
    log_error("rtnl: vrf_add_port %s -> %s: ifindex not found (%u, %u)", port, vrf, pidx, vidx);
    return -1;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = pidx;
  addattr_l(nh, sizeof(buf), IFLA_MASTER, &vidx, sizeof(vidx));
  log_info("rtnl: vrf %s enslave %s", vrf, port);
  return rtnl_talk(nh);
}

int rtnl_bridge_set_stp(const char *name, bool on) {
  unsigned ifindex = if_nametoindex(name);
  if (!ifindex) return -1;
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  int nest = addattr_nested_start(nh, sizeof(buf), IFLA_LINKINFO);
  addattr_l(nh, sizeof(buf), IFLA_INFO_KIND, "bridge", 7);
  int nest2 = addattr_nested_start(nh, sizeof(buf), IFLA_INFO_DATA);
  uint8_t stp = on ? 1 : 0;
  addattr_l(nh, sizeof(buf), IFLA_BR_STP_STATE, &stp, sizeof(stp));
  addattr_nested_end(nh, nest2);
  addattr_nested_end(nh, nest);
  return rtnl_talk(nh);
}

int rtnl_bridge_set_vlan_aware(const char *name, bool on) {
  unsigned ifindex = if_nametoindex(name);
  if (!ifindex) return -1;
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = ifindex;
  int nest = addattr_nested_start(nh, sizeof(buf), IFLA_LINKINFO);
  addattr_l(nh, sizeof(buf), IFLA_INFO_KIND, "bridge", 7);
  int nest2 = addattr_nested_start(nh, sizeof(buf), IFLA_INFO_DATA);
  uint8_t vlan = on ? 1 : 0;
  addattr_l(nh, sizeof(buf), IFLA_BR_VLAN_FILTERING, &vlan, sizeof(vlan));
  addattr_nested_end(nh, nest2);
  addattr_nested_end(nh, nest);
  return rtnl_talk(nh);
}

int rtnl_bridge_add_port(const char *br, const char *port) {
  unsigned bridx = if_nametoindex(br);
  unsigned pidx = if_nametoindex(port);
  if (!bridx || !pidx) {
    log_error("rtnl: bridge_add_port %s -> %s: ifindex not found (%u, %u)", port, br, pidx, bridx);
    return -1;
  }
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = pidx;
  addattr_l(nh, sizeof(buf), IFLA_MASTER, &bridx, sizeof(bridx));
  return rtnl_talk(nh);
}

int rtnl_bridge_del_port(const char *br, const char *port) {
  (void)br;
  unsigned pidx = if_nametoindex(port);
  if (!pidx) return -1;
  char buf[NL_BUFSZ];
  struct nlmsghdr *nh = (struct nlmsghdr*)buf;
  struct ifinfomsg *ifi;
  memset(buf, 0, sizeof(buf));
  nh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
  nh->nlmsg_type = RTM_NEWLINK;
  nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  ifi = NLMSG_DATA(nh);
  ifi->ifi_family = AF_UNSPEC;
  ifi->ifi_index = pidx;
  // No master = 0 removes from bridge? Actually need to set master 0 and use NLM_F?
  // For del, we set master to 0 and use RTM_NEWLINK with master 0? The kernel handles master 0 as no bridge.
  // Alternative is to use RTM_DELLINK? No, enslavement is via master.
  // Setting master to 0 and sending NEWLINK should release.
  // But to be safe, we send with master 0 and hope.
  // If that doesn't work, we can try to use `ip link set dev port nomaster`
  uint32_t zero = 0;
  addattr_l(nh, sizeof(buf), IFLA_MASTER, &zero, sizeof(zero));
  return rtnl_talk(nh);
}
