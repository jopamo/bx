#ifndef BX_COMMON_SIGNAL_NAMES_H
#define BX_COMMON_SIGNAL_NAMES_H

#include <stdbool.h>
#include <stdio.h>

bool bx_signal_name_lookup(const char* name, int* number_out);
void bx_signal_name_list(FILE* stream);

#endif /* BX_COMMON_SIGNAL_NAMES_H */
