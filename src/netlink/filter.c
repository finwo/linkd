#include <string.h>
#include <sys/socket.h>

#include <linux/rtnetlink.h>

#include "filter.h"

#define FILTER_MAX_TRACKED 64
#define FILTER_MAX_EXCLUDED 16

static uint32_t tracked[FILTER_MAX_TRACKED];
static int tracked_count = 0;
static uint32_t excluded[FILTER_MAX_EXCLUDED];
static int excluded_count = 0;

static int find_u32(const uint32_t *arr, int n, uint32_t v) {
  int i;
  for (i = 0; i < n; i++) {
    if (arr[i] == v) return i;
  }
  return -1;
}

void nl_filter_reset(void) {
  tracked_count = 0;
  excluded_count = 0;
  tracked[tracked_count++] = RT_TABLE_MAIN;
}

void nl_filter_track(uint32_t table) {
  if (table == RT_TABLE_LOCAL) return;
  if (find_u32(tracked, tracked_count, table) >= 0) return;
  if (tracked_count >= FILTER_MAX_TRACKED) return;
  tracked[tracked_count++] = table;
}

void nl_filter_untrack(uint32_t table) {
  int idx;
  if (table == RT_TABLE_MAIN) return;
  idx = find_u32(tracked, tracked_count, table);
  if (idx < 0) return;
  tracked[idx] = tracked[--tracked_count];
}

void nl_filter_exclude(uint32_t table) {
  if (find_u32(excluded, excluded_count, table) >= 0) return;
  if (excluded_count >= FILTER_MAX_EXCLUDED) return;
  excluded[excluded_count++] = table;
}

bool nl_filter_mgmt_name(const char *ifname) {
  if (!ifname) return false;
  if (!strcmp(ifname, "mgmt")) return true;
  if (!strcmp(ifname, "mgmt0")) return true;
  return false;
}

bool nl_table_allowed(uint32_t table) {
  if (table == RT_TABLE_LOCAL) return false;
  if (find_u32(excluded, excluded_count, table) >= 0) return false;
  return find_u32(tracked, tracked_count, table) >= 0;
}

bool nl_route_allowed(int family, uint8_t type, uint8_t scope, uint32_t table) {
  if (family != AF_INET && family != AF_INET6) return false;
  if (type != RTN_UNICAST) return false;
  if (scope == RT_SCOPE_HOST) return false;
  return nl_table_allowed(table);
}
