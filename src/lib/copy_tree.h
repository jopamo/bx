#ifndef BX_COMMON_COPY_TREE_H
#define BX_COMMON_COPY_TREE_H

#include <stdbool.h>
#include <sys/stat.h>
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/backup_ops.h"
#include "lib/copy_data.h"

enum bx_deref_mode {
    BX_DEREF_DEFAULT = 0,
    BX_DEREF_ALWAYS,
    BX_DEREF_NEVER,
    BX_DEREF_COMMAND_LINE,
};

enum bx_mode_policy {
    BX_MODE_POLICY_DEFAULT = 0,
    BX_MODE_POLICY_PRESERVE,
    BX_MODE_POLICY_NO_PRESERVE,
};

struct bx_copy_options {
    bool recursive;
    bool attributes_only;
    bool copy_contents;
    bool interactive;
    bool force;
    bool no_clobber;
    bool remove_destination;
    bool hard_link;
    bool symbolic_link;
    bool parents;
    bool one_file_system;
    bool verbose;
    bool debug;
    bool move_mode;

    enum bx_deref_mode deref_mode;
    enum bx_sparse_mode sparse_mode;
    enum bx_reflink_mode reflink_mode;
    enum bx_mode_policy mode_policy;
    enum bx_update_mode update_mode;

    unsigned preserve_mask;
};

struct bx_link_entry {
    dev_t dev;
    ino_t ino;
    char* dest_path;
    struct bx_link_entry* next;
};

struct bx_dir_entry {
    dev_t dev;
    ino_t ino;
    struct bx_dir_entry* next;
};

struct bx_parent_attr_entry {
    char* src_path;
    char* dest_path;
    struct stat src_stat;
    struct bx_parent_attr_entry* next;
};

struct bx_copy_context {
    const struct bx_copy_options* options;
    struct bx_diag_ctx* diag;
    struct bx_backup_params backup_params;
    mode_t umask_value;

    struct bx_link_entry* links;
    struct bx_dir_entry* source_dirs;
    struct bx_parent_attr_entry* parent_attrs;

    bool dest_root_active;
    dev_t dest_root_dev;
    ino_t dest_root_ino;
    dev_t source_root_dev;

    const char* target_root;

    /* Internal state */
    bool stop_current_source;
    const char* current_source_root;
    const char* current_dest_root;
    char* current_dest_root_realpath;
};

bool bx_copy_path(struct bx_copy_context* ctx, const char* src_path, const char* source_operand, const char* dest_path, bool top_level);

void bx_copy_free_links(struct bx_copy_context* ctx);
void bx_copy_free_source_dirs(struct bx_copy_context* ctx);
void bx_copy_free_parent_attrs(struct bx_copy_context* ctx);

#endif /* BX_COMMON_COPY_TREE_H */
