#ifndef __UNOS_CLI_REGISTRY_H__
#define __UNOS_CLI_REGISTRY_H__

typedef int (*cli_main_fn)(int argc, char **argv);

void cli_register(const char *name, cli_main_fn fn);
cli_main_fn cli_find(const char *name);

#endif // __UNOS_CLI_REGISTRY_H__
