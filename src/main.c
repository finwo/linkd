#define _GNU_SOURCE

#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "cofyc/argparse.h"
#include "rxi/log.h"

#include "config/ports.h"
#include "config/ifaces.h"
#include "dataplane.h"
#include "dataplane/registry.h"
#include "ipc.h"
#include "netlink/netlink.h"
#include "util/config.h"

static const char *const usage[] = {
  __NAME " [options]",
  __NAME " --help",
  NULL,
};

static FILE *log_file;
static char *log_path;
static volatile sig_atomic_t sighup_received;

static void logfile_callback(log_Event *ev) {
  if (sighup_received) {
    sighup_received = 0;
    if (log_path && log_file) {
      fclose(log_file);
      log_file = fopen(log_path, "a");
    }
  }
  if (log_file) {
    char buf[64];
    buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
    fprintf(log_file, "%s %-5s %s:%d: ", buf, log_level_string(ev->level), ev->file, ev->line);
    vfprintf(log_file, ev->fmt, ev->ap);
    fprintf(log_file, "\n");
    fflush(log_file);
  }
}

static void sighup_handler(int sig) {
  (void)sig;
  sighup_received = 1;
}

static void stop_handler(int sig) {
  (void)sig;
  nl_request_stop();
}

// Port and interface configuration live in either:
//   <dir>/ports, <dir>/interfaces      single files, or
//   <dir>/ports.d/*.cnf, <dir>/interfaces.d/*.cnf  drop-ins
//
// Rather than growing the config API, the drop-in case is handed to the parser
// as an in-memory config consisting of `source` -- the parser expands it.
static int load_ports(const char *dir) {
  char  path[1024];
  char  stub[1024];
  FILE *fd;
  int   rc;

  snprintf(path, sizeof(path), "%s/ports", dir);

  if (access(path, R_OK) == 0) {
    fd = fopen(path, "r");
    if (!fd) {
      perror("Could not open config file");
      return CFG_RET_ERROR;
    }
  } else {
    snprintf(stub, sizeof(stub), "source ports.d/*.cnf\n");
    fd = fmemopen(stub, strlen(stub), "r");
    if (!fd) {
      perror("Could not open in-memory config");
      return CFG_RET_ERROR;
    }
  }

  rc = cfg_parse(cfg_ns_get("ports"), dir, fd, NULL);
  if (rc < 0) {
    log_error("Error during reading port configuration from %s", dir);
  }

  fclose(fd);
  return rc;
}

static int load_interfaces(const char *dir) {
  char  path[1024];
  char  stub[1024];
  FILE *fd;
  int   rc;

  snprintf(path, sizeof(path), "%s/interfaces", dir);

  if (access(path, R_OK) == 0) {
    fd = fopen(path, "r");
    if (!fd) {
      perror("Could not open config file");
      return CFG_RET_ERROR;
    }
  } else {
    snprintf(stub, sizeof(stub), "source interfaces.d/*.cnf\n");
    fd = fmemopen(stub, strlen(stub), "r");
    if (!fd) {
      perror("Could not open in-memory config");
      return CFG_RET_ERROR;
    }
  }

  rc = cfg_parse(cfg_ns_get("interfaces"), dir, fd, NULL);
  if (rc < 0) {
    log_error("Error during reading interfaces configuration from %s", dir);
  }

  fclose(fd);
  return rc;
}

static int apply_ports(void) {
  const struct dp_ops *dp = dp_active();
  struct unos_port    *cur;
  struct dp_port       port;

  if (!dp || !dp->port_apply) return 0;

  for ( cur = ports_list() ; cur ; cur = cur->next ) {
    memset(&port, 0, sizeof(port));
    port.name    = cur->name;
    port.speed   = cur->speed;
    port.fec     = cur->fec;
    port.autoneg = cur->autoneg;

    if (dp->port_apply(&port) != DP_RET_OK) {
      log_warn("%s: failed to apply port configuration", cur->name);
    }
  }

  return 0;
}

