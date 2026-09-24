#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "finwo/resp.h"
#include "rxi/log.h"

#include "config/daemon.h"
#include "config/ifaces.h"
#include "config/ports.h"
#include "ipc.h"
#include "netlink/netlink.h"
#include "netlink/rtnl.h"
#include "util/auth.h"

#define RBUF_MAX (1024 * 1024)

struct listener {
  int   fd;
  int   is_unix;
  char *path;      // unix only, for unlink on shutdown
};

struct ipc_conn {
  int    fd;
  int    is_unix;
  int    role;
  uid_t  uid;

  char  *rbuf; size_t rlen, rcap;
  char  *wbuf; size_t wlen, wcap, wsent;

  int    close_after_write;
  struct ipc_conn *next;
};

static struct listener  listeners[IPC_MAX_LISTENERS];
static int              listener_n = 0;
static struct ipc_conn *conns      = NULL;
static int              conn_n     = 0;

static int wbuf_append(struct ipc_conn *c, const char *data, size_t len) {
  if (c->wlen + len > c->wcap) {
    size_t cap = c->wcap ? c->wcap : 1024;
    while (cap < c->wlen + len) cap *= 2;
    char *g = realloc(c->wbuf, cap);
    if (!g) return -1;
    c->wbuf = g;
    c->wcap = cap;
  }
  memcpy(c->wbuf + c->wlen, data, len);
  c->wlen += len;
  return 0;
}

static void reply_obj(struct ipc_conn *c, resp_object *o) {
  if (!o) return;
  char  *buf = NULL;
  size_t len = 0;
  if (resp_serialize(o, &buf, &len) == 0) {
    wbuf_append(c, buf, len);
    free(buf);
  }
  resp_free(o);
}

static void reply_ok(struct ipc_conn *c) {
  reply_obj(c, resp_simple_init("OK"));
}

static void reply_err(struct ipc_conn *c, const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  reply_obj(c, resp_error_init(msg));
}

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

