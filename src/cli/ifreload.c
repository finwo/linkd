#define _GNU_SOURCE
#include "cli/client.h"
#include "registry.h"

int main_ifreload(int argc, char *argv[]) {
  (void)argc; (void)argv;
  const char *cmd[] = { "IFRELOAD" };
  return linkd_call(1, cmd);
}

__attribute__((constructor))
static void register_ifreload(void) {
  cli_register("ifreload", main_ifreload);
}