static int apply_interfaces(void) {
  struct unos_iface *cur;
  for (cur = ifaces_list(); cur; cur = cur->next) {
    if (!cur->auto_flag) continue;
    // For now, use ipc's handle logic via system ip -- will be replaced with netlink
    // We call the same helper as ipc ifup would, but directly
    char cmd[512];
    // pre-up hook
    if (cur->pre_up) {
      log_info("apply_interfaces: pre-up %s: %s", cur->name, cur->pre_up);
      if (system(cur->pre_up) != 0) {
        log_warn("pre-up for %s failed", cur->name);
        continue;
      }
    }
    // VLAN creation if needed (dot notation)
    if (cur->vlan_id >= 0) {
      const char *raw = cur->vlan_raw_device;
      if (!raw) {
        // Infer from dot notation
        char *dot = strrchr(cur->name, '.');
        if (dot) {
          size_t len = dot - cur->name;
          char *tmp = malloc(len+1);
          strncpy(tmp, cur->name, len);
          tmp[len] = '\0';
          raw = tmp;
          snprintf(cmd, sizeof(cmd), "/sbin/ip link add link %s name %s type vlan id %d 2>&1 || true", raw, cur->name, cur->vlan_id);
          log_info("apply_interfaces: %s", cmd);
          system(cmd);
          free(tmp);
        }
      } else {
        snprintf(cmd, sizeof(cmd), "/sbin/ip link add link %s name %s type vlan id %d 2>&1 || true", raw, cur->name, cur->vlan_id);
        log_info("apply_interfaces: %s", cmd);
        system(cmd);
      }
    }
    // Bring link up
    snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s up 2>&1 || true", cur->name);
    log_info("apply_interfaces: %s", cmd);
    system(cmd);
    // Add addresses (v4 and v6)
    struct iface_addr *a;
    for (a = cur->addrs; a; a = a->next) {
      snprintf(cmd, sizeof(cmd), "/sbin/ip addr add %s dev %s 2>&1 || true", a->address, cur->name);
      log_info("apply_interfaces: %s", cmd);
      system(cmd);
    }
    if (cur->gateway) {
      snprintf(cmd, sizeof(cmd), "/sbin/ip route add default via %s 2>&1 || true", cur->gateway);
      log_info("apply_interfaces: %s", cmd);
      system(cmd);
    }
    if (cur->mtu) {
      snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s mtu %d 2>&1 || true", cur->name, cur->mtu);
      log_info("apply_interfaces: %s", cmd);
      system(cmd);
    }
    if (cur->hwaddress) {
      snprintf(cmd, sizeof(cmd), "/sbin/ip link set %s address %s 2>&1 || true", cur->name, cur->hwaddress);
      log_info("apply_interfaces: %s", cmd);
      system(cmd);
    }
    if (cur->post_up) {
      log_info("apply_interfaces: post-up %s: %s", cur->name, cur->post_up);
      system(cur->post_up);
    }
  }
  return 0;
}