static int do_ifup(struct ipc_conn *c, const char *ifname) {
  struct linkd_iface *iface = ifaces_find(ifname);
  if (!iface) {
    log_info("ipc: ifup %s (not in interfaces, just bringing link up)", ifname);
    if (rtnl_link_up(ifname) != 0) {
      reply_err(c, "ifup %s failed", ifname);
      return -1;
    }
    return 0;
  }
  log_info("ipc: ifup %s", ifname);
  if (iface->pre_up && run_hook(iface->pre_up) != 0) {
    reply_err(c, "pre-up hook failed for %s", ifname);
    return -1;
  }

  if (iface->vlan_id >= 0) {
    const char *raw = iface->vlan_raw_device;
    char tmp[IF_NAMESIZE];
    if (!raw) {
      char *dot = strrchr(iface->name, '.');
      if (dot) {
        size_t len = (size_t)(dot - iface->name);
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

  // `ifup <vrf>` alone must work, not only a full apply.
  if (iface_is_vrf(iface)) {
    log_info("ipc: ifup %s vrf table %d", ifname, iface->vrf_table);
    rtnl_vrf_create(ifname, (uint32_t)iface->vrf_table);
  }

  if (iface_is_bridge(iface)) {
    log_info("ipc: ifup %s bridge (ports %s)", ifname,
             iface->bridge_ports ? iface->bridge_ports : "(none)");
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

  // Enslave before addresses land: entering a VRF flushes them.
  if (iface->vrf_master) {
    rtnl_vrf_add_port(iface->vrf_master, ifname);
  }
  if (rtnl_link_up(ifname) != 0) {
    reply_err(c, "ifup %s: link up failed", ifname);
    return -1;
  }
  for (struct iface_addr *a = iface->addrs; a; a = a->next) {
    log_info("ipc: ifup %s addr %s", ifname, a->address);
    rtnl_addr_add(ifname, a->address, a->netmask);
  }
  if (iface->gateway) {
    log_info("ipc: ifup %s gateway %s", ifname, iface->gateway);
    rtnl_route_add_default(iface->gateway, ifname);
  }
  if (iface->mtu && rtnl_link_set_mtu(ifname, iface->mtu) != 0) {
    reply_err(c, "ifup %s: mtu failed", ifname);
    return -1;
  }
  if (iface->hwaddress && rtnl_link_set_hwaddr(ifname, iface->hwaddress) != 0) {
    reply_err(c, "ifup %s: hwaddress failed", ifname);
    return -1;
  }
  if (iface->post_up && run_hook(iface->post_up) != 0) {
    reply_err(c, "post-up hook failed for %s", ifname);
    return -1;
  }
  return 0;
}

static int do_ifdown(struct ipc_conn *c, const char *ifname) {
  struct linkd_iface *iface = ifaces_find(ifname);
  if (!iface) {
    log_info("ipc: ifdown %s (not in interfaces, just bringing link down)", ifname);
    if (rtnl_link_down(ifname) != 0) {
      reply_err(c, "ifdown %s failed", ifname);
      return -1;
    }
    return 0;
  }
  log_info("ipc: ifdown %s", ifname);
  if (iface->pre_down && run_hook(iface->pre_down) != 0) {
    reply_err(c, "pre-down hook failed for %s", ifname);
    return -1;
  }
  if (rtnl_link_down(ifname) != 0) {
    reply_err(c, "ifdown %s failed", ifname);
    return -1;
  }
  if (iface->post_down && run_hook(iface->post_down) != 0) {
    reply_err(c, "post-down hook failed for %s", ifname);
    return -1;
  }
  return 0;
}

static int count_ifaces(void) {
  int n = 0;
  for (struct linkd_iface *c = ifaces_list(); c; c = c->next) n++;
  return n;
}

static int do_ifreload(struct ipc_conn *c) {
  log_info("ipc: ifreload start, old list has %d", count_ifaces());
  // TODO: real diff-apply; this frees and re-parses.
  ifaces_free();
  ports_free();

  const struct linkd_daemon_cfg *dcfg = daemon_cfg();
  if (load_namespace("interfaces", dcfg->iface, dcfg->iface_n) < 0) {
    reply_err(c, "ifreload: failed to reload interface configuration");
    return -1;
  }
  if (load_namespace("ports", dcfg->ports, dcfg->ports_n) < 0) {
    reply_err(c, "ifreload: failed to reload port configuration");
    return -1;
  }
  log_info("ipc: ifreload done, new list has %d", count_ifaces());
  return 0;
}

// Flat key/value pairs (RESP2 map). Keys may repeat: several addresses.
static void iface_to_map(resp_object *m, const struct linkd_iface *i) {
  char num[16];
  resp_array_append_bulk(m, "name");
  resp_array_append_bulk(m, i->name);
  if (i->method) {
    resp_array_append_bulk(m, "method");
    resp_array_append_bulk(m, i->method);
  }
  resp_array_append_bulk(m, "auto");
  resp_array_append_bulk(m, i->auto_flag ? "yes" : "no");
  for (struct iface_addr *a = i->addrs; a; a = a->next) {
    resp_array_append_bulk(m, "address");
    resp_array_append_bulk(m, a->address);
    if (a->netmask) {
      resp_array_append_bulk(m, "netmask");
      resp_array_append_bulk(m, a->netmask);
    }
  }
  if (i->gateway)          { resp_array_append_bulk(m, "gateway");   resp_array_append_bulk(m, i->gateway); }
  if (i->mtu)              { snprintf(num, sizeof(num), "%d", i->mtu);
                             resp_array_append_bulk(m, "mtu");       resp_array_append_bulk(m, num); }
  if (i->hwaddress)        { resp_array_append_bulk(m, "hwaddress"); resp_array_append_bulk(m, i->hwaddress); }
  if (i->vlan_raw_device)  { resp_array_append_bulk(m, "vlan-raw-device"); resp_array_append_bulk(m, i->vlan_raw_device); }
  if (i->vlan_id >= 0)     { snprintf(num, sizeof(num), "%d", i->vlan_id);
                             resp_array_append_bulk(m, "vlan-id");   resp_array_append_bulk(m, num); }
  if (i->bridge_ports)     { resp_array_append_bulk(m, "bridge-ports"); resp_array_append_bulk(m, i->bridge_ports); }
  if (i->bridge_stp)       { resp_array_append_bulk(m, "bridge-stp");   resp_array_append_bulk(m, i->bridge_stp); }
  if (i->bridge_vlan_aware >= 0) {
    resp_array_append_bulk(m, "bridge-vlan-aware");
    resp_array_append_bulk(m, i->bridge_vlan_aware ? "yes" : "no");
  }
  if (i->vrf_table >= 0)   { snprintf(num, sizeof(num), "%d", i->vrf_table);
                             resp_array_append_bulk(m, "vrf-table"); resp_array_append_bulk(m, num); }
  if (i->vrf_master)       { resp_array_append_bulk(m, "vrf");       resp_array_append_bulk(m, i->vrf_master); }
  if (i->pre_up)           { resp_array_append_bulk(m, "pre-up");    resp_array_append_bulk(m, i->pre_up); }
  if (i->post_up)          { resp_array_append_bulk(m, "post-up");   resp_array_append_bulk(m, i->post_up); }
  if (i->pre_down)         { resp_array_append_bulk(m, "pre-down");  resp_array_append_bulk(m, i->pre_down); }
  if (i->post_down)        { resp_array_append_bulk(m, "post-down"); resp_array_append_bulk(m, i->post_down); }
}

static int do_ifquery(struct ipc_conn *c, const char *ifname) {
  if (ifname) {
    struct linkd_iface *i = ifaces_find(ifname);
    if (!i) {
      reply_err(c, "no such interface: %s", ifname);
      return -1;
    }
    resp_object *m = resp_array_init();
    iface_to_map(m, i);
    reply_obj(c, m);
    return 0;
  }

  resp_object *outer = resp_array_init();
  for (struct linkd_iface *i = ifaces_list(); i; i = i->next) {
    resp_object *m = resp_array_init();
    iface_to_map(m, i);
    resp_array_append_obj(outer, m);
  }
  reply_obj(c, outer);
  return 0;
}

struct command {
  const char *name;
  int         min_args;   // including the verb
  int         max_args;   // -1 = unlimited
  int         role;       // minimum role
};

static const struct command commands[] = {
  { "PING",     1, 2, IPC_ROLE_NONE     },
  { "INFO",     1, 1, IPC_ROLE_READONLY },
  { "COMMAND",  1, 2, IPC_ROLE_READONLY },
  { "IFQUERY",  1, 2, IPC_ROLE_READONLY },
  { "IFUP",     2, 2, IPC_ROLE_FULL     },
  { "IFDOWN",   2, 2, IPC_ROLE_FULL     },
  { "IFRELOAD", 1, 1, IPC_ROLE_FULL     },
  { "AUTH",     2, 3, IPC_ROLE_NONE     },
  { "QUIT",     1, 1, IPC_ROLE_NONE     },
};
static const size_t commands_n = sizeof(commands) / sizeof(commands[0]);

static const char *arg(const resp_object *cmd, size_t i) {
  if (!cmd || cmd->type != RESPT_ARRAY || i >= cmd->u.arr.n) return NULL;
  const resp_object *e = &cmd->u.arr.elem[i];
  if (e->type != RESPT_BULK && e->type != RESPT_SIMPLE) return NULL;
  return e->u.s;
}

static void do_info(struct ipc_conn *c) {
  char buf[512];
  snprintf(buf, sizeof(buf),
           "# Server\r\n"
           "name:linkd\r\n"
           "version:" __TARGET "\r\n"
           "\r\n"
           "# Clients\r\n"
           "connected_clients:%d\r\n"
           "\r\n"
           "# Config\r\n"
           "interfaces:%d\r\n",
           conn_n, count_ifaces());
  resp_object *o = calloc(1, sizeof(*o));
  if (!o) return;
  o->type = RESPT_BULK;
  o->u.s  = strdup(buf);
  reply_obj(c, o);
}

static void do_command_list(struct ipc_conn *c) {
  resp_object *a = resp_array_init();
  for (size_t i = 0; i < commands_n; i++) {
    resp_object *e = resp_array_init();
    resp_array_append_bulk(e, commands[i].name);
    resp_array_append_int(e, commands[i].max_args < 0
                              ? -commands[i].min_args : commands[i].max_args);
    resp_array_append_obj(a, e);
  }
  reply_obj(c, a);
}

static void do_auth(struct ipc_conn *c, const resp_object *cmd) {
  const struct linkd_daemon_cfg *cfg = daemon_cfg();
  if (!cfg->authfile) {
    reply_err(c, "Client sent AUTH, but no password file is configured");
    return;
  }

  const char *user = arg(cmd, 1);
  const char *pass = arg(cmd, 2);
  if (!pass) { pass = user; user = "default"; }
  if (!user || !pass) {
    reply_err(c, "wrong number of arguments for 'AUTH'");
    return;
  }

  int role = auth_check(cfg->authfile, user, pass);
  if (role == AUTH_ROLE_NONE) {
    log_warn("ipc: failed AUTH for user '%s'", user);
    reply_err(c, "WRONGPASS invalid username-password pair");
    return;
  }

  c->role = (role == AUTH_ROLE_FULL) ? IPC_ROLE_FULL : IPC_ROLE_READONLY;
  log_info("ipc: user '%s' authenticated (%s)", user,
           c->role == IPC_ROLE_FULL ? "full" : "readonly");
  reply_ok(c);
}

static void dispatch(struct ipc_conn *c, const resp_object *cmd) {
  const char *verb = arg(cmd, 0);
  if (!verb) {
    reply_err(c, "ERR malformed command");
    return;
  }

  size_t argc = (cmd->type == RESPT_ARRAY) ? cmd->u.arr.n : 0;

  const struct command *spec = NULL;
  for (size_t i = 0; i < commands_n; i++) {
    if (!strcasecmp(verb, commands[i].name)) { spec = &commands[i]; break; }
  }
  if (!spec) {
    reply_err(c, "unknown command '%s'", verb);
    return;
  }

  if ((int)argc < spec->min_args ||
      (spec->max_args >= 0 && (int)argc > spec->max_args)) {
    reply_err(c, "wrong number of arguments for '%s'", verb);
    return;
  }

  if (c->role < spec->role) {
    reply_err(c, c->role == IPC_ROLE_NONE
                   ? "NOAUTH Authentication required"
                   : "NOPERM this command requires full access");
    return;
  }

  if (!strcasecmp(verb, "PING")) {
    const char *msg = arg(cmd, 1);
    if (msg) {
      resp_object *o = calloc(1, sizeof(*o));
      if (o) { o->type = RESPT_BULK; o->u.s = strdup(msg); reply_obj(c, o); }
    } else {
      reply_obj(c, resp_simple_init("PONG"));
    }
  } else if (!strcasecmp(verb, "QUIT")) {
    reply_ok(c);
    c->close_after_write = 1;
  } else if (!strcasecmp(verb, "INFO")) {
    do_info(c);
  } else if (!strcasecmp(verb, "COMMAND")) {
    do_command_list(c);
  } else if (!strcasecmp(verb, "AUTH")) {
    do_auth(c, cmd);
  } else if (!strcasecmp(verb, "IFQUERY")) {
    do_ifquery(c, arg(cmd, 1));
  } else if (!strcasecmp(verb, "IFUP")) {
    if (do_ifup(c, arg(cmd, 1)) == 0) reply_ok(c);
  } else if (!strcasecmp(verb, "IFDOWN")) {
    if (do_ifdown(c, arg(cmd, 1)) == 0) reply_ok(c);
  } else if (!strcasecmp(verb, "IFRELOAD")) {
    if (do_ifreload(c) == 0) reply_ok(c);
  }
}

static void conn_close(struct ipc_conn *c) {
  struct ipc_conn **p = &conns;
  while (*p && *p != c) p = &(*p)->next;
  if (*p) *p = c->next;
  if (c->fd >= 0) close(c->fd);
  free(c->rbuf);
  free(c->wbuf);
  free(c);
  conn_n--;
}

static void conn_accept(struct listener *l) {
  int fd = accept4(l->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0) return;

  if (conn_n >= IPC_MAX_CONNS) {
    log_warn("ipc: connection limit reached, rejecting");
    close(fd);
    return;
  }

  struct ipc_conn *c = calloc(1, sizeof(*c));
  if (!c) { close(fd); return; }
  c->fd      = fd;
  c->is_unix = l->is_unix;
  c->role    = IPC_ROLE_NONE;

  // Root over unix is pre-authenticated; the kernel vouches for the uid.
  if (l->is_unix) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
      c->uid = cred.uid;
      if (cred.uid == 0) c->role = IPC_ROLE_FULL;
    }
  }

  c->next = conns;
  conns   = c;
  conn_n++;
}

static void conn_read(struct ipc_conn *c) {
  char buf[4096];
  for (;;) {
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n == 0) { conn_close(c); return; }
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      conn_close(c);
      return;
    }
    if (c->rlen + (size_t)n > RBUF_MAX) {
      log_warn("ipc: request too large, dropping client");
      conn_close(c);
      return;
    }
    if (c->rlen + (size_t)n > c->rcap) {
      size_t cap = c->rcap ? c->rcap : 4096;
      while (cap < c->rlen + (size_t)n) cap *= 2;
      char *g = realloc(c->rbuf, cap);
      if (!g) { conn_close(c); return; }
      c->rbuf = g;
      c->rcap = cap;
    }
    memcpy(c->rbuf + c->rlen, buf, (size_t)n);
    c->rlen += (size_t)n;
    if ((size_t)n < sizeof(buf)) break;
  }

  // Drain every complete command; this is what makes pipelining work.
  for (;;) {
    resp_object *cmd = NULL;
    int used = resp_read_buf(c->rbuf, c->rlen, &cmd);
    if (used <= 0) {
      // 0 = nothing yet, <0 = partial; wait for more data.
      if (cmd) resp_free(cmd);
      break;
    }
    dispatch(c, cmd);
    resp_free(cmd);

    memmove(c->rbuf, c->rbuf + used, c->rlen - (size_t)used);
    c->rlen -= (size_t)used;
    if (c->rlen == 0) break;
  }
}

