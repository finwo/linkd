#ifndef __LINKD_CONFIG_DAEMON_H__
#define __LINKD_CONFIG_DAEMON_H__

#include <stddef.h>

// linkd's own configuration: /etc/linkd.cnf. Same grammar as the rest, so
// `source` works here too.

struct linkd_plugin_cfg {
  char                    *target;    // executable path, or tcp://[user:pass@]host[:port]
  int                      optional;  // failures do not fail the operation
  struct linkd_plugin_cfg *next;
};

struct linkd_daemon_cfg {
  char  **ports;    size_t ports_n;   // path/glob patterns
  char  **iface;    size_t iface_n;
  char  **listen;   size_t listen_n;
  char   *authfile;

  // argv template for `method dhcp`, each element rendered per interface.
  // Empty means autodetect.
  char  **dhcp_client; size_t dhcp_client_n;

  struct linkd_plugin_cfg *plugins;
};

void daemon_cfg_register(void);

// A missing config file is not an error; the defaults are usable.
int daemon_cfg_load(const char *path);

// Never NULL; returns defaults before daemon_cfg_load() runs.
const struct linkd_daemon_cfg *daemon_cfg(void);

void daemon_cfg_free(void);

// First unix:// listen address, minus the scheme. NULL if only tcp:// is
// configured. Used by the daemon to bind and by the clients to connect.
const char *daemon_cfg_unix_socket(void);

// Same, for the CLI tools: loads the config first, honouring $LINKD_CONFIG.
// Never NULL -- falls back to the built-in default.
const char *linkd_client_socket(void);

// Load "ports" / "interfaces" from config_ports / config_iface patterns.
// Expanded via `source`, so a pattern matching nothing is not an error.
// Shared by startup and ifreload so the two cannot drift apart.
int load_namespace(const char *ns_name, char **patterns, size_t n);

// Drop-in dir, used only when LINKD_CONFIG_PATH is absent.
#define LINKD_CONFIG_PATH "/etc/linkd.cnf"
#define LINKD_CONFIG_DIR  "/etc/linkd"

#endif // __LINKD_CONFIG_DAEMON_H__
