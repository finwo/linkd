#define _GNU_SOURCE
#include <libgen.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/types.h>

#include "rxi/log.h"

#include "util/config.h"
#include "config/daemon.h"
#include "config/ifaces.h"
#include "config/ports.h"
#include "ipc.h"
#include "netlink/netlink.h"
#include "netlink/rtnl.h"

static int ipc_sock = -1;
static const char *ipc_path = NULL;

int ipc_fd(void) {
  return ipc_sock;
}

int ipc_init(void) {
  struct sockaddr_un addr;

  ipc_path = daemon_cfg_unix_socket();
  if (!ipc_path) {
    log_error("ipc: no unix:// listen address configured");
    return -1;
  }

  // Create the socket's directory; early boot may not have mounted it yet.
  {
    char *d = strdup(ipc_path);
    if (d) { mkdir(dirname(d), 0755); free(d); }
  }
  unlink(ipc_path);

  ipc_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (ipc_sock < 0) {
    log_error("ipc: socket failed: %s", strerror(errno));
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, ipc_path, sizeof(addr.sun_path)-1);

  if (bind(ipc_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    log_error("ipc: bind %s failed: %s", ipc_path, strerror(errno));
    close(ipc_sock);
    ipc_sock = -1;
    return -1;
  }

  if (chmod(ipc_path, 0600) != 0) {
    log_warn("ipc: chmod %s failed: %s", ipc_path, strerror(errno));
  }

  if (listen(ipc_sock, 8) != 0) {
    log_error("ipc: listen failed: %s", strerror(errno));
    close(ipc_sock);
    ipc_sock = -1;
    unlink(ipc_path);
    return -1;
  }

  log_info("ipc: listening on %s", ipc_path);
  return 0;
}

void ipc_fini(void) {
  if (ipc_sock >= 0) {
    close(ipc_sock);
    ipc_sock = -1;
  }
  unlink(ipc_path);
}

// Helpers to send response
static void ipc_send_ok(FILE *out, const char *msg) {
  if (msg) fprintf(out, "OK %s\n", msg);
  else fprintf(out, "OK\n");
}

static void ipc_send_err(FILE *out, const char *msg) {
  fprintf(out, "ERR %s\n", msg ? msg : "unknown error");
}

// Forward declarations for handlers (implemented below, using netlink helpers)
static int handle_ifup(FILE *out, const char *ifname, uid_t uid);
static int handle_ifdown(FILE *out, const char *ifname, uid_t uid);
static int handle_ifreload(FILE *out, uid_t uid);
static int handle_ifquery(FILE *out, const char *ifname);

// IPC namespace handlers -- these are registered with cfg_ns_get("ipc")
// They are invoked via cfg_parse on the connected socket's FILE*
static struct cnf_directive *cfg_handle_ifup(FILE *fd, struct cnf_directive *dir, void *user) {
  FILE *out = (FILE*)user;
  (void)fd;
  log_info("ipc: cfg_handle_ifup argc=%d", dir->argc);
  uid_t uid = 0;
  extern uid_t ipc_current_uid;
  uid = ipc_current_uid;
  if (uid != 0) {
    ipc_send_err(out, "permission denied: ifup requires root");
    cnf_directive_free(dir);
    return NULL;
  }
  if (dir->argc != 1) {
    ipc_send_err(out, "ifup expects 1 arg: <ifname>");
    cnf_directive_free(dir);
    return NULL;
  }
  log_info("ipc: cfg_handle_ifup %s uid=%d", dir->argv[0], uid);
  if (handle_ifup(out, dir->argv[0], uid) == 0) ipc_send_ok(out, NULL);
  else log_info("ipc: handle_ifup %s failed", dir->argv[0]);
  cnf_directive_free(dir);
  return NULL;
}

static struct cnf_directive *cfg_handle_ifdown(FILE *fd, struct cnf_directive *dir, void *user) {
  FILE *out = (FILE*)user;
  (void)fd;
  extern uid_t ipc_current_uid;
  uid_t uid = ipc_current_uid;
  if (uid != 0) {
    ipc_send_err(out, "permission denied: ifdown requires root");
    cnf_directive_free(dir);
    return NULL;
  }
  if (dir->argc != 1) {
    ipc_send_err(out, "ifdown expects 1 arg: <ifname>");
    cnf_directive_free(dir);
    return NULL;
  }
  if (handle_ifdown(out, dir->argv[0], uid) == 0) ipc_send_ok(out, NULL);
  cnf_directive_free(dir);
  return NULL;
}

static struct cnf_directive *cfg_handle_ifreload(FILE *fd, struct cnf_directive *dir, void *user) {
  FILE *out = (FILE*)user;
  (void)fd;
  extern uid_t ipc_current_uid;
  uid_t uid = ipc_current_uid;
  if (uid != 0) {
    ipc_send_err(out, "permission denied: ifreload requires root");
    cnf_directive_free(dir);
    return NULL;
  }
  if (dir->argc != 0) {
    ipc_send_err(out, "ifreload expects no args");
    cnf_directive_free(dir);
    return NULL;
  }
  if (handle_ifreload(out, uid) == 0) ipc_send_ok(out, NULL);
  cnf_directive_free(dir);
  return NULL;
}

static struct cnf_directive *cfg_handle_ifquery(FILE *fd, struct cnf_directive *dir, void *user) {
  FILE *out = (FILE*)user;
  (void)fd;
  log_info("ipc: ifquery argc=%d", dir->argc);
  const char *ifname = NULL;
  if (dir->argc > 1) {
    ipc_send_err(out, "ifquery expects 0 or 1 arg");
    cnf_directive_free(dir);
    return NULL;
  }
  if (dir->argc == 1) ifname = dir->argv[0];
  log_info("ipc: ifquery %s", ifname ? ifname : "(all)");
  handle_ifquery(out, ifname);
  cnf_directive_free(dir);
  return NULL;
}

// Global for current uid (single-threaded, one client at a time)
uid_t ipc_current_uid = 0;

int ipc_handle(void) {
  int cfd;
  struct sockaddr_un addr;
  socklen_t alen = sizeof(addr);
  struct ucred cred;
  socklen_t clen = sizeof(cred);
  FILE *fin = NULL;
  FILE *fout = NULL;
  int fd_dup;

  // The LISTENING socket is non-blocking (it lives in the nl_run poll set), but
  // the ACCEPTED connection must be blocking: the client connects and writes its
  // command as two separate syscalls, so a non-blocking read here returns EAGAIN
  // whenever the bytes have not landed yet. stdio maps that to EOF, cfg_parse
  // sees an empty stream, no handler runs and the client gets an empty reply.
  // That is a race with no upper bound in wall-clock terms - it is not fixable
  // by sleeping in the caller, only by blocking here.
  cfd = accept4(ipc_sock, (struct sockaddr*)&addr, &alen, SOCK_CLOEXEC);
  if (cfd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    log_error("ipc: accept failed: %s", strerror(errno));
    return -1;
  }

  // Blocking, but never indefinitely: linkd is single-threaded, so a client that
  // connects and then stalls would otherwise wedge the whole daemon (netlink
  // included). A short timeout bounds that without reintroducing the race.
  {
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  // Get peer credentials
  if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0) {
    ipc_current_uid = cred.uid;
  } else {
    ipc_current_uid = 99999;
  }

  // Use fdopen for reading and writing -- need separate FILE* for read and write to handle duplex?
  // We'll use one FILE* for reading commands, and the fd directly for writing responses.
  // Simpler: use fdopen for reading, and use fdopen dup for writing.
  fd_dup = dup(cfd);
  if (fd_dup < 0) {
    close(cfd);
    return -1;
  }
  fin = fdopen(cfd, "r");
  fout = fdopen(fd_dup, "w");
  if (!fin || !fout) {
    if (fin) fclose(fin); else close(cfd);
    if (fout) fclose(fout); else close(fd_dup);
    return -1;
  }

  // Set line buffering
  setvbuf(fout, NULL, _IONBF, 0);

  // Parse one or more commands from this connection (each line is a command)
  // Use cfg_parse on the ipc namespace
  struct cfg_ns *ns = cfg_ns_get("ipc");
  // cfg_parse will read directives from fin and dispatch to handlers, which write to fout
  // It handles multiple lines until EOF.
  cfg_parse(ns, NULL, fin, fout);

  // Ensure OK is sent if handler didn't already? Our handlers send OK/ERR themselves.
  // Flush and close
  fflush(fout);
  fclose(fin); // also closes cfd
  fclose(fout);
  ipc_current_uid = 0;
  return 0;
}

// Register IPC commands
void ipc_register(void) {
  struct cfg_ns *ns = cfg_ns_get("ipc");
  cfg_register_directive(ns, "ifup", cfg_handle_ifup);
  cfg_register_directive(ns, "ifdown", cfg_handle_ifdown);
  cfg_register_directive(ns, "ifreload", cfg_handle_ifreload);
  cfg_register_directive(ns, "ifquery", cfg_handle_ifquery);
}

// Stubs for netlink helpers (to be implemented in netlink.c or here)
// For now, provide minimal implementations that just log and return OK
// The real implementation will be wired in the next commit (Wire netlink apply)

#include <sys/wait.h>

static int run_hook(const char *cmd) {
  if (!cmd || !*cmd) return 0;
  log_info("ipc: running hook: %s", cmd);
  int rc = system(cmd);
  if (rc != 0) {
    log_warn("hook `%s` failed with %d", cmd, rc);
    return -1;
  }
  return 0;
}

static int handle_ifup(FILE *out, const char *ifname, uid_t uid) {
  (void)uid;
  struct linkd_iface *iface = ifaces_find(ifname);
  if (!iface) {
    log_info("ipc: ifup %s (not in interfaces, just bringing link up)", ifname);
    if (rtnl_link_up(ifname) != 0) {
      fprintf(out, "ERR ifup %s failed\n", ifname);
      return -1;
    }
    return 0;
  }
  log_info("ipc: ifup %s", ifname);
  if (iface->pre_up && run_hook(iface->pre_up) != 0) {
    fprintf(out, "ERR pre-up hook failed for %s\n", ifname);
    return -1;
  }
  // VLAN creation first (needs rawdev)
  if (iface->vlan_id >= 0) {
    const char *raw = iface->vlan_raw_device;
    char tmp[IF_NAMESIZE];
    if (!raw) {
      char *dot = strrchr(iface->name, '.');
      if (dot) {
        size_t len = dot - iface->name;
        if (len < sizeof(tmp)) {
          memcpy(tmp, iface->name, len);
          tmp[len] = '\0';
          raw = tmp;
        }
      }
    }
    if (raw) {
      log_info("ipc: ifup %s vlan %d on %s", ifname, iface->vlan_id, raw);
      rtnl_vlan_create(ifname, raw, iface->vlan_id);
    }
  }
  // VRF device: create before anything else touches it. `ifup <vrf>` on its
  // own must work, not only a full apply.
  if (iface_is_vrf(iface)) {
    log_info("ipc: ifup %s vrf table %d", ifname, iface->vrf_table);
    rtnl_vrf_create(ifname, (uint32_t)iface->vrf_table);
  }
  // Bridge creation -- classification shared with apply_interfaces, see
  // config/ifaces.c:iface_is_bridge().
  if (iface_is_bridge(iface)) {
    log_info("ipc: ifup %s bridge (ports %s)", ifname, iface->bridge_ports ? iface->bridge_ports : "(none)");
    rtnl_bridge_create(ifname);
    if (iface->bridge_stp) {
      bool on = !strcasecmp(iface->bridge_stp, "on") || !strcasecmp(iface->bridge_stp, "yes");
      rtnl_bridge_set_stp(ifname, on);
    }
    if (iface->bridge_vlan_aware >= 0) {
      rtnl_bridge_set_vlan_aware(ifname, iface->bridge_vlan_aware);
    }
    if (iface->bridge_ports && strcasecmp(iface->bridge_ports, "none") != 0) {
      char *ports = strdup(iface->bridge_ports);
      char *tok = strtok(ports, " \t");
      while (tok) {
        if (strcasecmp(tok, "none") == 0) { tok = strtok(NULL, " \t"); continue; }
        log_info("ipc: ifup %s bridge add port %s", ifname, tok);
        rtnl_bridge_add_port(ifname, tok);
        rtnl_link_up(tok);
        tok = strtok(NULL, " \t");
      }
      free(ports);
    }
  }
  // Bring link up
  // Enslave before the link comes up and addresses land: moving an interface
  // into a VRF flushes its addresses, so doing it afterwards would silently
  // discard what we just configured.
  if (iface->vrf_master) {
    rtnl_vrf_add_port(iface->vrf_master, ifname);
  }
  if (rtnl_link_up(ifname) != 0) {
    fprintf(out, "ERR ifup %s: link up failed\n", ifname);
    return -1;
  }
  // Addresses
  for (struct iface_addr *a = iface->addrs; a; a = a->next) {
    log_info("ipc: ifup %s addr %s", ifname, a->address);
    rtnl_addr_add(ifname, a->address, a->netmask);
  }
  if (iface->gateway) {
    log_info("ipc: ifup %s gateway %s", ifname, iface->gateway);
    rtnl_route_add_default(iface->gateway, ifname);
  }
  if (iface->mtu) {
    if (rtnl_link_set_mtu(ifname, iface->mtu) != 0) {
      fprintf(out, "ERR ifup %s: mtu failed\n", ifname);
      return -1;
    }
  }
  if (iface->hwaddress) {
    if (rtnl_link_set_hwaddr(ifname, iface->hwaddress) != 0) {
      fprintf(out, "ERR ifup %s: hwaddress failed\n", ifname);
      return -1;
    }
  }
  if (iface->post_up && run_hook(iface->post_up) != 0) {
    fprintf(out, "ERR post-up hook failed for %s\n", ifname);
    return -1;
  }
  return 0;
}

static int handle_ifdown(FILE *out, const char *ifname, uid_t uid) {
  (void)uid;
  struct linkd_iface *iface = ifaces_find(ifname);
  if (!iface) {
    log_info("ipc: ifdown %s (not in interfaces, just bringing link down)", ifname);
    if (rtnl_link_down(ifname) != 0) {
      fprintf(out, "ERR ifdown %s failed\n", ifname);
      return -1;
    }
    return 0;
  }
  log_info("ipc: ifdown %s", ifname);
  if (iface->pre_down && run_hook(iface->pre_down) != 0) {
    fprintf(out, "ERR pre-down hook failed for %s\n", ifname);
    return -1;
  }
  if (rtnl_link_down(ifname) != 0) {
    fprintf(out, "ERR ifdown %s failed\n", ifname);
    return -1;
  }
  // Flush addresses (keep for now)
  if (iface->post_down && run_hook(iface->post_down) != 0) {
    fprintf(out, "ERR post-down hook failed for %s\n", ifname);
    return -1;
  }
  return 0;
}

static int count_ifaces(void) {
  int n = 0;
  for (struct linkd_iface *c = ifaces_list(); c; c = c->next) n++;
  return n;
}

static int handle_ifreload(FILE *out, uid_t uid) {
  (void)uid;
  log_info("ipc: ifreload (diff-apply) start, old list has %d", count_ifaces());
  // For now, just re-parse and apply all auto interfaces
  // TODO: implement diff-apply: compare old vs new lists
  ifaces_free();
  ports_free();
  log_info("ipc: ifreload after free, list has %d", count_ifaces());

  // Same loader startup uses. Was a private copy hardcoding /etc/network,
  // so --config was silently ignored here.
  const struct linkd_daemon_cfg *dcfg = daemon_cfg();
  if (load_namespace("interfaces", dcfg->iface, dcfg->iface_n) < 0) {
    ipc_send_err(out, "ifreload: failed to reload interface configuration");
    return -1;
  }
  if (load_namespace("ports", dcfg->ports, dcfg->ports_n) < 0) {
    ipc_send_err(out, "ifreload: failed to reload port configuration");
    return -1;
  }
  {
    int n = 0;
    for (struct linkd_iface *c = ifaces_list(); c; c = c->next) n++;
    log_info("ipc: ifreload done, new list has %d", n);
    for (struct linkd_iface *c = ifaces_list(); c; c = c->next) {
      log_info("ipc: ifreload iface %s", c->name);
    }
  }
  return 0;
}

static int handle_ifquery(FILE *out, const char *ifname) {
  struct linkd_iface *cur;
  int n = 0;
  for (cur = ifaces_list(); cur; cur = cur->next) n++;
  log_info("ipc: ifquery %s (list has %d)", ifname ? ifname : "(all)", n);
  if (ifname) {
    cur = ifaces_find(ifname);
    log_info("ipc: ifquery find %s -> %s", ifname, cur ? "found" : "not found");
    if (!cur) {
      fprintf(out, "ERR no such interface: %s\n", ifname);
      return -1;
    }
    // Dump this iface in cnf style
    fprintf(out, "iface %s\n", cur->name);
    for (struct iface_addr *a = cur->addrs; a; a = a->next) {
      fprintf(out, "  address %s\n", a->address);
      if (a->netmask) fprintf(out, "  netmask %s\n", a->netmask);
    }
    if (cur->gateway) fprintf(out, "  gateway %s\n", cur->gateway);
    if (cur->mtu) fprintf(out, "  mtu %d\n", cur->mtu);
    if (cur->hwaddress) fprintf(out, "  hwaddress %s\n", cur->hwaddress);
    if (cur->vlan_raw_device) fprintf(out, "  vlan-raw-device %s\n", cur->vlan_raw_device);
    if (cur->vlan_id >= 0) fprintf(out, "  vlan-id %d\n", cur->vlan_id);
    if (cur->bridge_ports) fprintf(out, "  bridge-ports %s\n", cur->bridge_ports);
    if (cur->bridge_stp) fprintf(out, "  bridge-stp %s\n", cur->bridge_stp);
    if (cur->bridge_vlan_aware >= 0)
      fprintf(out, "  bridge-vlan-aware %s\n", cur->bridge_vlan_aware ? "yes" : "no");
    if (cur->vrf_table >= 0) fprintf(out, "  vrf-table %d\n", cur->vrf_table);
    if (cur->vrf_master) fprintf(out, "  vrf %s\n", cur->vrf_master);
    if (cur->pre_up) fprintf(out, "  pre-up %s\n", cur->pre_up);
    if (cur->post_up) fprintf(out, "  post-up %s\n", cur->post_up);
    if (cur->pre_down) fprintf(out, "  pre-down %s\n", cur->pre_down);
    if (cur->post_down) fprintf(out, "  post-down %s\n", cur->post_down);
    fprintf(out, "OK\n");
  } else {
    for (cur = ifaces_list(); cur; cur = cur->next) {
      fprintf(out, "iface %s", cur->name);
      if (cur->method) fprintf(out, " %s", cur->method);
      fprintf(out, "\n");
      for (struct iface_addr *a = cur->addrs; a; a = a->next) {
        fprintf(out, "  address %s\n", a->address);
      }
      if (cur->gateway) fprintf(out, "  gateway %s\n", cur->gateway);
    }
    fprintf(out, "OK\n");
  }
  return 0;
}
