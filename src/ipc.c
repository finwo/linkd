#define _GNU_SOURCE
#include <errno.h>
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
#include "config/ifaces.h"
#include "ipc.h"
#include "netlink/netlink.h"

static int ipc_sock = -1;

int ipc_fd(void) {
  return ipc_sock;
}

int ipc_init(void) {
  struct sockaddr_un addr;
  // Ensure /run exists (tmpfs mount in rcS, but early boot may not have it)
  mkdir("/run", 0755);
  unlink(UNOS_IPC_PATH);

  ipc_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (ipc_sock < 0) {
    log_error("ipc: socket failed: %s", strerror(errno));
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, UNOS_IPC_PATH, sizeof(addr.sun_path)-1);

  if (bind(ipc_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    log_error("ipc: bind %s failed: %s", UNOS_IPC_PATH, strerror(errno));
    close(ipc_sock);
    ipc_sock = -1;
    return -1;
  }

  if (chmod(UNOS_IPC_PATH, 0600) != 0) {
    log_warn("ipc: chmod %s failed: %s", UNOS_IPC_PATH, strerror(errno));
  }

  if (listen(ipc_sock, 8) != 0) {
    log_error("ipc: listen failed: %s", strerror(errno));
    close(ipc_sock);
    ipc_sock = -1;
    unlink(UNOS_IPC_PATH);
    return -1;
  }

  log_info("ipc: listening on %s", UNOS_IPC_PATH);
  return 0;
}

void ipc_fini(void) {
  if (ipc_sock >= 0) {
    close(ipc_sock);
    ipc_sock = -1;
  }
  unlink(UNOS_IPC_PATH);
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
  log_info("ipc: handle called");
  int cfd;
  struct sockaddr_un addr;
  socklen_t alen = sizeof(addr);
  struct ucred cred;
  socklen_t clen = sizeof(cred);
  FILE *fin = NULL;
  FILE *fout = NULL;
  int fd_dup;

  cfd = accept4(ipc_sock, (struct sockaddr*)&addr, &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (cfd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    log_error("ipc: accept failed: %s", strerror(errno));
    return -1;
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
  struct unos_iface *iface = ifaces_find(ifname);
  if (!iface) {
    log_info("ipc: ifup %s (not in interfaces, just bringing link up)", ifname);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip link set %s up 2>&1", ifname);
    int rc = system(cmd);
    if (rc != 0) {
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
  // VLAN handling: if iface is vlan, ensure raw device exists and create vlan link
  // For now, just bring link up and add addresses via netlink helpers (to be implemented)
  // Placeholder: use system ip commands via netlink abstraction
  // TODO: wire real netlink RTM_NEWLINK/NEWADDR
  char cmd[512];
  int rc;
  // Bring link up
  snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s up", ifname);
  rc = system(cmd);
  log_info("ipc: ifup %s link up rc=%d cmd='%s'", ifname, rc, cmd);
  if (rc != 0) { fprintf(out, "ERR ifup %s: link up failed (%d)\n", ifname, rc); return -1; }
  // Add addresses
  struct iface_addr *a;
  for (a = iface->addrs; a; a = a->next) {
    snprintf(cmd, sizeof(cmd), "/sbin/ip addr add %s dev %s 2>&1", a->address, ifname);
    rc = system(cmd);
    log_info("ipc: ifup %s addr %s rc=%d", ifname, a->address, rc);
    // File exists is not fatal (addr already present)
    if (rc != 0 && rc != 256) { /* 256 = File exists from ip */ }
  }
  if (iface->gateway) {
    snprintf(cmd, sizeof(cmd), "/sbin/ip route add default via %s 2>&1 || true", iface->gateway);
    rc = system(cmd);
    log_info("ipc: ifup %s gateway %s rc=%d", ifname, iface->gateway, rc);
  }
  if (iface->mtu) {
    snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s mtu %d", ifname, iface->mtu);
    rc = system(cmd);
    log_info("ipc: ifup %s mtu %d rc=%d", ifname, iface->mtu, rc);
    if (rc != 0) { fprintf(out, "ERR ifup %s: mtu failed\n", ifname); return -1; }
  }
  if (iface->hwaddress) {
    snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s address %s", ifname, iface->hwaddress);
    rc = system(cmd);
    log_info("ipc: ifup %s hwaddress %s rc=%d", ifname, iface->hwaddress, rc);
    if (rc != 0) { fprintf(out, "ERR ifup %s: hwaddress failed\n", ifname); return -1; }
  }
  if (iface->post_up && run_hook(iface->post_up) != 0) {
    fprintf(out, "ERR post-up hook failed for %s\n", ifname);
    return -1;
  }
  return 0;
}

static int handle_ifdown(FILE *out, const char *ifname, uid_t uid) {
  (void)uid;
  struct unos_iface *iface = ifaces_find(ifname);
  if (!iface) {
    log_info("ipc: ifdown %s (not in interfaces, just bringing link down)", ifname);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip link set %s down 2>&1", ifname);
    int rc = system(cmd);
    if (rc != 0) {
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
  char cmd[512];
  int rc2;
  snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s down", ifname);
  rc2 = system(cmd);
  log_info("ipc: ifdown %s rc=%d", ifname, rc2);
  if (rc2 != 0) { fprintf(out, "ERR ifdown %s failed (%d)\n", ifname, rc2); return -1; }
  // Flush addresses (keep for now)
  if (iface->post_down && run_hook(iface->post_down) != 0) {
    fprintf(out, "ERR post-down hook failed for %s\n", ifname);
    return -1;
  }
  return 0;
}

static int handle_ifreload(FILE *out, uid_t uid) {
  (void)uid;
  log_info("ipc: ifreload (diff-apply)");
  // For now, just re-parse and apply all auto interfaces
  // TODO: implement diff-apply: compare old vs new lists
  // Placeholder: reload config files
  // We need to free old and re-parse
  ifaces_free();
  // Re-register is idempotent, but we need to reload
  // Use same logic as main.c load_interfaces
  char *dir = "/etc/network";
  char path[1024];
  char stub[1024];
  FILE *fd;
  snprintf(path, sizeof(path), "%s/interfaces", dir);
  if (access(path, R_OK) == 0) {
    fd = fopen(path, "r");
    if (fd) {
      cfg_parse(cfg_ns_get("interfaces"), dir, fd, NULL);
      fclose(fd);
    }
  } else {
    snprintf(stub, sizeof(stub), "source interfaces.d/*.cnf\n");
    fd = fmemopen(stub, strlen(stub), "r");
    if (fd) {
      cfg_parse(cfg_ns_get("interfaces"), dir, fd, NULL);
      fclose(fd);
    }
  }
  // Also reload ports?
  snprintf(path, sizeof(path), "%s/ports", dir);
  if (access(path, R_OK) == 0) {
    fd = fopen(path, "r");
    if (fd) {
      cfg_parse(cfg_ns_get("ports"), dir, fd, NULL);
      fclose(fd);
    }
  } else {
    snprintf(stub, sizeof(stub), "source ports.d/*.cnf\n");
    fd = fmemopen(stub, strlen(stub), "r");
    if (fd) {
      cfg_parse(cfg_ns_get("ports"), dir, fd, NULL);
      fclose(fd);
    }
  }
  return 0;
}

static int handle_ifquery(FILE *out, const char *ifname) {
  struct unos_iface *cur;
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
