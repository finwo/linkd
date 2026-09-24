#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "finwo/resp.h"
#include "rxi/log.h"

#include "config/daemon.h"
#include "dataplane.h"
#include "dataplane/plugin.h"

#define PLUGIN_TIMEOUT_S    5
#define BACKOFF_MIN_MS    500
#define BACKOFF_MAX_MS  30000
#define MAX_CONFIG_ARGV    32

static struct dp_plugin *plugins = NULL;

static void strlist_free(char ***list, size_t *n) {
  for (size_t i = 0; i < *n; i++) free((*list)[i]);
  free(*list);
  *list = NULL;
  *n = 0;
}

static int strlist_add(char ***list, size_t *n, const char *s) {
  char **g = realloc(*list, sizeof(char *) * (*n + 1));
  if (!g) return -1;
  *list = g;
  (*list)[*n] = strdup(s);
  if (!(*list)[*n]) return -1;
  (*n)++;
  return 0;
}

static int strlist_has(char **list, size_t n, const char *s) {
  for (size_t i = 0; i < n; i++) {
    if (!strcasecmp(list[i], s)) return 1;
  }
  return 0;
}

static void now_plus_ms(struct timespec *out, int ms) {
  clock_gettime(CLOCK_MONOTONIC, out);
  out->tv_sec  += ms / 1000;
  out->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (out->tv_nsec >= 1000000000L) { out->tv_sec++; out->tv_nsec -= 1000000000L; }
}

static int deadline_passed(const struct timespec *t) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (now.tv_sec != t->tv_sec) return now.tv_sec > t->tv_sec;
  return now.tv_nsec >= t->tv_nsec;
}

static int set_timeouts(int fd) {
  struct timeval tv = { .tv_sec = PLUGIN_TIMEOUT_S, .tv_usec = 0 };
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return -1;
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return -1;
  return 0;
}

// socketpair, not pipes: SO_RCVTIMEO works on it, so a wedged plugin cannot
// block the daemon forever.
static int spawn_child(struct dp_plugin *p) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
    log_error("plugin %s: socketpair: %s", p->target, strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    log_error("plugin %s: fork: %s", p->target, strerror(errno));
    close(sv[0]);
    close(sv[1]);
    return -1;
  }

  if (pid == 0) {
    // stderr is inherited, so plugin diagnostics reach the daemon log.
    dup2(sv[1], STDIN_FILENO);
    dup2(sv[1], STDOUT_FILENO);
    if (sv[1] > STDERR_FILENO) close(sv[1]);
    close(sv[0]);
    signal(SIGPIPE, SIG_DFL);
    execl(p->target, p->target, (char *)NULL);
    fprintf(stderr, "plugin %s: exec failed: %s\n", p->target, strerror(errno));
    _exit(127);
  }

  close(sv[1]);
  if (set_timeouts(sv[0]) != 0) {
    log_error("plugin %s: setsockopt: %s", p->target, strerror(errno));
    close(sv[0]);
    return -1;
  }
  p->fd  = sv[0];
  p->pid = pid;
  return 0;
}

// tcp://[user:pass@]host[:port]
static int parse_tcp(const char *url, char **host, char **port, char **user, char **pass) {
  const char *rest = url + 6; // skip tcp://
  const char *at   = strrchr(rest, '@');
  *user = *pass = NULL;

  if (at) {
    size_t clen = (size_t)(at - rest);
    char  *cred = strndup(rest, clen);
    if (!cred) return -1;
    char *colon = strchr(cred, ':');
    if (colon) {
      *colon = '\0';
      *user = strdup(cred);
      *pass = strdup(colon + 1);
    } else {
      *user = strdup(cred);
    }
    free(cred);
    rest = at + 1;
  }

  const char *colon = strrchr(rest, ':');
  if (colon) {
    *host = strndup(rest, (size_t)(colon - rest));
    *port = strdup(colon + 1);
  } else {
    *host = strdup(rest);
    *port = strdup("6789");
  }
  return (*host && *port) ? 0 : -1;
}

static int connect_tcp(struct dp_plugin *p) {
  char *host = NULL, *port = NULL;
  free(p->user); free(p->pass);
  p->user = p->pass = NULL;

  if (parse_tcp(p->target, &host, &port, &p->user, &p->pass) != 0) {
    log_error("plugin %s: malformed address", p->target);
    free(host); free(port);
    return -1;
  }

  struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
  struct addrinfo *res = NULL;
  int gai = getaddrinfo(host, port, &hints, &res);
  free(host); free(port);
  if (gai != 0) {
    log_error("plugin %s: resolve: %s", p->target, gai_strerror(gai));
    return -1;
  }

  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0) {
    log_error("plugin %s: connect failed: %s", p->target, strerror(errno));
    return -1;
  }
  if (set_timeouts(fd) != 0) {
    close(fd);
    return -1;
  }
  p->fd  = fd;
  p->pid = 0;
  return 0;
}

