#define _GNU_SOURCE
#include <stddef.h>

#include "cli/client.h"
#include "registry.h"

int main_ifquery(int argc, char *argv[]) {
  const char *cmd[2] = { "IFQUERY", NULL };
  if (argc > 1) {
    cmd[1] = argv[1];
    return linkd_call(2, cmd);
  }
  return linkd_call(1, cmd);
}

__attribute__((constructor))
static void register_ifquery(void) {
  cli_register("ifquery", main_ifquery);
}
