#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "finwo/cnfparse.h"

#include "util/config.h"

#include "ifaces.h"

static struct unos_iface *ifaces = NULL;
static struct unos_iface *ifaces_tail = NULL;

// Allocate an iface with every "unset" field at its sentinel.
//
// calloc() zeroes, which is the correct "unset" for pointers and for counters
// like mtu, but WRONG for any field whose valid range includes 0. Those use -1
// as the sentinel and must be set explicitly.
//
// This exists because bridge_vlan_aware was documented as "-1 unset" but never
// initialised, so it sat at 0 and the `bridge_vlan_aware >= 0` test classified
// EVERY interface as a bridge. It was masked only because creating a bridge on
// an existing device fails harmlessly. Centralising the sentinels here means a
// new tri-state field cannot repeat that by being forgotten at one of the two
// allocation sites.
static struct unos_iface * iface_new(const char *name) {
  struct unos_iface *ifc = calloc(1, sizeof(*ifc));
  if (!ifc) return NULL;
  ifc->name              = name ? strdup(name) : NULL;
  ifc->vlan_id           = -1;
  ifc->bridge_vlan_aware = -1;
  ifc->vrf_table         = -1;
  return ifc;
}

// Is this interface a bridge?
//
// Single source of truth: this was duplicated in cli/unosd.c and ipc.c, which
// is how the two could drift. Explicit bridge-* directives win; the name
// prefix is only a fallback, and never applies to something already declared
// a VLAN or enslaved to a VRF.
bool iface_is_bridge(const struct unos_iface *ifc) {
  if (!ifc) return false;
  if (ifc->bridge_ports) return true;
  if (ifc->bridge_stp) return true;
  if (ifc->bridge_vlan_aware >= 0) return true;
  if (ifc->vlan_id >= 0) return false;
  if (ifc->vrf_table >= 0) return false;
  return !strncmp(ifc->name, "br", 2);
}

// Is this interface a VRF? `vrf-table` makes it one; `vrf <name>` makes it a
// MEMBER of one, which is a different thing entirely.
bool iface_is_vrf(const struct unos_iface *ifc) {
  return ifc && ifc->vrf_table >= 0;
}

// `vrf-table auto`: lowest free id at or above VRF_TABLE_AUTO_BASE.
//
// 1001 is not arbitrary -- it is where Cumulus starts, and the S5248F-ON we
// captured has its `mgmt` VRF on exactly table 1001 (PROGRESS.md 2.1). Matching
// that means a config moved off a Cumulus box lands on the same tables.
// Deliberately skips 253/254/255 by construction, and skips any id already
// claimed explicitly, so `auto` and a hardcoded id cannot collide.
#define VRF_TABLE_AUTO_BASE 1001
static int vrf_table_auto(const char *name) {
  struct unos_iface *cur;
  int candidate;
  for (candidate = VRF_TABLE_AUTO_BASE; candidate < 2147483647; candidate++) {
    int taken = 0;
    for (cur = ifaces; cur; cur = cur->next) {
      // An interface re-parsing its own stanza must keep its table.
      if (name && cur->name && !strcmp(cur->name, name)) continue;
      if (cur->vrf_table == candidate) { taken = 1; break; }
    }
    if (!taken) return candidate;
  }
  fprintf(stderr, "vrf-table auto: no free table id\n");
  exit(1);
}

struct unos_iface * ifaces_list(void) {
  return ifaces;
}

struct unos_iface * ifaces_find(const char *name) {
  struct unos_iface *cur;
  for (cur = ifaces; cur; cur = cur->next) {
    if (!strcmp(cur->name, name)) return cur;
  }
  return NULL;
}

void ifaces_set_auto(const char *name, int flag) {
  struct unos_iface *cur = ifaces_find(name);
  if (cur) {
    cur->auto_flag = flag;
    return;
  }
  // Create placeholder for auto without iface block yet (common in Debian: auto comes before iface)
  cur = iface_new(name);
  cur->auto_flag = flag;
  if (ifaces_tail) {
    ifaces_tail->next = cur;
  } else {
    ifaces = cur;
  }
  ifaces_tail = cur;
}

