#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "config/daemon.h"
#include "registry.h"


int main_ifreload(int argc, char *argv[]) {
  if (argc != 1) {
    fprintf(stderr, "usage: ifreload\n");
    return 1;
  }
  (void)argv;
  char out[4096];
  int len = snprintf(out, sizeof(out), "ifreload\n");
  const char *sockpath = linkd_client_socket();
  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0) { perror("socket"); return 1; }
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path)-1);
  if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "ifreload: connect %s failed: %s\nIs linkd running?\n", sockpath, strerror(errno));
    close(sock);
    return 1;
  }
  if (write(sock, out, len) != len) { perror("write"); close(sock); return 1; }
  // Half-close: signals EOF to linkd so it stops reading and replies. Without this
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
static void register_ifreload(void) {
  cli_register("ifreload", main_ifreload);
}
