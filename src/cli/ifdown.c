#define _GNU_SOURCE
#include <stdio.h>

#include "cli/client.h"
#include "registry.h"

int main_ifdown(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: ifdown <interface>\n");
    return 1;
  }
  const char *cmd[] = { "IFDOWN", argv[1] };
  return linkd_call(2, cmd);
}

__attribute__((constructor))
static void register_ifdown(void) {
  cli_register("ifdown", main_ifdown);
}