void ifaces_free(void) {
  struct unos_iface *cur = ifaces;
  struct unos_iface *next;
  struct iface_addr *a, *anext;
  while (cur) {
    next = cur->next;
    free(cur->name);
    free(cur->method);
    free(cur->family);
    free(cur->gateway);
    free(cur->broadcast);
    free(cur->hwaddress);
    free(cur->vlan_raw_device);
    free(cur->pre_up);
    free(cur->post_up);
    free(cur->pre_down);
    free(cur->post_down);
    free(cur->bridge_ports);
    free(cur->bridge_stp);
    free(cur->vrf_master);
    for (a = cur->addrs; a; a = anext) {
      anext = a->next;
      free(a->address);
      free(a->netmask);
      free(a);
    }
    free(cur);
    cur = next;
  }
  ifaces = NULL;
  ifaces_tail = NULL;
}

static void iface_add_addr(struct unos_iface *iface, const char *addr, const char *netmask) {
  struct iface_addr *a = calloc(1, sizeof(*a));
  a->address = strdup(addr);
  if (netmask) a->netmask = strdup(netmask);
  a->next = NULL;
  // append
  if (!iface->addrs) {
    iface->addrs = a;
  } else {
    struct iface_addr *cur = iface->addrs;
    while (cur->next) cur = cur->next;
    cur->next = a;
  }
}

// auto <if> [<if> ...]
struct cnf_directive * cfg_parse_auto(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)fd; (void)user;
  int i;
  for (i = 0; i < (int)dir->argc; i++) {
    ifaces_set_auto(dir->argv[i], 1);
  }
  cnf_directive_free(dir);
  return NULL;
}

// iface <name> [inet|inet6 <method>] -- cumulus style `iface <name>` preferred (no inet)
struct cnf_directive * cfg_parse_iface(FILE *fd, struct cnf_directive *dir, void *user) {
  struct unos_iface *iface;
  (void)user;

  if (dir->argc < 1 || dir->argc > 3) {
    fprintf(stderr, "`iface` directive: expected `iface <name>` or `iface <name> inet <method>`\n");
    exit(1);
  }

  const char *name = dir->argv[0];
  const char *family = NULL;
  const char *method = NULL;

  if (dir->argc == 1) {
    // Cumulus style: iface <name> -- defaults to static, no family
    method = "static";
  } else if (dir->argc == 3) {
    // Traditional: iface <name> inet static
    family = dir->argv[1];
    method = dir->argv[2];
    if (strcasecmp(family, "inet") && strcasecmp(family, "inet6")) {
      fprintf(stderr, "`iface %s` unknown family: %s (expected inet/inet6)\n", name, family);
      exit(1);
    }
  } else {
    fprintf(stderr, "`iface` directive: expected `iface <name>` or `iface <name> inet <method>`\n");
    exit(1);
  }

  // Find existing placeholder from auto, or create new
  iface = ifaces_find(name);
  int was_placeholder = 0;
  if (iface && !iface->method && !iface->family && !iface->addrs) {
    // placeholder from auto -- reuse
    was_placeholder = 1;
  } else if (iface) {
    // Duplicate iface block -- for now we merge? Cumulus allows multiple iface stanzas for same if
    // but we treat as error to keep simple, unless it's same name with different family
    // Allow merging: if family/method differ, we keep as separate? For now, error on exact duplicate
    // Check if same iface already has addrs/method -- if so, create new entry with same name but as separate node
    // To keep diff-apply simple, we allow multiple entries with same name as separate list nodes
    // So we will create a new node even if name exists, unless it was placeholder
    was_placeholder = 0;
  }

  if (iface && was_placeholder) {
    // reuse placeholder
    free(iface->method);
    free(iface->family);
    iface->method = method ? strdup(method) : NULL;
    iface->family = family ? strdup(family) : NULL;
    // keep auto_flag
  } else {
    // create new
    iface = iface_new(name);
    iface->method = method ? strdup(method) : NULL;
    iface->family = family ? strdup(family) : NULL;
    // check if auto was previously set for this name via placeholder? Already handled, but if not placeholder, check if any placeholder had auto
    // Actually ifaces_set_auto may have created placeholder with auto, we already handled.
    // If no placeholder, auto_flag stays 0 until auto directive is parsed (order may be auto before or after iface)
    // So we need to check if there was a prior auto for this name that wasn't yet linked -- but our placeholder logic already covers.
    if (ifaces_tail) {
      ifaces_tail->next = iface;
    } else {
      ifaces = iface;
    }
    ifaces_tail = iface;
  }

