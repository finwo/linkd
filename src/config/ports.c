#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "finwo/cnfparse.h"

#include "util/config.h"

#include "ports.h"

static struct unos_port *ports = NULL;
static struct unos_port *ports_tail = NULL;

struct unos_port * ports_list(void) {
  return ports;
}

struct unos_port * ports_find(const char *name) {
  struct unos_port *cur;
  for ( cur = ports ; cur ; cur = cur->next ) {
    if (!strcmp(cur->name, name)) return cur;
  }
  return NULL;
}

void ports_free(void) {
  struct unos_port *cur = ports;
  struct unos_port *next;

  while (cur) {
    next = cur->next;
    if (cur->name)     free(cur->name);
    if (cur->breakout) free(cur->breakout);
    free(cur);
    cur = next;
  }

  ports      = NULL;
  ports_tail = NULL;
}

static enum dp_fec ports_parse_fec(const char *value) {
  if (!strcasecmp("off"  , value)) return DP_FEC_OFF;
  if (!strcasecmp("none" , value)) return DP_FEC_OFF;
  if (!strcasecmp("auto" , value)) return DP_FEC_AUTO;
  if (!strcasecmp("rs"   , value)) return DP_FEC_RS;
  if (!strcasecmp("baser", value)) return DP_FEC_BASER;
  if (!strcasecmp("fc"   , value)) return DP_FEC_BASER;
  return DP_FEC_UNSET;
}

static int ports_parse_bool(const char *value) {
  if (!strcasecmp("on"  , value)) return 1;
  if (!strcasecmp("yes" , value)) return 1;
  if (!strcasecmp("true", value)) return 1;
  if (!strcasecmp("1"   , value)) return 1;
  return 0;
}

// Directives ifupdown already implements. Accepting them here would create a
// second source of truth for the same value, so reject them loudly and point
// at the right file rather than silently ignoring them.
static const char *ports_reserved[] = {
  "address", "gateway", "netmask", "broadcast", "metric",
  "mtu", "hwaddress", "admin", "up", "down",
  NULL,
};

static int ports_is_reserved(const char *name) {
  int i;
  for ( i = 0 ; ports_reserved[i] ; i++ ) {
    if (!strcasecmp(ports_reserved[i], name)) return 1;
  }
  return 0;
}

struct cnf_directive * cfg_parse_port(FILE *fd, struct cnf_directive *dir, void *user) {
  struct unos_port *port;
  (void)user;

  if (dir->argc != 1) {
    fprintf(stderr, "`port` directive accepts only 1 argument\n");
    exit(1);
  }

  if (ports_find(dir->argv[0])) {
    fprintf(stderr, "duplicate `port` definition: %s\n", dir->argv[0]);
    exit(1);
  }

  port = calloc(1, sizeof(struct unos_port));
  port->name    = strdup(dir->argv[0]);
  port->fec     = DP_FEC_UNSET;
  port->autoneg = -1;

  for(;;) {
    if (dir) cnf_directive_free(dir);
    dir = cnf_directive_read(fd);
    if (!dir) break; // EOF = done

    // Make following blocks identical in structure
    if(0) {}

    else if (!strcasecmp("speed", dir->name)) {
      if (dir->argc != 1) {
        fprintf(stderr, "`speed` directive accepts only 1 argument\n");
        exit(1);
      }
      port->speed = (uint32_t)strtoul(dir->argv[0], NULL, 10);
    }

    else if (!strcasecmp("fec", dir->name)) {
      if (dir->argc != 1) {
        fprintf(stderr, "`fec` directive accepts only 1 argument\n");
        exit(1);
      }
      port->fec = ports_parse_fec(dir->argv[0]);
      if (port->fec == DP_FEC_UNSET) {
        fprintf(stderr, "unknown `fec` value: %s\n", dir->argv[0]);
        exit(1);
      }
    }

    else if (!strcasecmp("autoneg", dir->name)) {
      if (dir->argc != 1) {
        fprintf(stderr, "`autoneg` directive accepts only 1 argument\n");
        exit(1);
      }
      port->autoneg = ports_parse_bool(dir->argv[0]);
    }

    else if (!strcasecmp("breakout", dir->name)) {
      if (dir->argc != 1) {
        fprintf(stderr, "`breakout` directive accepts only 1 argument\n");
        exit(1);
      }
      if (port->breakout) free(port->breakout);
      port->breakout = strdup(dir->argv[0]);
    }

    else if (ports_is_reserved(dir->name)) {
      fprintf(stderr,
              "`%s` is handled by ifupdown; configure it in "
              "/etc/network/interfaces.d/ instead of ports.cnf\n",
              dir->name);
      exit(1);
    }

    else {
      // unknown directive
      break;
    }
  }

  // Store the completed port
  if (ports_tail) {
    ports_tail->next = port;
  } else {
    ports = port;
  }
  ports_tail = port;

  // Return the unparsed dir if set
  return dir;
}

void ports_register(void) {
  cfg_register_directive("port", cfg_parse_port);
}
