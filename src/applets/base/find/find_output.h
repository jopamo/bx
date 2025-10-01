#ifndef BX_APPLETS_BASE_FIND_OUTPUT_H
#define BX_APPLETS_BASE_FIND_OUTPUT_H

#include <stdbool.h>
#include <stdio.h>

struct walk_entry;

bool find_write_path_file(const char *progname, const char *filename,
                          const char *path, char terminator);
bool find_write_printf_format(FILE *fp, const char *format,
                              const struct walk_entry *entry);
bool find_write_ls_entry(FILE *fp, const struct walk_entry *entry);

#endif