// Atomic publish: write tmp + rename so frr never observes a half file.
static int ready_publish(const char *path) {
  char tmp[1024];
  FILE *fd;

  if (!path || !*path) return 0;
  snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
  fd = fopen(tmp, "w");
  if (!fd) {
    log_error("ready-file %s: %s", tmp, strerror(errno));
    return -1;
  }
  fprintf(fd, "%d\n", (int)getpid());
  fclose(fd);
  if (rename(tmp, path) != 0) {
    log_error("ready-file rename: %s", strerror(errno));
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void ready_remove(const char *path) {
  if (!path || !*path) return;
  unlink(path);
}

int main(int argc, const char **argv) {
  char *config_dir = "/etc/network";
  char *dataplane  = NULL;
  char *loglevel = "info";
  char *logfile_path = NULL;
  char *ready_file = "";
  int   resync_interval = 60;
  int   nl_fd = -1;
  int   rc = 0;

  struct argparse_option options[] = {
    OPT_HELP(),
    OPT_STRING('c', "config", &config_dir, "Configuration directory", NULL, 0, 0),
    OPT_STRING('d', "dataplane", &dataplane, "Force a dataplane backend", NULL, 0, 0),
    OPT_STRING('v', "verbosity", &loglevel, "log verbosity: fatal,error,warn,info,debug,trace (default: info)", NULL, 0, 0),
    OPT_STRING(0, "log", &logfile_path, "also write log to file (SIGHUP reopens for logrotate)", NULL, 0, 0),
    OPT_INTEGER(0, "resync-interval", &resync_interval, "seconds between full resyncs (0 disables timer)", NULL, 0, 0),
    OPT_STRING(0, "ready-file", &ready_file, "readiness file to publish after initial resync (empty disables)", NULL, 0, 0),
    OPT_END(),
  };

  // Initialize components
  ports_register();
  ifaces_register();
  ipc_register();

  struct argparse argparse;
  argparse_init(&argparse, options, usage, 0);
  argparse_describe(&argparse, NULL,
      "\n"
      __NAME " programs the forwarding plane from the kernel's routing state.\n"
      "Built for target " __TARGET ".\n"
  );
  argc = argparse_parse(&argparse, argc, argv);

  int level = LOG_INFO;
  if (0) {
    (void)0;
  } else if (!strcasecmp(loglevel, "trace")) {
    level = LOG_TRACE;
  } else if (!strcasecmp(loglevel, "debug")) {
    level = LOG_DEBUG;
  } else if (!strcasecmp(loglevel, "info")) {
    level = LOG_INFO;
  } else if (!strcasecmp(loglevel, "warn")) {
    level = LOG_WARN;
  } else if (!strcasecmp(loglevel, "error")) {
    level = LOG_ERROR;
  } else if (!strcasecmp(loglevel, "fatal")) {
    level = LOG_FATAL;
  } else {
    fprintf(stderr, "Unknown log level: %s\n", loglevel);
    return 1;
  }
  log_set_level(level);
  setvbuf(stderr, NULL, _IOLBF, 0);

  log_file = NULL;
  log_path = NULL;
  sighup_received = 0;

  if (logfile_path && logfile_path[0]) {
    log_path = strdup(logfile_path);
    log_file = fopen(log_path, "a");
    if (log_file) {
      log_add_callback(logfile_callback, log_path, level);
    } else {
      fprintf(stderr, "Could not open log file: %s\n", logfile_path);
      free(log_path);
      log_path = NULL;
    }
  }

  if (resync_interval < 0) {
    log_error("--resync-interval must be >= 0");
    return 1;
  }

  signal(SIGHUP, sighup_handler);
  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  signal(SIGPIPE, SIG_IGN);

  if (load_ports(config_dir) < 0) {
    return 1;
  }
  if (load_interfaces(config_dir) < 0) {
    return 1;
  }
  {
    int n = 0;
    for (struct unos_iface *c = ifaces_list(); c; c = c->next) n++;
    log_info("main: loaded %d interfaces from %s", n, config_dir);
    for (struct unos_iface *c = ifaces_list(); c; c = c->next) {
      log_info("main: iface %s method %s auto %d addrs %s", c->name, c->method ? c->method : "(null)", c->auto_flag, c->addrs ? c->addrs->address : "(none)");
    }
  }

  // Startup order is load-bearing: ports -> backend select -> init ->
  // port_apply -> nl_open -> initial resync -> ready -> event loop. frr must
  // not start before the ready file exists or zebra misses the swpN netdevs.
  //
  // The built-in kernel backend is registered first so it acts as the
  // fallback; plugins installed by hardware packages are probed ahead of it.
  dp_register(dp_kernel_ops());
  dp_plugins_load(UNOS_DATAPLANE_DIR);

  if (dp_select(dataplane) != DP_RET_OK) {
    return 1;
  }

  if (dp_init() != DP_RET_OK) {
    log_error("dataplane: initialisation failed");
    return 1;
  }

  if (ipc_init() != 0) {
    log_warn("ipc: init failed, ifup/ifquery will not be available");
  }

  apply_ports();
  // Interfaces are applied via netlink/system ip after ports, before resync
  // so that the subsequent nl_resync sees them.

  nl_fd = nl_open();
  if (nl_fd < 0) {
    dp_fini();
    ports_free();
    ifaces_free();
    ipc_fini();
    return 1;
  }

  // Apply auto interfaces (needs netlink to be open for link creation, but we use system ip for now)
  apply_interfaces();

  // Fail loud: never publish readiness on partial state; runit retries.
  if (nl_resync(nl_fd) != 0) {
    log_fatal("netlink: initial resync failed");
    close(nl_fd);
    ipc_fini();
    dp_fini();
    ports_free();
    ifaces_free();
    return 1;
  }

  if (ready_file && *ready_file) {
    if (ready_publish(ready_file) != 0) {
      close(nl_fd);
      ipc_fini();
      dp_fini();
      ports_free();
      ifaces_free();
      return 1;
    }
    log_info("ready: published %s", ready_file);
  }

  rc = nl_run(nl_fd, resync_interval);
  close(nl_fd);
  ipc_fini();

  ready_remove(ready_file);
  dp_fini();
  ports_free();
  ifaces_free();

  if (log_file) fclose(log_file);
  free(log_path);

  return rc < 0 ? 1 : 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
