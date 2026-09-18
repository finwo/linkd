#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "registry.h"

#define UNOS_IPC_PATH "/run/unosd.sock"

int main_ifup(int argc, char *argv[]) {
  // ifup <if> -- thin wrapper, same socket logic as unosc but fixed command
  if (argc < 2) {
    fprintf(stderr, "usage: ifup <interface>\n");
    return 1;
  }
  // Build command: "ifup <if> [extra args]"
  char out[4096];
  int len = snprintf(out, sizeof(out), "ifup");
  for (int i = 1; i < argc; i++) {
    len += snprintf(out+len, sizeof(out)-len, " %s", argv[i]);
  }
  len += snprintf(out+len, sizeof(out)-len, "\n");

  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) { perror("socket"); return 1; }
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, UNOS_IPC_PATH, sizeof(addr.sun_path)-1);
  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "ifup: connect %s failed: %s\nIs unosd running?\n", UNOS_IPC_PATH, strerror(errno));
    close(sock);
    return 1;
  }
  if (write(sock, out, len) != len) { perror("write"); close(sock); return 1; }
  // Half-close: signals EOF to unosd so it stops reading and replies. Without this
  // the server blocks waiting for a second command until its receive timeout.
  shutdown(sock, SHUT_WR);
  char buf[4096];
  int saw_err = 0;
  FILE *f = fdopen(sock, "r");
  if (!f) { perror("fdopen"); close(sock); return 1; }
  while (fgets(buf, sizeof(buf), f)) {
    if (!strncmp(buf, "ERR", 3)) saw_err = 1;
    fputs(buf, stdout);
  }
  fclose(f);
  return saw_err ? 1 : 0;
}

__attribute__((constructor))
static void register_ifup(void) {
  cli_register("ifup", main_ifup);
}
