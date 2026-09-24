#define _GNU_SOURCE
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "finwo/cnfparse.h"

#include "config/daemon.h"
#include "dataplane/plugin.h"
#include "util/config.h"

static struct linkd_daemon_cfg cfg;
static int cfg_defaults_applied = 0;

// ---------------------------------------------------------------------------
// string-list helpers

static int list_append(char ***list, size_t *n, const char *val) {
  char **grown = realloc(*list, sizeof(char *) * (*n + 1));
  if (!grown) return -1;
  *list = grown;
  (*list)[*n] = strdup(val);
  if (!(*list)[*n]) return -1;
  (*n)++;
  return 0;
}

static void list_free(char ***list, size_t *n) {
  for (size_t i = 0; i < *n; i++) free((*list)[i]);
  free(*list);
  *list = NULL;
  *n = 0;
}

// Replace rather than append: a later config_iface overrides, not merges.
static struct cnf_directive *take_list(struct cnf_directive *dir, char ***list, size_t *n) {
  if (dir->argc < 1) {
    cfg_error("%s needs at least one path or glob", dir->name);
    cnf_directive_free(dir);
    return NULL;
  }
  list_free(list, n);
  for (size_t i = 0; i < dir->argc; i++) {
    if (list_append(list, n, dir->argv[i]) != 0) {
      cfg_error("out of memory reading %s", dir->name);
      cnf_directive_free(dir);
      return NULL;
    }
  }
  cnf_directive_free(dir);
  return NULL;
}

// ---------------------------------------------------------------------------
// directives

static struct cnf_directive *h_config_ports(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)fd; (void)user;
  return take_list(dir, &cfg.ports, &cfg.ports_n);
}

static struct cnf_directive *h_config_iface(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)fd; (void)user;
  return take_list(dir, &cfg.iface, &cfg.iface_n);
}

// Additive: listening on several addresses at once is the point.
static struct cnf_directive *h_listen(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)fd; (void)user;
  if (dir->argc != 1) {
    cfg_error("listen needs exactly one address, e.g. unix:///var/run/linkd.sock");
    cnf_directive_free(dir);
    return NULL;
  }
  const char *a = dir->argv[0];
  if (strncmp(a, "unix://", 7) && strncmp(a, "tcp://", 6)) {
    cfg_error("listen: unsupported address `%s' (expected unix:// or tcp://)", a);
    cnf_directive_free(dir);
    return NULL;
  }
  if (list_append(&cfg.listen, &cfg.listen_n, a) != 0) {
    cfg_error("out of memory reading listen");
  }
  cnf_directive_free(dir);
  return NULL;
}

static struct cnf_directive *h_authfile(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)fd; (void)user;
  if (dir->argc != 1) {
    cfg_error("authfile needs exactly one path");
    cnf_directive_free(dir);
    return NULL;
  }
  free(cfg.authfile);
  cfg.authfile = strdup(dir->argv[0]);
  if (!cfg.authfile) cfg_error("out of memory reading authfile");
  cnf_directive_free(dir);
  return NULL;
}

// Opens a stanza. Known sub-directives handled here; the first unknown one
// ends the stanza and is handed back for re-dispatch.
static struct cnf_directive *h_plugin(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)user;

  if (dir->argc != 1) {
    cfg_error("plugin needs exactly one executable path or tcp:// address");
    cnf_directive_free(dir);
    return NULL;
  }

  struct linkd_plugin_cfg *p = calloc(1, sizeof(*p));
  if (!p) {
    cfg_error("out of memory reading plugin");
    cnf_directive_free(dir);
    return NULL;
  }
  p->target = strdup(dir->argv[0]);
  if (!p->target) {
    free(p);
    cfg_error("out of memory reading plugin");
    cnf_directive_free(dir);
    return NULL;
  }
  cnf_directive_free(dir);

  // Append: plugins are broadcast to in declaration order.
  struct linkd_plugin_cfg **tail = &cfg.plugins;
  while (*tail) tail = &(*tail)->next;
  *tail = p;

  for (;;) {
    struct cnf_directive *sub = cnf_directive_read(fd);
    if (!sub) return NULL; // EOF ends the stanza

    if (!strcasecmp(sub->name, "optional")) {
      if (sub->argc != 0) {
        cfg_error("plugin %s: optional takes no arguments", p->target);
        cnf_directive_free(sub);
        return NULL;
      }
      p->optional = 1;
      cnf_directive_free(sub);
      continue;
    }

    // Unknown to us: the plugin gets first refusal. Starting it here is what
    // makes its config vocabulary discoverable at all -- CONFIG LIST cannot
    // be asked of a process that is not running.
    struct dp_plugin *dp = dp_plugin_for(p->target);
    if (dp && dp_plugin_config_supports(dp, sub->name)) {
      if (dp_plugin_config_set(dp, sub->name, sub->argv, sub->argc) != 0) {
        cfg_error("plugin %s rejected `%s'", p->target, sub->name);
        cnf_directive_free(sub);
        return NULL;
      }
      cnf_directive_free(sub);
      continue;
    }

    // Neither side claims it: ends the stanza, hand it back for re-dispatch.
    return sub;
  }
}

