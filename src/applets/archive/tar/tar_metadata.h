#ifndef BX_TAR_METADATA_H
#define BX_TAR_METADATA_H

#include "applets/archive/archive_common.h"
#include "lib/file_metadata.h"

struct bx_tar_metadata_options {
    bool xattrs;
    bool acls;
    bool selinux;
    struct bx_archive_name_list include;
    struct bx_archive_name_list exclude;
};

/* Replace selected with an owned policy-filtered copy. The two models must
 * be distinct; neither the source nor options are retained. */
void bx_tar_metadata_select(struct bx_file_metadata* selected,
                             const struct bx_file_metadata* metadata,
                             const struct bx_tar_metadata_options* options);
bool bx_tar_metadata_collect(struct bx_file_metadata* metadata, int fd,
                              bool symlink, bool directory, bool numeric_ids,
                              const struct bx_tar_metadata_options* options);
bool bx_tar_metadata_parse(struct bx_file_metadata* metadata, const char* key,
                            const void* value, size_t len);
bool bx_tar_metadata_write(struct bx_archive_buffer* pax,
                            const struct bx_file_metadata* metadata);
bool bx_tar_metadata_present(const struct bx_file_metadata* metadata);
bool bx_tar_pax_append(struct bx_archive_buffer* pax, const char* key,
                       const void* value, size_t len);

#endif
