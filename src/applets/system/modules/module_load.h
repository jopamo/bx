#ifndef BX_MODULE_LOAD_H
#define BX_MODULE_LOAD_H

#ifdef __linux__
#include <sys/stat.h>
#include "module_index.h"

struct bx_diag_ctx;

char* bx_module_parameters(int argc, char** argv);
/* The caller supplies a readable, regular module fd and its fstat result. */
int bx_module_load_fd(int fd, const struct stat* st, const char* parameters);
bool bx_module_remove_name_valid(struct bx_module_path name);
/* Remove without force or waiting. Return -1 and set errno on failure. */
int bx_module_remove(struct bx_module_path name);
void bx_module_remove_report(struct bx_diag_ctx* diag, struct bx_module_path name, int error);
#endif

#endif
