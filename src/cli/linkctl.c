#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

#include "cli/client.h"
#include "registry.h"
#include "util/auth.h"

static void usage(const char *prog) {
  fprintf(stderr, "usage: %s <command> [args...]\n", prog);
  fprintf(stderr, "       %s COMMAND          list what the daemon accepts\n", prog);
  fprintf(stderr, "       %s hash [user]      generate a password-file entry\n", prog);
}

static char *read_password(const char *prompt) {
  struct termios old, quiet;
  int   tty = isatty(STDIN_FILENO);
  char *line = NULL;
  size_t cap = 0;

  if (tty) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    if (tcgetattr(STDIN_FILENO, &old) == 0) {
      quiet = old;
      quiet.c_lflag &= (tcflag_t)~ECHO;
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    } else {
      tty = 0;
    }
  }

  ssize_t n = getline(&line, &cap, stdin);

  if (tty) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    fprintf(stderr, "\n");
  }

  if (n <= 0) { free(line); return NULL; }
  char *nl = strpbrk(line, "\r\n");
  if (nl) *nl = '\0';
  return line;
}

static int cmd_hash(int argc, char *argv[]) {
  const char *user = argc > 1 ? argv[1] : NULL;

  char *pass = read_password("Password: ");
  if (!pass || !*pass) {
    fprintf(stderr, "linkctl: empty password\n");
    free(pass);
    return 1;
  }
  if (isatty(STDIN_FILENO)) {
    char *again = read_password("Again: ");
    if (!again || strcmp(pass, again) != 0) {
      fprintf(stderr, "linkctl: passwords do not match\n");
      free(pass);
      free(again);
      return 1;
    }
    free(again);
  }

  char *hash = auth_make_hash(pass, AUTH_DEFAULT_ITERATIONS);
  memset(pass, 0, strlen(pass));
  free(pass);

  if (!hash) {
    fprintf(stderr, "linkctl: could not generate hash\n");
    return 1;
  }

  if (user) printf("%s:%s:readonly\n", user, hash);
  else      printf("%s\n", hash);
  free(hash);
  return 0;
}

int main_linkctl(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }
  if (!strcasecmp(argv[1], "hash")) return cmd_hash(argc - 1, argv + 1);

  return linkd_call(argc - 1, (const char **)argv + 1);
}

__attribute__((constructor))
static void register_linkctl(void) {
  cli_register("linkctl", main_linkctl);
}
