#ifndef BX_SYSTEM_MODULE_SYSTEM_MAP_H
#define BX_SYSTEM_MODULE_SYSTEM_MAP_H

#ifdef __linux__
#include "module_elf.h"

/* Read text System.map records and visit __ksymtab_ exports. Names borrow
 * the supplied bytes and are not NUL-terminated. Discard collected names
 * on error. This reads a supplied map, not live kernel symbols. */
bool bx_module_system_map_exports(const char* data, size_t length,
                                  bx_module_elf_symbol_fn visit, void* user);
#endif

#endif
