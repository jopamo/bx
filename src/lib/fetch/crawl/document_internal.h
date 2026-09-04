#ifndef BX_FETCH_DOCUMENT_INTERNAL_H
#define BX_FETCH_DOCUMENT_INTERNAL_H

/* Private boundary shared by extraction and derived-content conversion. */

#include "lib/fetch/document.h"
#include <stddef.h>
#include <sys/stat.h>

int bx_fetch_document_read(const char* path, unsigned char** data_out, size_t* length_out, struct stat* status_out, BxFetchDocumentOutcome* outcome);

#endif  // BX_FETCH_DOCUMENT_INTERNAL_H
