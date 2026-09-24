#define _GNU_SOURCE
#include <stdio.h>

#include "cli/client.h"
#include "registry.h"

int main_ifup(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: ifup <interface>\n");
    return 1;
  }
  const char *cmd[] = { "IFUP", argv[1] };
  return linkd_call(2, cmd);
}

__attribute__((constructor))
static void register_ifup(void) {
  cli_register("ifup", main_ifup);
}
