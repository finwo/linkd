#ifndef __LINKD_IPC_H__
#define __LINKD_IPC_H__

#include <poll.h>

// RESP server. Speaks the Redis wire protocol over any number of unix and tcp
// listeners, as configured by `listen` in /etc/linkd.cnf.

#define IPC_ROLE_NONE     0   // connected, not authenticated
#define IPC_ROLE_READONLY 1   // IFQUERY, INFO, COMMAND, PING
#define IPC_ROLE_FULL     2   // + IFUP, IFDOWN, IFRELOAD

// Upper bound on fds this module contributes to the poll set.
#define IPC_MAX_LISTENERS 8
#define IPC_MAX_CONNS     64
#define IPC_MAX_POLLFDS   (IPC_MAX_LISTENERS + IPC_MAX_CONNS)

int  ipc_init(void);
void ipc_fini(void);

// Fill `pfds` with listeners and live connections. Returns the count.
int  ipc_pollfds(struct pollfd *pfds, int max);

// Service whatever `ipc_pollfds` filled in, after poll() has run.
void ipc_dispatch(struct pollfd *pfds, int n);

#endif // __LINKD_IPC_H__
