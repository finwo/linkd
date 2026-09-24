#ifndef __LINKD_DATAPLANE_PLUGIN_H__
#define __LINKD_DATAPLANE_PLUGIN_H__

#include <sys/types.h>
#include <time.h>

#include "finwo/resp.h"

struct dp_plugin {
  char  *target;
  int    optional;

  int    fd;            // socketpair to the child, or the tcp socket
  pid_t  pid;           // 0 when connected over tcp
  char  *user;          // tcp:// credentials, if any
  char  *pass;

  int    up;
  int    hardware;      // hardware_detected from INFO

  char **commands;      // verbs from COMMAND, upper-cased
  size_t commands_n;
  char **config_keys;   // directives from CONFIG LIST
  size_t config_keys_n;

  int    backoff_ms;    // 0 until the first failure
  struct timespec retry_at;

  struct dp_plugin *next;
};

// Send argv as a RESP array and read one reply. NULL on transport failure,
// which also marks the plugin down. Caller owns the result.
resp_object *dp_plugin_call(struct dp_plugin *p, const char **argv, int argc);

// True if COMMAND advertised this verb.
int dp_plugin_supports(const struct dp_plugin *p, const char *verb);

// True if CONFIG LIST advertised this directive.
int dp_plugin_config_supports(const struct dp_plugin *p, const char *key);

// Start, or reconnect after backoff. Returns 0 when usable.
int  dp_plugin_up(struct dp_plugin *p);
void dp_plugin_down(struct dp_plugin *p, const char *why);

// Plugin list, in declaration order.
struct dp_plugin *dp_plugin_list(void);

// Look up or create the entry for a target, starting it if needed. Called
// during config parsing (to ask about directives) and by dp_start().
struct dp_plugin *dp_plugin_for(const char *target);

// CONFIG SET <key> <argv...>. Returns 0 on +OK.
int dp_plugin_config_set(struct dp_plugin *p, const char *key, char **argv, size_t argc);

#endif // __LINKD_DATAPLANE_PLUGIN_H__
