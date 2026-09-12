#ifndef __UNOS_UTIL_CONFIG_H__
#define __UNOS_UTIL_CONFIG_H__

#include <stdio.h>

#include "finwo/cnfparse.h"

#define CFG_RET_ERROR -1
#define CFG_RET_OK     0

// A directive handler consumes its own sub-directives straight off `fd`, then
// returns the first directive it did not recognise so the caller can
// re-dispatch it. Returning NULL means end-of-input was reached.
typedef struct cnf_directive * (*cfg_directive_fn)(FILE *fd, struct cnf_directive *dir, void *user);

void cfg_register_directive(const char *name, cfg_directive_fn fn);

// `wd` is the directory relative `include` patterns resolve against.
int cfg_parse(const char *wd, FILE *fd, void *user);

#endif // __UNOS_UTIL_CONFIG_H__
