#define _GNU_SOURCE
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/registry.h"

int main(int argc, char *argv[]) {
  const char *prog = basename(argv[0]);
  // Strip path, handle multicall: argv[0] determines command
  // Also handle "unosc <cmd>" as before: if prog is unosc and argc>1, dispatch to <cmd>
  if (!strcmp(prog, "unosc") && argc > 1) {
    prog = argv[1];
    // Shift argv for the target main: make argv[0] be prog, argc-1
    // We need to adjust argv for the called main: it expects argv[0] to be the command name
    // Create a new argv array with prog as argv[0] and the rest shifted
    char **new_argv = malloc(sizeof(char*) * argc);
    new_argv[0] = (char*)prog;
    for (int i = 2; i < argc; i++) new_argv[i-1] = argv[i];
    int new_argc = argc - 1;
    cli_main_fn fn = cli_find(prog);
    if (!fn) {
      fprintf(stderr, "unosc: unknown command: %s\n", prog);
      fprintf(stderr, "available: ifup, ifdown, ifquery, ifreload, unosd, unosc\n");
      free(new_argv);
      return 1;
    }
    int rc = fn(new_argc, new_argv);
    free(new_argv);
    return rc;
  }

  cli_main_fn fn = cli_find(prog);
  if (fn) {
    return fn(argc, argv);
  }

  // Also handle case where binary is called as "unosd" but we want to allow "unos" as alias,
  // and also handle being called as "unosc" without extra dispatch above already handled.
  // If not found, try to find "unosd" as default for bare invocation with --help etc.?
  // For backwards compat, if prog is not found and argc>1 and first arg is a known command, dispatch to it
  if (argc > 1) {
    fn = cli_find(argv[1]);
    if (fn) {
      // Shift as above
      char **new_argv = malloc(sizeof(char*) * argc);
      new_argv[0] = argv[1];
      for (int i = 2; i < argc; i++) new_argv[i-1] = argv[i];
      int new_argc = argc - 1;
      int rc = fn(new_argc, new_argv);
      free(new_argv);
      return rc;
    }
  }

  fprintf(stderr, "%s: unknown command: %s\n", argv[0], prog);
  fprintf(stderr, "available commands: unosd, unosc, ifup, ifdown, ifquery, ifreload\n");
  return 1;
}
