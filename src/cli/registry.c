#include <string.h>
#include "registry.h"

#define CLI_MAX 32

static const char *cli_names[CLI_MAX];
static cli_main_fn cli_fns[CLI_MAX];
static int cli_count = 0;

void cli_register(const char *name, cli_main_fn fn) {
  if (cli_count >= CLI_MAX) return;
  cli_names[cli_count] = name;
  cli_fns[cli_count] = fn;
  cli_count++;
}

cli_main_fn cli_find(const char *name) {
  for (int i = 0; i < cli_count; i++) {
    if (!strcmp(cli_names[i], name)) return cli_fns[i];
  }
  return NULL;
}
