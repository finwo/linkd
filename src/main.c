#define _GNU_SOURCE
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/registry.h"

// Multicall: basename(argv[0]) selects which main runs. Commands self-register
// from constructors in src/cli/.

static const char *AVAILABLE = "linkd, linkctl, ifup, ifdown, ifquery, ifreload";

int main(int argc, char *argv[]) {
  const char *prog = basename(argv[0]);

  cli_main_fn fn = cli_find(prog);
  if (fn) return fn(argc, argv);

  // Unrecognised name (build tree, wrapper script): try argv[1], shifting so
  // the callee sees its own name in argv[0].
  if (argc > 1 && (fn = cli_find(argv[1]))) {
    char **shifted = malloc(sizeof(char *) * (size_t)argc);
    if (!shifted) {
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    shifted[0] = argv[1];
    for (int i = 2; i < argc; i++) shifted[i - 1] = argv[i];
    int rc = fn(argc - 1, shifted);
    free(shifted);
    return rc;
  }

  fprintf(stderr, "%s: unknown command: %s\n", argv[0], prog);
  fprintf(stderr, "available commands: %s\n", AVAILABLE);
  return 1;
}
