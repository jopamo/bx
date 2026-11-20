#ifndef BX_SYSTEM_MODULE_ELF_H
#define BX_SYSTEM_MODULE_ELF_H

#include <stdbool.h>
#include <stddef.h>

/* Return a borrowed, validated .modinfo section of an uncompressed ELF module. */
bool bx_module_elf_info(const unsigned char* file, size_t size, const char** data, size_t* length);

#ifdef __linux__
struct bx_diag_ctx;

/* Borrow a regular-file descriptor; on success transfer ownership of the
 * decoded (or uncompressed) bytes to the caller. */
unsigned char* bx_module_elf_read(int fd, const char* path, size_t* length,
                                  struct bx_diag_ctx* diag);
#endif

#endif
