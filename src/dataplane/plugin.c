// Dataplane plugin loader.
//
// A single linkd image is identical on every machine. Hardware support arrives as a
// package: on a Broadcom box the `openbcm` package installs its kernel modules
// and drops a backend into LINKD_DATAPLANE_DIR. On anything else the directory
// is absent or empty and we quietly fall back to the built-in kernel backend.
//
// A plugin is a shared object exporting `struct dp_ops linkd_dataplane_ops`.

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "dataplane.h"

#include "registry.h"

int dp_plugins_load(const char *dir) {
  struct dirent *ent;
  DIR           *dh;
  char           path[1024];
  void          *handle;
  struct dp_ops *ops;
  size_t         len;
  int            loaded = 0;

  if (!dir) dir = LINKD_DATAPLANE_DIR;

  dh = opendir(dir);
  if (!dh) {
    // Not an error: no plugin directory simply means no hardware backends.
    return 0;
  }

  while ((ent = readdir(dh))) {
    len = strlen(ent->d_name);
    if (len < 4) continue;
    if (strcmp(ent->d_name + len - 3, ".so")) continue;

    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      fprintf(stderr, "dataplane: %s: %s\n", path, dlerror());
      continue;
    }

    ops = dlsym(handle, LINKD_DATAPLANE_SYM);
    if (!ops) {
      fprintf(stderr, "dataplane: %s: missing symbol `%s`\n", path, LINKD_DATAPLANE_SYM);
      dlclose(handle);
      continue;
    }

    if (dp_register(ops) != DP_RET_OK) {
      dlclose(handle);
      continue;
    }

    // Deliberately leaked: the ops table stays live for the process lifetime.
    loaded++;
  }

  closedir(dh);
  return loaded;
}
