#ifndef BX_LIB_PRELOAD_OPS_H
#define BX_LIB_PRELOAD_OPS_H

char *bx_preload_find_runtime_module(
    const char *module_name,
    const char *install_subdir,
    const char *installed_path
);

int bx_preload_append_environment(
    const char *environment_name,
    const char *module_path,
    char **value_out
);

#endif
