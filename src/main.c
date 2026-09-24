#define _GNU_SOURCE
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/registry.h"

// Multicall: basename(argv[0]) selects which main runs. Commands self-register
// from constructors in src/cli/. linkctl is the exception -- it takes its
// subcommand from argv[1].

static const char *AVAILABLE = "linkd, linkctl, ifup, ifdown, ifquery, ifreload";

// Shift argv left by one so the callee sees its own name in argv[0].
static int dispatch_shifted(cli_main_fn fn, int argc, char *argv[]) {
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

int main(int argc, char *argv[]) {
  const char *prog = basename(argv[0]);

  // linkctl <command> [args...]
  if (!strcmp(prog, "linkctl") && argc > 1) {
    cli_main_fn fn = cli_find(argv[1]);
    if (!fn) {
      fprintf(stderr, "linkctl: unknown command: %s\n", argv[1]);
      fprintf(stderr, "available: %s\n", AVAILABLE);
      return 1;
    }
    return dispatch_shifted(fn, argc, argv);
  }

  cli_main_fn fn = cli_find(prog);
  if (fn) return fn(argc, argv);

  // Unrecognised name (build tree, wrapper script): try argv[1].
  if (argc > 1 && (fn = cli_find(argv[1]))) {
    return dispatch_shifted(fn, argc, argv);
  }

  fprintf(stderr, "%s: unknown command: %s\n", argv[0], prog);
  fprintf(stderr, "available commands: %s\n", AVAILABLE);
  return 1;
}
