#ifndef __LINKD_NETLINK_H__
#define __LINKD_NETLINK_H__

#include <stdint.h>

// Raw NETLINK_ROUTE mirror. No libnl/libmnl dependency by design.
//
// Ownership: nl_run() resolves dp_active() on every dispatch, never caches it.
// The VRF map (master table ids, member enslavement) stays inside netlink.c;
// only the numeric table id crosses struct dp_ops.

int nl_open(void);                                  // bound NETLINK_ROUTE fd, or -1
int nl_run(int fd, int resync_interval_s);          // poll loop; 0 on stop, -1 on fatal
void nl_request_stop(void);                         // async-signal-safe stop flag
int nl_dump(int fd, uint16_t type, int family);     // one GET*+DUMP, replies via handle_nlmsg
int nl_resync(int fd);                              // filter_reset + dumps + dp resync; 0 ok, -1 fail

#endif // __LINKD_NETLINK_H__
