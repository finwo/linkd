#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define UNOS_IPC_PATH "/run/unosd.sock"

int main(int argc, char *argv[]) {
  const char *prog = basename(argv[0]);
  const char *cmd = NULL;
  int cmd_argc = 0;
  char **cmd_argv = NULL;
  int sock;
  struct sockaddr_un addr;
  char buf[4096];

  // Determine command from argv[0] or argv[1]
  if (!strcmp(prog, "unosc")) {
    if (argc < 2) {
      fprintf(stderr, "usage: %s <ifup|ifdown|ifquery|ifreload> [args...]\n", prog);
      fprintf(stderr, "   or: ifup/ifdown/ifquery/ifreload as symlinks to unosc\n");
      return 1;
    }
    cmd = argv[1];
    cmd_argc = argc - 2;
    cmd_argv = argv + 2;
  } else {
    // Called as ifup, ifdown, etc. via symlink
    cmd = prog;
    cmd_argc = argc - 1;
    cmd_argv = argv + 1;
  }

  sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, UNOS_IPC_PATH, sizeof(addr.sun_path)-1);

  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "unosc: connect %s failed: %s\n", UNOS_IPC_PATH, strerror(errno));
    fprintf(stderr, "Is unosd running?\n");
    close(sock);
    return 1;
  }

  // Send command: "cmd [args...]\n"
  {
    char out[4096];
    int len = 0;
    len += snprintf(out+len, sizeof(out)-len, "%s", cmd);
    for (int i = 0; i < cmd_argc; i++) {
      len += snprintf(out+len, sizeof(out)-len, " %s", cmd_argv[i]);
    }
    len += snprintf(out+len, sizeof(out)-len, "\n");
    if (write(sock, out, len) != len) {
      perror("write");
      close(sock);
      return 1;
    }
  }

  // Read response and pipe to stdout
  // Protocol: server sends lines, first line is OK/ERR, but for ifquery the dump is the response
  // We just pipe everything to stdout, and exit 0 on OK, 1 on ERR
  int saw_ok = 0;
  int saw_err = 0;
  FILE *f = fdopen(sock, "r");
  if (!f) {
    perror("fdopen");
    close(sock);
    return 1;
  }
  while (fgets(buf, sizeof(buf), f)) {
    // Check for OK/ERR prefix on first line? Just print and detect
    if (!saw_ok && !saw_err) {
      if (!strncmp(buf, "OK", 2)) saw_ok = 1;
      else if (!strncmp(buf, "ERR", 3)) saw_err = 1;
    }
    fputs(buf, stdout);
    // For non-ifquery, the response is just OK/ERR line
    // For ifquery, there may be multiple lines plus OK
  }
  fclose(f);
  // If we saw ERR, exit 1
  if (saw_err) return 1;
  return 0;
}
