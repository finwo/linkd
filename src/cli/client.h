#ifndef __LINKD_CLI_CLIENT_H__
#define __LINKD_CLI_CLIENT_H__

// Connect to the daemon, send argv as a RESP command, render the reply.
// Returns a process exit code: 0 on success, 1 on error.
int linkd_call(int argc, const char **argv);

#endif // __LINKD_CLI_CLIENT_H__