// ---------------------------------------------------------------------------

void daemon_cfg_register(void) {
  struct cfg_ns *ns = cfg_ns_get("linkd");
  cfg_register_directive(ns, "config_ports", h_config_ports);
  cfg_register_directive(ns, "config_iface", h_config_iface);
  cfg_register_directive(ns, "listen",       h_listen);
  cfg_register_directive(ns, "authfile",     h_authfile);
  cfg_register_directive(ns, "plugin",       h_plugin);
}

// Per-field, so a config setting only `listen` still gets working paths.
static void apply_defaults(void) {
  if (cfg_defaults_applied) return;
  cfg_defaults_applied = 1;

  if (cfg.ports_n == 0) {
    list_append(&cfg.ports, &cfg.ports_n, "/etc/network/ports");
    list_append(&cfg.ports, &cfg.ports_n, "/etc/network/ports.d/*.cnf");
  }
  if (cfg.iface_n == 0) {
    list_append(&cfg.iface, &cfg.iface_n, "/etc/network/interfaces");
    list_append(&cfg.iface, &cfg.iface_n, "/etc/network/interfaces.d/*.cnf");
  }
  if (cfg.listen_n == 0) {
    list_append(&cfg.listen, &cfg.listen_n, "unix:///var/run/linkd.sock");
  }
}

int daemon_cfg_load(const char *path) {
  if (!path) path = LINKD_CONFIG_PATH;

  // Missing file is fine: defaults are a working configuration.
  if (access(path, R_OK) != 0) {
    apply_defaults();
    return CFG_RET_OK;
  }

  FILE *fd = fopen(path, "r");
  if (!fd) {
    fprintf(stderr, "config: cannot open %s\n", path);
    return CFG_RET_ERROR;
  }

  // Relative `source` resolves against the config file's own directory.
  char *dup = strdup(path);
  char *wd  = dup ? dirname(dup) : NULL;

  cfg_error_reset();
  int rc = cfg_parse(cfg_ns_get("linkd"), wd, fd, NULL);
  fclose(fd);
  free(dup);

  if (rc != CFG_RET_OK) return rc;

  apply_defaults();
  return CFG_RET_OK;
}

int load_namespace(const char *ns_name, char **patterns, size_t n) {
  if (n == 0) return CFG_RET_OK;

  size_t len = strlen("source");
  for (size_t i = 0; i < n; i++) len += 1 + strlen(patterns[i]);
  len += 2; // newline + NUL

  char *stub = malloc(len);
  if (!stub) {
    fprintf(stderr, "config: out of memory building %s source list\n", ns_name);
    return CFG_RET_ERROR;
  }

  char *p = stub + sprintf(stub, "source");
  for (size_t i = 0; i < n; i++) p += sprintf(p, " %s", patterns[i]);
  *p++ = '\n';
  *p = '\0';

  FILE *fd = fmemopen(stub, strlen(stub), "r");
  if (!fd) {
    perror("Could not open in-memory config");
    free(stub);
    return CFG_RET_ERROR;
  }

  // wd NULL: patterns come from linkd.cnf and are expected to be absolute.
  cfg_error_reset();
  int rc = cfg_parse(cfg_ns_get(ns_name), NULL, fd, NULL);
  if (rc < 0) {
    fprintf(stderr, "config: error reading %s configuration\n", ns_name);
  }

  fclose(fd);
  free(stub);
  return rc;
}

const char *daemon_cfg_unix_socket(void) {
  apply_defaults();
  for (size_t i = 0; i < cfg.listen_n; i++) {
    if (!strncmp(cfg.listen[i], "unix://", 7)) return cfg.listen[i] + 7;
  }
  return NULL;
}

const char *linkd_client_socket(void) {
  static int loaded = 0;
  if (!loaded) {
    loaded = 1;
    daemon_cfg_register();
    // Failure is not fatal here: fall through to the default rather than
    // leaving the operator with no way to reach a running daemon.
    daemon_cfg_load(getenv("LINKD_CONFIG"));
  }
  const char *p = daemon_cfg_unix_socket();
  return p ? p : "/var/run/linkd.sock";
}

const struct linkd_daemon_cfg *daemon_cfg(void) {
  apply_defaults();
  return &cfg;
}

void daemon_cfg_free(void) {
  list_free(&cfg.ports,  &cfg.ports_n);
  list_free(&cfg.iface,  &cfg.iface_n);
  list_free(&cfg.listen, &cfg.listen_n);
  free(cfg.authfile);
  cfg.authfile = NULL;

  struct linkd_plugin_cfg *p = cfg.plugins;
  while (p) {
    struct linkd_plugin_cfg *next = p->next;
    free(p->target);
    free(p);
    p = next;
  }
  cfg.plugins = NULL;
  cfg_defaults_applied = 0;
}