static void conn_write(struct ipc_conn *c) {
  while (c->wsent < c->wlen) {
    ssize_t n = write(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      conn_close(c);
      return;
    }
    c->wsent += (size_t)n;
  }
  c->wlen = c->wsent = 0;
  if (c->close_after_write) conn_close(c);
}

static int listen_unix(const char *path) {
  struct sockaddr_un addr;

  {
    char *d = strdup(path);
    if (d) { mkdir(dirname(d), 0755); free(d); }
  }
  unlink(path);

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    log_error("ipc: socket: %s", strerror(errno));
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    log_error("ipc: bind %s: %s", path, strerror(errno));
    close(fd);
    return -1;
  }
  if (chmod(path, 0600) != 0) {
    log_warn("ipc: chmod %s: %s", path, strerror(errno));
  }
  if (listen(fd, 8) != 0) {
    log_error("ipc: listen %s: %s", path, strerror(errno));
    close(fd);
    unlink(path);
    return -1;
  }
  return fd;
}

static int listen_tcp(const char *hostport) {
  char *copy = strdup(hostport);
  if (!copy) return -1;

  char *host = copy;
  char *port = strrchr(copy, ':');
  if (port) { *port++ = '\0'; } else { port = (char *)"6789"; }
  if (*host == '\0') host = NULL;

  struct addrinfo hints = {
    .ai_family   = AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
    .ai_flags    = AI_PASSIVE,
  };
  struct addrinfo *res = NULL;
  int gai = getaddrinfo(host, port, &hints, &res);
  if (gai != 0) {
    log_error("ipc: resolve %s: %s", hostport, gai_strerror(gai));
    free(copy);
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 8) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  free(copy);

  if (fd < 0) log_error("ipc: cannot listen on %s", hostport);
  return fd;
}

