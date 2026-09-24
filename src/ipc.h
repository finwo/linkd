#ifndef __LINKD_IPC_H__
#define __LINKD_IPC_H__


// Server side (linkd)
int ipc_init(void);
int ipc_fd(void);
int ipc_handle(void); // accept and handle one client, returns 0 on handled, -1 on error
void ipc_fini(void);
void ipc_register(void);

#endif // __LINKD_IPC_H__
