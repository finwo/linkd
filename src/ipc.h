#ifndef __UNOS_IPC_H__
#define __UNOS_IPC_H__

#define UNOS_IPC_PATH "/run/unosd.sock"

// Server side (unosd)
int ipc_init(void);
int ipc_fd(void);
int ipc_handle(void); // accept and handle one client, returns 0 on handled, -1 on error
void ipc_fini(void);
void ipc_register(void);

#endif // __UNOS_IPC_H__
