#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "finwo/cnfparse.h"

#include "util/config.h"

#include "ports.h"

static struct linkd_port *ports = NULL;
static struct linkd_port *ports_tail = NULL;

struct linkd_port * ports_list(void) {
  return ports;
}

struct linkd_port * ports_find(const char *name) {
  struct linkd_port *cur;
  for ( cur = ports ; cur ; cur = cur->next ) {
    if (!strcmp(cur->name, name)) return cur;
  }
  return NULL;
}

void ports_free(void) {
  struct linkd_port *cur = ports;
  struct linkd_port *next;

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

static void port_free(struct linkd_port *port) {
  if (!port) return;
  free(port->name);
  free(port->breakout);
  free(port);
}

struct cnf_directive * cfg_parse_port(FILE *fd, struct cnf_directive *dir, void *user) {
  struct linkd_port *port;
  (void)user;

  if (dir->argc != 1) {
    cfg_error("`port` directive accepts only 1 argument");
    cnf_directive_free(dir);
    return NULL;
  }

  if (ports_find(dir->argv[0])) {
    cfg_error("duplicate `port` definition: %s", dir->argv[0]);
    cnf_directive_free(dir);
    return NULL;
  }

  port = calloc(1, sizeof(struct linkd_port));
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
        cfg_error("`speed` directive accepts only 1 argument");
        goto fail;
      }
      port->speed = (uint32_t)strtoul(dir->argv[0], NULL, 10);
    }

    else if (!strcasecmp("fec", dir->name)) {
      if (dir->argc != 1) {
        cfg_error("`fec` directive accepts only 1 argument");
        goto fail;
      }
      port->fec = ports_parse_fec(dir->argv[0]);
      if (port->fec == DP_FEC_UNSET) {
        cfg_error("unknown `fec` value: %s", dir->argv[0]);
        goto fail;
      }
    }

    else if (!strcasecmp("autoneg", dir->name)) {
      if (dir->argc != 1) {
        cfg_error("`autoneg` directive accepts only 1 argument");
        goto fail;
      }
      port->autoneg = ports_parse_bool(dir->argv[0]);
    }

    else if (!strcasecmp("breakout", dir->name)) {
      if (dir->argc != 1) {
        cfg_error("`breakout` directive accepts only 1 argument");
        goto fail;
      }
      if (port->breakout) free(port->breakout);
      port->breakout = strdup(dir->argv[0]);
    }

    else if (ports_is_reserved(dir->name)) {
      cfg_error("`%s` is handled by the interfaces config, not ports", dir->name);
      goto fail;
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

fail:
  port_free(port);
  cnf_directive_free(dir);
  return NULL;
}

void ports_register(void) {
  struct cfg_ns *ns = cfg_ns_get("ports");
  cfg_register_directive(ns, "port", cfg_parse_port);
}
