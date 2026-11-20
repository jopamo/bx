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

enum bx_module_elf_symbol_kind {
    BX_MODULE_ELF_REQUIRED,
    BX_MODULE_ELF_WEAK,
    BX_MODULE_ELF_EXPORT,
};

typedef bool (*bx_module_elf_symbol_fn)(const char* name, size_t length,
                                       enum bx_module_elf_symbol_kind kind, void* user);
/* Visit undefined references and __ksymtab exports. Ordinary global
 * definitions are not exports. Extended symbol section indices are rejected.
 * Names borrow the supplied ELF bytes. On error, discard any collected names. */
bool bx_module_elf_symbols(const unsigned char* file, size_t size,
                           bx_module_elf_symbol_fn visit, void* user);
#endif

#endif
