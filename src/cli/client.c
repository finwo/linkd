#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "finwo/resp.h"

#include "cli/client.h"
#include "config/daemon.h"

#define CLIENT_TIMEOUT_S 10

static int connect_daemon(const char **path_out) {
  const char *sockpath = linkd_client_socket();
  *path_out = sockpath;

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "connect %s failed: %s\nIs linkd running?\n",
            sockpath, strerror(errno));
    close(fd);
    return -1;
  }

  struct timeval tv = { .tv_sec = CLIENT_TIMEOUT_S, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

static const char *map_get(const resp_object *m, const char *key) {
  if (!m || m->type != RESPT_ARRAY) return NULL;
  for (size_t i = 0; i + 1 < m->u.arr.n; i += 2) {
    const resp_object *k = &m->u.arr.elem[i];
    if ((k->type == RESPT_BULK || k->type == RESPT_SIMPLE) && k->u.s &&
        !strcmp(k->u.s, key)) {
      const resp_object *v = &m->u.arr.elem[i + 1];
      if (v->type == RESPT_BULK || v->type == RESPT_SIMPLE) return v->u.s;
    }
  }
  return NULL;
}

// An interface map renders as the config syntax it came from, so ifquery
// output can be pasted back into /etc/network/interfaces.
static int render_iface_map(const resp_object *m) {
  const char *name = map_get(m, "name");
  if (!name) return 0;

  printf("iface %s\n", name);
  for (size_t i = 0; i + 1 < m->u.arr.n; i += 2) {
    const resp_object *k = &m->u.arr.elem[i];
    const resp_object *v = &m->u.arr.elem[i + 1];
    if (k->type != RESPT_BULK && k->type != RESPT_SIMPLE) continue;
    if (!strcmp(k->u.s, "name")) continue;
    if (v->type == RESPT_BULK || v->type == RESPT_SIMPLE) {
      printf("  %s %s\n", k->u.s, v->u.s);
    } else if (v->type == RESPT_INT) {
      printf("  %s %lld\n", k->u.s, v->u.i);
    }
  }
  return 1;
}

static void render(const resp_object *o, int depth) {
  if (!o) return;
  switch (o->type) {
    case RESPT_SIMPLE:
      printf("%s\n", o->u.s ? o->u.s : "");
      break;
    case RESPT_BULK:
      if (o->u.s) fputs(o->u.s, stdout);
      if (o->u.s && o->u.s[strlen(o->u.s) - 1] != '\n') fputc('\n', stdout);
      break;
    case RESPT_INT:
      printf("%lld\n", o->u.i);
      break;
    case RESPT_ARRAY:
      if (depth == 0 && render_iface_map(o)) break;
      for (size_t i = 0; i < o->u.arr.n; i++) {
        const resp_object *e = &o->u.arr.elem[i];
        if (e->type == RESPT_ARRAY && render_iface_map(e)) continue;
        render(e, depth + 1);
      }
      break;
    default:
      break;
  }
}

int linkd_call(int argc, const char **argv) {
  const char *sockpath = NULL;
  int fd = connect_daemon(&sockpath);
  if (fd < 0) return 1;

  resp_object *req = resp_array_init();
  if (!req) { close(fd); return 1; }
  for (int i = 0; i < argc; i++) resp_array_append_bulk(req, argv[i]);

  char  *buf = NULL;
  size_t len = 0;
  int    rc  = resp_serialize(req, &buf, &len);
  resp_free(req);
  if (rc != 0) { close(fd); return 1; }

  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, buf + sent, len - sent);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      fprintf(stderr, "write failed: %s\n", strerror(errno));
      free(buf);
      close(fd);
      return 1;
    }
    sent += (size_t)n;
  }
  free(buf);

  resp_object *reply = resp_read(fd);
  close(fd);

  if (!reply) {
    fprintf(stderr, "no reply from linkd\n");
    return 1;
  }

  int status = 0;
  if (reply->type == RESPT_ERROR) {
    fprintf(stderr, "%s\n", reply->u.s ? reply->u.s : "error");
    status = 1;
  } else {
    render(reply, 0);
  }
  resp_free(reply);
  return status;
}
