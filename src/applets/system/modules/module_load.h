#ifndef BX_MODULE_LOAD_H
#define BX_MODULE_LOAD_H

#ifdef __linux__
#include <sys/stat.h>

char* bx_module_parameters(int argc, char** argv);
/* The caller supplies a readable, regular module fd and its fstat result. */
int bx_module_load_fd(int fd, const struct stat* st, const char* parameters);
#endif

#endif
