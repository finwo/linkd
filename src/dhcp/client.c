#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rxi/log.h"

#include "config/daemon.h"
#include "dhcp.h"

#define TOKEN     "{{interface}}"
#define TOKEN_LEN (sizeof(TOKEN) - 1)

struct dhcp_proc {
  char             *ifname;
  pid_t             pid;
  struct dhcp_proc *next;
};

static struct dhcp_proc *procs;

// ---------------------------------------------------------------------------

static char *render(const char *src, const char *ifname) {
  size_t ilen = strlen(ifname);
  size_t n    = 0;
  for (const char *p = src; (p = strstr(p, TOKEN)) != NULL; p += TOKEN_LEN) n++;

  // Subtract before adding: ifname may be shorter than the token.
  size_t len = strlen(src) - (n * TOKEN_LEN) + (n * ilen);
  char  *out = malloc(len + 1);
  if (!out) {
    log_error("dhcp: out of memory rendering `%s'", src);
    return NULL;
  }

  char       *w = out;
  const char *r = src;
  for (const char *p; (p = strstr(r, TOKEN)) != NULL; r = p + TOKEN_LEN) {
    memcpy(w, r, (size_t)(p - r));
    w += p - r;
    memcpy(w, ifname, ilen);
    w += ilen;
  }
  strcpy(w, r);
  return out;
}

// ---------------------------------------------------------------------------
// PATH resolution

static char *find_in_path(const char *bin) {
  if (strchr(bin, '/')) return access(bin, X_OK) == 0 ? strdup(bin) : NULL;

  const char *path = getenv("PATH");
  if (!path || !*path) path = "/sbin:/usr/sbin:/bin:/usr/bin";

  char *dup = strdup(path);
  if (!dup) return NULL;

  char *found = NULL;
  for (char *dir = strtok(dup, ":"); dir; dir = strtok(NULL, ":")) {
    char cand[512];
    if ((size_t)snprintf(cand, sizeof(cand), "%s/%s", dir, bin) >= sizeof(cand)) continue;
    if (access(cand, X_OK) == 0) { found = strdup(cand); break; }
  }
  free(dup);
  return found;
}

// ---------------------------------------------------------------------------
// process table

static struct dhcp_proc *proc_find(const char *ifname) {
  for (struct dhcp_proc *p = procs; p; p = p->next) {
    if (!strcmp(p->ifname, ifname)) return p;
  }
  return NULL;
}

static void proc_drop(struct dhcp_proc *victim) {
  struct dhcp_proc **pp = &procs;
  while (*pp) {
    if (*pp == victim) {
      *pp = victim->next;
      free(victim->ifname);
      free(victim);
      return;
    }
    pp = &(*pp)->next;
  }
}

// ---------------------------------------------------------------------------

int dhcp_start(const char *ifname) {
  if (proc_find(ifname)) return 0;

  const struct linkd_daemon_cfg *cfg = daemon_cfg();
  size_t argc = cfg->dhcp_client_n;
  if (!argc) {
    log_error("dhcp: %s: no dhcp_client configured", ifname);
    return -1;
  }
  const char *const *tpl = (const char *const *)cfg->dhcp_client;

  char **argv = calloc(argc + 1, sizeof(char *));
  if (!argv) {
    log_error("dhcp: %s: out of memory", ifname);
    return -1;
  }
  for (size_t i = 0; i < argc; i++) {
    argv[i] = render(tpl[i], ifname);
    if (!argv[i]) {
      for (size_t j = 0; j < i; j++) free(argv[j]);
      free(argv);
      return -1;
    }
  }

  char *exe = find_in_path(argv[0]);
  if (!exe) {
    log_error("dhcp: %s: `%s' not found or not executable", ifname, argv[0]);
    for (size_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    log_error("dhcp: %s: fork: %s", ifname, strerror(errno));
    free(exe);
    for (size_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return -1;
  }

  if (pid == 0) {
    signal(SIGPIPE, SIG_DFL);
    execv(exe, argv);
    _exit(127);
  }

  struct dhcp_proc *p = calloc(1, sizeof(*p));
  if (!p) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    log_error("dhcp: %s: out of memory", ifname);
    free(exe);
    for (size_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return -1;
  }
  p->ifname = strdup(ifname);
  p->pid    = pid;
  p->next   = procs;
  procs     = p;

  log_info("dhcp: %s: started %s (pid %d)", ifname, exe, (int)pid);

  free(exe);
  for (size_t i = 0; i < argc; i++) free(argv[i]);
  free(argv);
  return 0;
}

void dhcp_stop(const char *ifname) {
  struct dhcp_proc *p = proc_find(ifname);
  if (!p) return;
  log_info("dhcp: %s: stopping pid %d", ifname, (int)p->pid);
  kill(p->pid, SIGTERM);
  waitpid(p->pid, NULL, 0);
  proc_drop(p);
}

void dhcp_stop_all(void) {
  while (procs) {
    struct dhcp_proc *p = procs;
    kill(p->pid, SIGTERM);
    waitpid(p->pid, NULL, 0);
    proc_drop(p);
  }
}

// Loud: the address survives until the lease expires, so the breakage is
// otherwise invisible for hours.
void dhcp_reap(void) {
  struct dhcp_proc *p = procs;
  while (p) {
    struct dhcp_proc *next = p->next;
    int status;
    if (waitpid(p->pid, &status, WNOHANG) == p->pid) {
      log_error("dhcp: %s: client pid %d exited (status %d), lease will not be renewed",
                p->ifname, (int)p->pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      proc_drop(p);
    }
    p = next;
  }
}
