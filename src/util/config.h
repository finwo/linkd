#ifndef __LINKD_UTIL_CONFIG_H__
#define __LINKD_UTIL_CONFIG_H__

#include <stdio.h>

#include "finwo/cnfparse.h"

#define CFG_RET_ERROR -1
#define CFG_RET_OK     0

// A directive handler consumes its own sub-directives straight off `fd`, then
// returns the first directive it did not recognise so the caller can
// re-dispatch it. Returning NULL means end-of-input was reached.
typedef struct cnf_directive * (*cfg_directive_fn)(FILE *fd, struct cnf_directive *dir, void *user);

// Report a malformed directive. The handler return type has no error channel
// (NULL means "consumed"), so this flag is how a handler rejects input without
// exit(1) -- which would kill the daemon on an ifreload.
void cfg_error(const char *fmt, ...);
int  cfg_error_occurred(void);
void cfg_error_reset(void);

struct cfg_ns;

// Create+get interned namespace (idempotent)
struct cfg_ns *cfg_ns_get(const char *name);

void cfg_register_directive(struct cfg_ns *ns, const char *name, cfg_directive_fn fn);

// `wd` is the directory relative `source` patterns resolve against.
int cfg_parse(struct cfg_ns *ns, const char *wd, FILE *fd, void *user);

#endif // __LINKD_UTIL_CONFIG_H__