int ipc_init(void) {
  const struct linkd_daemon_cfg *cfg = daemon_cfg();
  int ok = 0;

  for (size_t i = 0; i < cfg->listen_n && listener_n < IPC_MAX_LISTENERS; i++) {
    const char *a = cfg->listen[i];
    int   is_unix = !strncmp(a, "unix://", 7);
    int   fd;

    if (is_unix) {
      fd = listen_unix(a + 7);
    } else {
      // No way to authorise a tcp client without one; SO_PEERCRED is
      // unix-only.
      if (!cfg->authfile) {
        log_error("ipc: %s needs `authfile` configured; refusing to listen", a);
        continue;
      }
      fd = listen_tcp(a + 6);
    }
    if (fd < 0) continue;

    listeners[listener_n].fd      = fd;
    listeners[listener_n].is_unix = is_unix;
    listeners[listener_n].path    = is_unix ? strdup(a + 7) : NULL;
    listener_n++;
    ok++;
    log_info("ipc: listening on %s", a);
  }

  return ok ? 0 : -1;
}

void ipc_fini(void) {
  while (conns) conn_close(conns);
  for (int i = 0; i < listener_n; i++) {
    if (listeners[i].fd >= 0) close(listeners[i].fd);
    if (listeners[i].path) {
      unlink(listeners[i].path);
      free(listeners[i].path);
    }
  }
  listener_n = 0;
}

