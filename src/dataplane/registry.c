#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "dataplane.h"

#include "registry.h"

#define DP_MAX_BACKENDS 16

static const struct dp_ops *backend[DP_MAX_BACKENDS];
static int                  backend_count = 0;
static const struct dp_ops *active        = NULL;

int dp_register(const struct dp_ops *ops) {
  if (!ops) return DP_RET_ERROR;

  if (ops->abi != LINKD_DATAPLANE_ABI) {
    fprintf(stderr, "dataplane: refusing backend `%s`: abi %u, expected %u\n",
            ops->name ? ops->name : "(unnamed)", ops->abi, LINKD_DATAPLANE_ABI);
    return DP_RET_ERROR;
  }

  if (!ops->name || !ops->probe || !ops->init) {
    fprintf(stderr, "dataplane: refusing incomplete backend\n");
    return DP_RET_ERROR;
  }

  if (backend_count >= DP_MAX_BACKENDS) {
    fprintf(stderr, "dataplane: backend table full, dropping `%s`\n", ops->name);
    return DP_RET_ERROR;
  }

  backend[backend_count++] = ops;
  return DP_RET_OK;
}

int dp_select(const char *force) {
  int i;

  if (force) {
    for (i = 0; i < backend_count; i++) {
      if (!strcasecmp(backend[i]->name, force)) {
        active = backend[i];
        fprintf(stderr, "dataplane: using `%s` (forced)\n", active->name);
        return DP_RET_OK;
      }
    }
    fprintf(stderr, "dataplane: no backend named `%s`\n", force);
    return DP_RET_ERROR;
  }

  // Plugins are probed before the built-in kernel backend, which is
  // registered first and always succeeds -- so iterate in reverse.
  for (i = backend_count - 1; i >= 0; i--) {
    if (backend[i]->probe() != DP_RET_OK) continue;
    active = backend[i];
    fprintf(stderr, "dataplane: using `%s`\n", active->name);
    return DP_RET_OK;
  }

  fprintf(stderr, "dataplane: no usable backend\n");
  return DP_RET_ERROR;
}

const struct dp_ops * dp_active(void) {
  return active;
}

int dp_init(void) {
  if (!active) return DP_RET_ERROR;
  return active->init();
}

void dp_fini(void) {
  if (!active) return;
  if (active->fini) active->fini();
  active = NULL;
}
