#include <glob.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "finwo/cnfparse.h"

#include "config.h"

#define CFG_MAX_REG 256

static int               handler_count = 0;
static const char       *handler_name[CFG_MAX_REG];
static cfg_directive_fn  handler_fn[CFG_MAX_REG];

void cfg_register_directive(const char *name, cfg_directive_fn fn) {
  if (handler_count >= CFG_MAX_REG) {
    fprintf(stderr, "config: handler table full, dropping `%s`\n", name);
    return;
  }
  handler_name[handler_count] = name;
  handler_fn  [handler_count] = fn;
  handler_count++;
}

int cfg_parse(const char *wd, FILE *fd, void *user) {
  struct cnf_directive *dir = NULL;
  glob_t globbuf;
  int globflags;
  int i;

  char *strtmp = NULL;
  FILE *nfd;

  for(;;) {
    if (dir) cnf_directive_free(dir);
    dir = cnf_directive_read(fd);
    if (!dir) break; // EOF = done

cfg_parse_reparse:

    // Core handlers
    if (!strcasecmp("include", dir->name)) {
      globflags = GLOB_ERR;
      for ( i = 0 ; i < (int)dir->argc ; i++ ) {
        if (dir->argv[i][0] == '/') {
          strtmp = calloc(strlen(dir->argv[i])+1, 1);
          strcpy(strtmp, dir->argv[i]);
        } else {
          strtmp = malloc(snprintf(NULL, 0, "%s/%s", wd, dir->argv[i])+1);
          sprintf(strtmp, "%s/%s", wd, dir->argv[i]);
        }
        glob(strtmp, globflags, NULL, &globbuf);
        free(strtmp);
        globflags = GLOB_ERR | GLOB_APPEND;
      }
      for( i = 0 ; i < (int)globbuf.gl_pathc ; i++ ) {
        strtmp = calloc(strlen(globbuf.gl_pathv[i])+1, 1);
        strcpy(strtmp, globbuf.gl_pathv[i]);
        dirname(strtmp);
        nfd = fopen(globbuf.gl_pathv[i], "r");
        if (!nfd) {
          perror("Could not open config file for reading");
          free(strtmp);
          globfree(&globbuf);
          return CFG_RET_ERROR;
        }
        if (cfg_parse(strtmp, nfd, user) < 0) {
          fprintf(stderr, "Error during reading configuration from %s\n", globbuf.gl_pathv[i]);
          fclose(nfd);
          free(strtmp);
          globfree(&globbuf);
          return CFG_RET_ERROR;
        }
        fclose(nfd);
        free(strtmp);
      }
      globfree(&globbuf);
      continue;
    }

    // Dynamic handlers
    for(i = 0 ; i < handler_count ; i++) {
      if (!strcasecmp(handler_name[i], dir->name)) {
        dir = handler_fn[i](fd, dir, user);
        if (dir) goto cfg_parse_reparse;
        break;
      }
    }

    if (i == handler_count) {
      // Here = not found
      fprintf(stderr, "Unknown directive: %s\n", dir->name);
      return CFG_RET_ERROR;
    }
  }

  // Cleanup here
  if (dir) cnf_directive_free(dir);

  return CFG_RET_OK;
}
