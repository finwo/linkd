#ifndef __UNOS_DATAPLANE_REGISTRY_H__
#define __UNOS_DATAPLANE_REGISTRY_H__

#include "dataplane.h"

// Add a backend to the selection table. Rejects backends whose ABI does not
// match or which omit mandatory entry points.
int dp_register(const struct dp_ops *ops);

#endif // __UNOS_DATAPLANE_REGISTRY_H__