int ipc_pollfds(struct pollfd *pfds, int max) {
  int n = 0;
  for (int i = 0; i < listener_n && n < max; i++) {
    pfds[n].fd      = listeners[i].fd;
    pfds[n].events  = POLLIN;
    pfds[n].revents = 0;
    n++;
  }
  for (struct ipc_conn *c = conns; c && n < max; c = c->next) {
    pfds[n].fd      = c->fd;
    pfds[n].events  = POLLIN | (c->wsent < c->wlen ? POLLOUT : 0);
    pfds[n].revents = 0;
    n++;
  }
  return n;
}

void ipc_dispatch(struct pollfd *pfds, int n) {
  for (int i = 0; i < n; i++) {
    if (!pfds[i].revents) continue;

    int is_listener = 0;
    for (int l = 0; l < listener_n; l++) {
      if (listeners[l].fd == pfds[i].fd) {
        conn_accept(&listeners[l]);
        is_listener = 1;
        break;
      }
    }
    if (is_listener) continue;

    // Look up by fd: an earlier fd may have closed and freed this one.
    struct ipc_conn *c = conns;
    while (c && c->fd != pfds[i].fd) c = c->next;
    if (!c) continue;

    if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { conn_close(c); continue; }
    if (pfds[i].revents & POLLIN)  conn_read(c);

    // conn_read may have closed it; re-find before writing.
    struct ipc_conn *still = conns;
    while (still && still != c) still = still->next;
    if (!still) continue;

    if (c->wsent < c->wlen) conn_write(c);
  }
}
