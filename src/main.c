#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "cofyc/argparse.h"

#include "config/ports.h"
#include "dataplane.h"
#include "dataplane/registry.h"
#include "util/config.h"

static const char *const usage[] = {
  __NAME " [options]",
  __NAME " --help",
  NULL,
};

// Port configuration lives in either:
//   <dir>/ports.cnf      a single file, or
//   <dir>/ports.d/*.cnf  a drop-in directory
//
// Rather than growing the config API with a glob entry point, the drop-in case
// is handed to the parser as an in-memory config consisting of nothing but an
// `include` -- the parser already knows how to expand those.
static int load_ports(const char *dir) {
  char  path[1024];
  char  stub[1024];
  FILE *fd;
  int   rc;

  snprintf(path, sizeof(path), "%s/ports.cnf", dir);

  if (access(path, R_OK) == 0) {
    fd = fopen(path, "r");
    if (!fd) {
      perror("Could not open config file");
      return CFG_RET_ERROR;
    }
  } else {
    snprintf(stub, sizeof(stub), "include ports.d/*.cnf\n");
    fd = fmemopen(stub, strlen(stub), "r");
    if (!fd) {
      perror("Could not open in-memory config");
      return CFG_RET_ERROR;
    }
  }

  rc = cfg_parse(dir, fd, NULL);
  if (rc < 0) {
    fprintf(stderr, "Error during reading port configuration from %s\n", dir);
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
      fprintf(stderr, "%s: failed to apply port configuration\n", cur->name);
    }
  }

  return 0;
}

int main(int argc, const char **argv) {
  char *config_dir = "/etc/unos";
  char *dataplane  = NULL;
  int   rc         = 0;

  struct argparse_option options[] = {
    OPT_HELP(),
    OPT_STRING('c', "config", &config_dir, "Configuration directory", NULL, 0, 0),
    OPT_STRING('d', "dataplane", &dataplane, "Force a dataplane backend", NULL, 0, 0),
    OPT_END(),
  };

  // Initialize components
  ports_register();

  struct argparse argparse;
  argparse_init(&argparse, options, usage, 0);
  argparse_describe(&argparse, NULL,
      "\n"
      __NAME " programs the forwarding plane from the kernel's routing state.\n"
      "Built for target " __TARGET ".\n"
  );
  argc = argparse_parse(&argparse, argc, argv);

  if (load_ports(config_dir) < 0) {
    return 1;
  }

  // The built-in kernel backend is registered first so it acts as the
  // fallback; plugins installed by hardware packages are probed ahead of it.
  dp_register(dp_kernel_ops());
  dp_plugins_load(UNOS_DATAPLANE_DIR);

  if (dp_select(dataplane) != DP_RET_OK) {
    return 1;
  }

  if (dp_init() != DP_RET_OK) {
    fprintf(stderr, "dataplane: initialisation failed\n");
    return 1;
  }

  apply_ports();

  // TODO: netlink event loop -- subscribe to RTNLGRP_LINK / NEIGH / IPV4_ROUTE
  //       / IPV6_ROUTE, mirror into the active dataplane, and periodically
  //       call dp_active()->resync() to recover from dropped messages.

  dp_fini();
  ports_free();

  return rc;
}

#ifdef __cplusplus
} // extern "C"
#endif