  // Handle vlan dot notation: swp1.100 -> raw_device swp1, vlan_id 100
  char *dot = strrchr(iface->name, '.');
  if (dot && !iface->vlan_raw_device) {
    char *end = NULL;
    long vid = strtol(dot+1, &end, 10);
    if (end != dot+1 && *end == '\0' && vid >= 1 && vid <= 4094) {
      iface->vlan_id = (int)vid;
      // Only set raw_device if not already set via explicit vlan-raw-device
      // Keep as is, will be resolved later if needed
    }
  }

  for(;;) {
    if (dir) cnf_directive_free(dir);
    dir = cnf_directive_read(fd);
    if (!dir) break;

    if(0) {}
    else if (!strcasecmp("address", dir->name)) {
      if (dir->argc < 1 || dir->argc > 2) {
        fprintf(stderr, "`address` expects 1 or 2 args (address + optional netmask)\n");
        exit(1);
      }
      const char *addr = dir->argv[0];
      const char *nm = dir->argc > 1 ? dir->argv[1] : NULL;
      iface_add_addr(iface, addr, nm);
    }
    else if (!strcasecmp("netmask", dir->name)) {
      if (dir->argc != 1) {
        fprintf(stderr, "`netmask` expects 1 arg\n");
        exit(1);
      }
      // Attach netmask to last address without netmask
      struct iface_addr *a = iface->addrs;
      if (!a) {
        fprintf(stderr, "`netmask` without preceding `address`\n");
        exit(1);
      }
      while (a->next) a = a->next;
      free(a->netmask);
      a->netmask = strdup(dir->argv[0]);
    }
    else if (!strcasecmp("broadcast", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`broadcast` expects 1 arg\n"); exit(1); }
      free(iface->broadcast);
      iface->broadcast = strdup(dir->argv[0]);
    }
    else if (!strcasecmp("gateway", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`gateway` expects 1 arg\n"); exit(1); }
      free(iface->gateway);
      iface->gateway = strdup(dir->argv[0]);
    }
    else if (!strcasecmp("mtu", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`mtu` expects 1 arg\n"); exit(1); }
      iface->mtu = atoi(dir->argv[0]);
    }
    else if (!strcasecmp("hwaddress", dir->name) || !strcasecmp("hw-address", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`hwaddress` expects 1 arg\n"); exit(1); }
      free(iface->hwaddress);
      iface->hwaddress = strdup(dir->argv[0]);
    }
    else if (!strcasecmp("vlan-raw-device", dir->name) || !strcasecmp("vlan_raw_device", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`vlan-raw-device` expects 1 arg\n"); exit(1); }
      free(iface->vlan_raw_device);
      iface->vlan_raw_device = strdup(dir->argv[0]);
    }
    else if (!strcasecmp("vlan-id", dir->name) || !strcasecmp("vlan_id", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`vlan-id` expects 1 arg\n"); exit(1); }
      iface->vlan_id = atoi(dir->argv[0]);
    }
    else if (!strcasecmp("pre-up", dir->name) || !strcasecmp("pre_up", dir->name)) {
      if (dir->argc < 1) { fprintf(stderr, "`pre-up` expects at least 1 arg\n"); exit(1); }
      // Join argv with spaces
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->pre_up);
      iface->pre_up = cmd;
    }
    else if (!strcasecmp("post-up", dir->name) || !strcasecmp("post_up", dir->name)) {
      if (dir->argc < 1) { fprintf(stderr, "`post-up` expects at least 1 arg\n"); exit(1); }
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->post_up);
      iface->post_up = cmd;
    }
    else if (!strcasecmp("pre-down", dir->name) || !strcasecmp("pre_down", dir->name)) {
      if (dir->argc < 1) { fprintf(stderr, "`pre-down` expects at least 1 arg\n"); exit(1); }
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->pre_down);
      iface->pre_down = cmd;
    }
    else if (!strcasecmp("post-down", dir->name) || !strcasecmp("post_down", dir->name)) {
      if (dir->argc < 1) { fprintf(stderr, "`post-down` expects at least 1 arg\n"); exit(1); }
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->post_down);
      iface->post_down = cmd;
    }
    else if (!strcasecmp("up", dir->name)) {
      // alias for post-up
      if (dir->argc < 1) { fprintf(stderr, "`up` expects at least 1 arg\n"); exit(1); }
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->post_up);
      iface->post_up = cmd;
    }
    else if (!strcasecmp("down", dir->name)) {
      if (dir->argc < 1) { fprintf(stderr, "`down` expects at least 1 arg\n"); exit(1); }
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->pre_down);
      iface->pre_down = cmd;
    }
    else if (!strcasecmp("bridge-ports", dir->name) || !strcasecmp("bridge_ports", dir->name)) {
      if (dir->argc < 1) { fprintf(stderr, "`bridge-ports` expects at least 1 arg\n"); exit(1); }
      size_t len = 0;
      for (int i=0;i<(int)dir->argc;i++) len += strlen(dir->argv[i])+1;
      char *cmd = calloc(1, len+1);
      for (int i=0;i<(int)dir->argc;i++) {
        strcat(cmd, dir->argv[i]);
        if (i+1 < (int)dir->argc) strcat(cmd, " ");
      }
      free(iface->bridge_ports);
      iface->bridge_ports = cmd;
    }
    else if (!strcasecmp("bridge-stp", dir->name) || !strcasecmp("bridge_stp", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`bridge-stp` expects 1 arg (on/off)\n"); exit(1); }
      free(iface->bridge_stp);
      iface->bridge_stp = strdup(dir->argv[0]);
    }
    else if (!strcasecmp("bridge-vlan-aware", dir->name) || !strcasecmp("bridge_vlan_aware", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`bridge-vlan-aware` expects 1 arg (yes/no)\n"); exit(1); }
      if (!strcasecmp(dir->argv[0], "yes") || !strcasecmp(dir->argv[0], "on") || !strcasecmp(dir->argv[0], "1")) iface->bridge_vlan_aware = 1;
      else if (!strcasecmp(dir->argv[0], "no") || !strcasecmp(dir->argv[0], "off") || !strcasecmp(dir->argv[0], "0")) iface->bridge_vlan_aware = 0;
      else { fprintf(stderr, "`bridge-vlan-aware` expects yes/no\n"); exit(1); }
    }
    // `vrf <name>`: enslave this interface to an existing VRF device.
    else if (!strcasecmp("vrf", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`vrf` expects 1 arg (vrf device name)\n"); exit(1); }
      free(iface->vrf_master);
      iface->vrf_master = strdup(dir->argv[0]);
    }
    // `vrf-table <id|auto>`: THIS interface is a VRF device with that table.
    // Cumulus spells the automatic case `auto`; we allocate from a private
    // range rather than colliding with main(254)/local(255)/default(253).
    else if (!strcasecmp("vrf-table", dir->name) || !strcasecmp("vrf_table", dir->name)) {
      if (dir->argc != 1) { fprintf(stderr, "`vrf-table` expects 1 arg (table id or `auto`)\n"); exit(1); }
      if (!strcasecmp(dir->argv[0], "auto")) {
        iface->vrf_table = vrf_table_auto(iface->name);
      } else {
        char *end = NULL;
        long v = strtol(dir->argv[0], &end, 10);
        if (!end || *end || v <= 0 || v > 2147483647L) {
          fprintf(stderr, "`vrf-table` expects a positive table id or `auto`, got `%s`\n", dir->argv[0]);
          exit(1);
        }
        if (v == 253 || v == 254 || v == 255) {
          fprintf(stderr, "`vrf-table` %ld is a reserved table (default/main/local)\n", v);
          exit(1);
        }
        iface->vrf_table = (int)v;
      }
    }
    else {
      break;
    }
  }

  return dir;
}

void ifaces_register(void) {
  struct cfg_ns *ns = cfg_ns_get("interfaces");
  cfg_register_directive(ns, "auto", cfg_parse_auto);
  cfg_register_directive(ns, "allow-auto", cfg_parse_auto);
  cfg_register_directive(ns, "allow-hotplug", cfg_parse_auto);
  cfg_register_directive(ns, "iface", cfg_parse_iface);
}
