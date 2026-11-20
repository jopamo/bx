#ifndef BX_SYSTEM_MODULE_ELF_H
#define BX_SYSTEM_MODULE_ELF_H

#include <stdbool.h>
#include <stddef.h>

/* Return a borrowed, validated .modinfo section of an uncompressed ELF module. */
bool bx_module_elf_info(const unsigned char* file, size_t size, const char** data, size_t* length);

#endif
