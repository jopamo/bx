#ifndef BX_FETCH_LINK_CONVERSION_H
#define BX_FETCH_LINK_CONVERSION_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core, applet */

/*
 * Cold-path derived-content publication. Conversion resolves links against the
 * authoritative committed URL map, stages replacements privately, and pins
 * the final replacement to the source inode read for parsing.
 */

#include "config.h"
#include "document.h"
#include "publication.h"

typedef enum {
    BX_FETCH_LINK_CONVERSION_SKIPPED = 0,
    BX_FETCH_LINK_CONVERSION_UNCHANGED,
    BX_FETCH_LINK_CONVERSION_COMMITTED,
} BxFetchLinkConversionStatus;

typedef enum {
    BX_FETCH_LINK_CONVERSION_FAILURE_NONE = 0,
    BX_FETCH_LINK_CONVERSION_FAILURE_INVALID_ARGUMENT,
    BX_FETCH_LINK_CONVERSION_FAILURE_BASE_URL,
    BX_FETCH_LINK_CONVERSION_FAILURE_DOCUMENT,
    BX_FETCH_LINK_CONVERSION_FAILURE_REWRITE,
    BX_FETCH_LINK_CONVERSION_FAILURE_OUTPUT_STAGE,
    BX_FETCH_LINK_CONVERSION_FAILURE_BACKUP_STAGE,
    BX_FETCH_LINK_CONVERSION_FAILURE_BACKUP_COMMIT,
    BX_FETCH_LINK_CONVERSION_FAILURE_OUTPUT_COMMIT,
} BxFetchLinkConversionFailure;

typedef struct {
    BxFetchLinkConversionStatus status;
    BxFetchLinkConversionFailure failure;
    BxFetchDocumentOutcome document;
    int error_number;
} BxFetchLinkConversionOutcome;

/*
 * A requested .orig backup is committed before the converted payload while
 * the payload candidate remains private. If the later payload commit fails,
 * the prior payload remains authoritative and the completed backup may remain.
 */
int bx_fetch_document_convert_download(const struct bx_fetch_config* cfg, const BxFetchPublicationState* publication, const BxFetchDownloadedFileView* download, BxFetchLinkConversionOutcome* outcome);

#endif  // BX_FETCH_LINK_CONVERSION_H