static int write_all(int fd, const char *buf, size_t len) {
  while (len) {
    ssize_t n = write(fd, buf, len);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return -1;
    }
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

resp_object *dp_plugin_call(struct dp_plugin *p, const char **argv, int argc) {
  if (!p->up || p->fd < 0) return NULL;

  resp_object *req = resp_array_init();
  if (!req) return NULL;
  for (int i = 0; i < argc; i++) resp_array_append_bulk(req, argv[i]);

  char  *buf = NULL;
  size_t len = 0;
  if (resp_serialize(req, &buf, &len) != 0) {
    resp_free(req);
    return NULL;
  }
  resp_free(req);

  int rc = write_all(p->fd, buf, len);
  free(buf);
  if (rc != 0) {
    dp_plugin_down(p, "write failed");
    return NULL;
  }

  // resp_read returns NULL for timeout, EOF and parse errors alike; all three
  // mean the plugin is unusable.
  resp_object *reply = resp_read(p->fd);
  if (!reply) {
    dp_plugin_down(p, "no reply (timeout, EOF or malformed)");
    return NULL;
  }
  return reply;
}

static int reply_is_error(const resp_object *o) {
  return o && o->type == RESPT_ERROR;
}

static void load_commands(struct dp_plugin *p) {
  strlist_free(&p->commands, &p->commands_n);
  const char  *argv[] = { "COMMAND" };
  resp_object *r = dp_plugin_call(p, argv, 1);
  if (!r) return;

  if (r->type == RESPT_ARRAY) {
    for (size_t i = 0; i < r->u.arr.n; i++) {
      resp_object *e = &r->u.arr.elem[i];
      // Accept a flat list of names or redis' array-per-command form.
      const char *name = NULL;
      if (e->type == RESPT_BULK || e->type == RESPT_SIMPLE) {
        name = e->u.s;
      } else if (e->type == RESPT_ARRAY && e->u.arr.n > 0) {
        resp_object *f = &e->u.arr.elem[0];
        if (f->type == RESPT_BULK || f->type == RESPT_SIMPLE) name = f->u.s;
      }
      if (name) strlist_add(&p->commands, &p->commands_n, name);
    }
  }
  resp_free(r);
}

static void load_config_keys(struct dp_plugin *p) {
  strlist_free(&p->config_keys, &p->config_keys_n);
  if (!dp_plugin_supports(p, "CONFIG")) return;

  const char  *argv[] = { "CONFIG", "LIST" };
  resp_object *r = dp_plugin_call(p, argv, 2);
  if (!r) return;

  if (r->type == RESPT_ARRAY) {
    for (size_t i = 0; i < r->u.arr.n; i++) {
      resp_object *e = &r->u.arr.elem[i];
      if (e->type == RESPT_BULK || e->type == RESPT_SIMPLE) {
        strlist_add(&p->config_keys, &p->config_keys_n, e->u.s);
      }
    }
  }
  resp_free(r);
}

// redis INFO format; hardware_detected is the only field we interpret.
static void load_info(struct dp_plugin *p) {
  p->hardware = -1;
  const char  *argv[] = { "INFO" };
  resp_object *r = dp_plugin_call(p, argv, 1);
  if (!r) return;

  if ((r->type == RESPT_BULK || r->type == RESPT_SIMPLE) && r->u.s) {
    const char *k = strstr(r->u.s, "hardware_detected:");
    if (k) p->hardware = atoi(k + strlen("hardware_detected:")) ? 1 : 0;
  }
  resp_free(r);
}

static int authenticate(struct dp_plugin *p) {
  if (!p->pass && !p->user) return 0;
  const char *argv[3];
  int argc = 0;
  argv[argc++] = "AUTH";
  if (p->user && p->pass) {
    argv[argc++] = p->user;
    argv[argc++] = p->pass;
  } else {
    argv[argc++] = p->user ? p->user : p->pass;
  }
  resp_object *r = dp_plugin_call(p, argv, argc);
  if (!r) return -1;
  int bad = reply_is_error(r);
  if (bad) log_error("plugin %s: AUTH rejected: %s", p->target, r->u.s ? r->u.s : "");
  resp_free(r);
  return bad ? -1 : 0;
}

void dp_plugin_down(struct dp_plugin *p, const char *why) {
  if (p->fd >= 0) { close(p->fd); p->fd = -1; }
  if (p->pid > 0) {
    kill(p->pid, SIGTERM);
    waitpid(p->pid, NULL, 0);
    p->pid = 0;
  }
  if (p->up) log_error("plugin %s: down (%s)", p->target, why ? why : "unknown");
  p->up = 0;

  p->backoff_ms = p->backoff_ms ? p->backoff_ms * 2 : BACKOFF_MIN_MS;
  if (p->backoff_ms > BACKOFF_MAX_MS) p->backoff_ms = BACKOFF_MAX_MS;
  now_plus_ms(&p->retry_at, p->backoff_ms);
}

int dp_plugin_up(struct dp_plugin *p) {
  if (p->up) return 0;
  if (p->backoff_ms && !deadline_passed(&p->retry_at)) return -1;

  int rc = !strncmp(p->target, "tcp://", 6) ? connect_tcp(p) : spawn_child(p);
  if (rc != 0) {
    dp_plugin_down(p, "start failed");
    return -1;
  }

  // Provisionally up so dp_plugin_call() will talk to it during handshake.
  p->up = 1;

  if (authenticate(p) != 0) {
    dp_plugin_down(p, "authentication failed");
    return -1;
  }

  load_commands(p);
  if (!p->up) return -1;
  load_info(p);
  if (!p->up) return -1;
  load_config_keys(p);
  if (!p->up) return -1;

  if (p->hardware == 0) {
    log_error("plugin %s: reports no supported hardware, not using it", p->target);
    dp_plugin_down(p, "no hardware");
    return -1;
  }

  p->backoff_ms = 0;
  log_info("plugin %s: up, %zu commands", p->target, p->commands_n);
  return 0;
}

int dp_plugin_supports(const struct dp_plugin *p, const char *verb) {
  return p->up && strlist_has(p->commands, p->commands_n, verb);
}

int dp_plugin_config_supports(const struct dp_plugin *p, const char *key) {
  return strlist_has(p->config_keys, p->config_keys_n, key);
}

struct dp_plugin *dp_plugin_list(void) {
  return plugins;
}

static struct dp_plugin *plugin_entry(const char *target) {
  for (struct dp_plugin *p = plugins; p; p = p->next) {
    if (!strcmp(p->target, target)) return p;
  }
  struct dp_plugin *p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  p->target = strdup(target);
  if (!p->target) { free(p); return NULL; }
  p->fd = -1;
  struct dp_plugin **tail = &plugins;
  while (*tail) tail = &(*tail)->next;
  *tail = p;
  return p;
}

struct dp_plugin *dp_plugin_for(const char *target) {
  struct dp_plugin *p = plugin_entry(target);
  if (!p) return NULL;
  dp_plugin_up(p);
  return p;
}

int dp_plugin_config_set(struct dp_plugin *p, const char *key, char **argv, size_t argc) {
  const char *a[MAX_CONFIG_ARGV];
  size_t n = 0;
  a[n++] = "CONFIG";
  a[n++] = "SET";
  a[n++] = key;
  for (size_t i = 0; i < argc && n < MAX_CONFIG_ARGV; i++) a[n++] = argv[i];

  resp_object *r = dp_plugin_call(p, a, (int)n);
  if (!r) return -1;
  int bad = reply_is_error(r);
  if (bad) {
    log_error("plugin %s: CONFIG SET %s: %s", p->target, key, r->u.s ? r->u.s : "error");
  }
  resp_free(r);
  return bad ? -1 : 0;
}

int dp_start(void) {
  const struct linkd_daemon_cfg *cfg = daemon_cfg();

  for (struct linkd_plugin_cfg *c = cfg->plugins; c; c = c->next) {
    struct dp_plugin *p = plugin_entry(c->target);
    if (!p) return DP_RET_ERROR;
    p->optional = c->optional;
    // A plugin that cannot start yet is not fatal; the backoff retries it.
    dp_plugin_up(p);
  }
  return DP_RET_OK;
}

void dp_stop(void) {
  struct dp_plugin *p = plugins;
  while (p) {
    struct dp_plugin *next = p->next;
    if (p->fd >= 0) close(p->fd);
    if (p->pid > 0) {
      kill(p->pid, SIGTERM);
      waitpid(p->pid, NULL, 0);
    }
    strlist_free(&p->commands, &p->commands_n);
    strlist_free(&p->config_keys, &p->config_keys_n);
    free(p->target);
    free(p->user);
    free(p->pass);
    free(p);
    p = next;
  }
  plugins = NULL;
}

int dp_have_plugins(void) {
  return plugins != NULL;
}
