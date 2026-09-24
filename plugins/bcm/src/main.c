// Broadcom XGS plugin for linkd: a RESP server on stdin/stdout.
//
// Skeleton. Every operation below is unimplemented and answers with an error,
// which linkd surfaces rather than silently treating as applied. INFO reports
// hardware_detected, which is how linkd decides whether to use this plugin at
// all -- see the /dev/linux-kernel-bde probe in hardware_present().
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "finwo/resp.h"

static int hardware_present(void) {
  // The legacy BDE character device. A box with the module loaded and a
  // supported ASIC has it; anything else does not.
  //
  // Note /proc/devices on captured hardware also lists linux_ngbde (the newer
  // BDE) alongside this one. Which of the two bcm.so binds to is still an
  // open question, so this check may need widening.
  return access("/dev/linux-kernel-bde", F_OK) == 0;
}

static void emit(resp_object *o) {
  char  *buf = NULL;
  size_t len = 0;
  if (resp_serialize(o, &buf, &len) == 0) {
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    free(buf);
  }
  resp_free(o);
}

static void reply_ok(void)                { emit(resp_simple_init("OK")); }
static void reply_err(const char *msg)    { emit(resp_error_init(msg)); }

static void reply_commands(void) {
  resp_object *a = resp_array_init();
  resp_array_append_bulk(a, "COMMAND");
  resp_array_append_bulk(a, "INFO");
  resp_array_append_bulk(a, "PORT");
  resp_array_append_bulk(a, "RIF");
  resp_array_append_bulk(a, "NEIGH");
  resp_array_append_bulk(a, "ROUTE");
  resp_array_append_bulk(a, "RESYNC");
  emit(a);
}

// resp.c has no bulk constructor, only simple/error/array, so build it here.
static resp_object *bulk_init(const char *s) {
  resp_object *o = calloc(1, sizeof(*o));
  if (!o) return NULL;
  o->type = RESPT_BULK;
  o->u.s  = strdup(s);
  return o;
}

static void reply_info(void) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "# Server\r\n"
           "name:bcm\r\n"
           "version:0.1.0\r\n"
           "\r\n"
           "# Stats\r\n"
           "hardware_detected:%d\r\n",
           hardware_present() ? 1 : 0);
  emit(bulk_init(buf));
}

static const char *arg(const resp_object *cmd, size_t i) {
  if (!cmd || cmd->type != RESPT_ARRAY || i >= cmd->u.arr.n) return NULL;
  const resp_object *e = &cmd->u.arr.elem[i];
  if (e->type != RESPT_BULK && e->type != RESPT_SIMPLE) return NULL;
  return e->u.s;
}

int main(void) {
  for (;;) {
    resp_object *cmd = resp_read(STDIN_FILENO);
    if (!cmd) break; // EOF or malformed: linkd will restart us

    const char *verb = arg(cmd, 0);
    if (!verb) {
      reply_err("ERR malformed command");
      resp_free(cmd);
      continue;
    }

    if (!strcasecmp(verb, "COMMAND")) {
      reply_commands();
    } else if (!strcasecmp(verb, "INFO")) {
      reply_info();
    } else if (!strcasecmp(verb, "AUTH")) {
      reply_ok();
    } else if (!strcasecmp(verb, "PORT")   || !strcasecmp(verb, "RIF") ||
               !strcasecmp(verb, "NEIGH")  || !strcasecmp(verb, "ROUTE") ||
               !strcasecmp(verb, "RESYNC")) {
      // TODO: program the ASIC through the OpenBCM SDK.
      reply_err("ERR not implemented");
    } else {
      reply_err("ERR unknown command");
    }

    resp_free(cmd);
  }
  return 0;
}
