#include <glob.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "finwo/cnfparse.h"

#include "config.h"

#define CFG_MAX_NS  8
#define CFG_MAX_REG 256

struct cfg_ns {
  char               name[32];
  int                count;
  const char        *h_name[CFG_MAX_REG];
  cfg_directive_fn   h_fn[CFG_MAX_REG];
};

static struct cfg_ns ns_tab[CFG_MAX_NS];
static int ns_count = 0;

static struct cnf_directive *cfg_handle_source_directory(FILE *fd, struct cnf_directive *dir, void *user);

struct cfg_ns *cfg_ns_get(const char *name) {
  int i;
  for (i = 0; i < ns_count; i++) {
    if (!strcmp(ns_tab[i].name, name)) return &ns_tab[i];
  }
  if (ns_count >= CFG_MAX_NS) {
    fprintf(stderr, "config: namespace table full, dropping `%s`\n", name);
    return NULL;
  }
  snprintf(ns_tab[ns_count].name, sizeof(ns_tab[ns_count].name), "%s", name);
  ns_tab[ns_count].count = 0;
  struct cfg_ns *ns = &ns_tab[ns_count++];
  // Every namespace supports source-directory -> source <dir>/* expansion
  cfg_register_directive(ns, "source-directory", cfg_handle_source_directory);
  return ns;
}

void cfg_register_directive(struct cfg_ns *ns, const char *name, cfg_directive_fn fn) {
  if (!ns) return;
  if (ns->count >= CFG_MAX_REG) {
    fprintf(stderr, "config: handler table full for ns `%s`, dropping `%s`\n", ns->name, name);
    return;
  }
  ns->h_name[ns->count] = name;
  ns->h_fn[ns->count] = fn;
  ns->count++;
}

// source-directory handler: transforms `source-directory <dir>` into `source <dir>/*`
static struct cnf_directive *cfg_handle_source_directory(FILE *fd, struct cnf_directive *dir, void *user) {
  (void)fd; (void)user;
  struct cnf_directive *ndir = NULL;
  char *pattern = NULL;

  if (dir->argc != 1) {
    fprintf(stderr, "`source-directory` directive accepts only 1 argument\n");
    // Return as unknown to let caller handle error? For now treat as error via exit-style?
    // Keep consistent with other handlers: print and return dir as unknown for outer to error.
    // But we want to synthesize source, so we need at least 1 arg.
    // If argc !=1, free and return NULL to signal error via cfg_parse's unknown path?
    cnf_directive_free(dir);
    return NULL;
  }

  // Synthesize a `source <dir>/*` directive
  // Allocate new directive manually
  ndir = calloc(1, sizeof(*ndir));
  ndir->name = strdup("source");
  ndir->argc = 1;
  ndir->argv = calloc(1, sizeof(char*));
  // Append /* if not already ending with /*
  if (dir->argv[0][strlen(dir->argv[0])-1] == '/') {
    pattern = malloc(strlen(dir->argv[0]) + 2); // "/" + "*"
    sprintf(pattern, "%s*", dir->argv[0]);
  } else {
    // if dir is like "/etc/network/interfaces.d", we want "/etc/network/interfaces.d/*"
    pattern = malloc(strlen(dir->argv[0]) + 3);
    sprintf(pattern, "%s/*", dir->argv[0]);
  }
  ndir->argv[0] = pattern;
  // we own pattern, will be freed via cnf_directive_free later
  cnf_directive_free(dir);
  return ndir;
}

int cfg_parse(struct cfg_ns *ns, const char *wd, FILE *fd, void *user) {
  struct cnf_directive *dir = NULL;
  glob_t globbuf;
  int globflags;
  int i;

  char *strtmp = NULL;
  FILE *nfd;

  if (!ns) {
    fprintf(stderr, "config: NULL namespace\n");
    return CFG_RET_ERROR;
  }

  for(;;) {
    if (dir) cnf_directive_free(dir);
    dir = cnf_directive_read(fd);
    if (!dir) break; // EOF = done

  cfg_parse_reparse:

    // Core handlers: source and source-directory (source-directory is via handler table, but we also handle source here as built-in)
    if (!strcasecmp("source", dir->name)) {
      globflags = GLOB_ERR;
      for ( i = 0 ; i < (int)dir->argc ; i++ ) {
        if (dir->argv[i][0] == '/') {
          strtmp = calloc(strlen(dir->argv[i])+1, 1);
          strcpy(strtmp, dir->argv[i]);
        } else {
          // wd may be NULL for IPC (no filesystem context), then treat as absolute or fail
          if (wd) {
            strtmp = malloc(snprintf(NULL, 0, "%s/%s", wd, dir->argv[i])+1);
            sprintf(strtmp, "%s/%s", wd, dir->argv[i]);
          } else {
            strtmp = calloc(strlen(dir->argv[i])+1, 1);
            strcpy(strtmp, dir->argv[i]);
          }
        }
        glob(strtmp, globflags, NULL, &globbuf);
        free(strtmp);
        globflags = GLOB_ERR | GLOB_APPEND;
      }
      for ( i = 0 ; i < (int)globbuf.gl_pathc ; i++ ) {
        strtmp = calloc(strlen(globbuf.gl_pathv[i])+1, 1);
        strcpy(strtmp, globbuf.gl_pathv[i]);
        char *dname = strdup(strtmp);
        dirname(dname);
        // Use wd as dirname for nested relative sources? Keep dname as wd for nested
        // But for simplicity, use dname
        nfd = fopen(globbuf.gl_pathv[i], "r");
        if (!nfd) {
          perror("Could not open config file for reading");
          free(strtmp);
          free(dname);
          globfree(&globbuf);
          return CFG_RET_ERROR;
        }
        if (cfg_parse(ns, dname, nfd, user) < 0) {
          fprintf(stderr, "Error during reading configuration from %s\n", globbuf.gl_pathv[i]);
          fclose(nfd);
          free(strtmp);
          free(dname);
          globfree(&globbuf);
          return CFG_RET_ERROR;
        }
        fclose(nfd);
        free(strtmp);
        free(dname);
      }
      globfree(&globbuf);
      continue;
    }

    // Dynamic handlers
    for(i = 0 ; i < ns->count ; i++) {
      if (!strcasecmp(ns->h_name[i], dir->name)) {
        dir = ns->h_fn[i](fd, dir, user);
        if (dir) goto cfg_parse_reparse;
        break;
      }
    }

    if (i == ns->count) {
      // Here = not found
      fprintf(stderr, "Unknown directive: %s\n", dir->name);
      return CFG_RET_ERROR;
    }
  }

  // Cleanup here
  if (dir) cnf_directive_free(dir);

  return CFG_RET_OK;
}


