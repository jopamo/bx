#define _GNU_SOURCE
#include "lib/fetch/metadata.h"
#include "lib/fetch/secure_path.h"
#include "fswalk/walk.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    BxFetchMetadataMappingVisitor visitor;
    void* userdata;
    int error_number;
} MetadataRecovery;

static bool path_has_suffix(const char* path, const char* suffix) {
    if (!path || !suffix)
        return false;
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    return path_length >= suffix_length && strcmp(path + path_length - suffix_length, suffix) == 0;
}

static char* metadata_recovery_root(const struct bx_fetch_config* cfg) {
    if (cfg->dirs.directory_prefix && cfg->dirs.directory_prefix[0] != '\0')
        return strdup(cfg->dirs.directory_prefix);

    const char* output_document = cfg->download.output_document;
    if (!output_document || output_document[0] == '\0')
        return strdup(".");
    const char* slash = strrchr(output_document, '/');
    if (!slash)
        return strdup(".");
    if (slash == output_document)
        return strdup("/");
    return strndup(output_document, (size_t)(slash - output_document));
}

static enum bx_walk_action metadata_recovery_fail(MetadataRecovery* recovery, int error_number) {
    recovery->error_number = error_number ? error_number : EIO;
    errno = recovery->error_number;
    return BX_WALK_ERROR;
}

static enum bx_walk_action visit_metadata_sidecar(struct bx_walk_entry* entry, void* userdata) {
    MetadataRecovery* recovery = userdata;
    static const char suffix[] = ".mira.meta";
    if (!recovery) {
        errno = EINVAL;
        return BX_WALK_ERROR;
    }
    if (!entry)
        return metadata_recovery_fail(recovery, EINVAL);
    if (entry->is_dir || entry->is_symlink || !path_has_suffix(entry->path, suffix))
        return BX_WALK_CONTINUE;
    if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_FILTER))
        return metadata_recovery_fail(recovery, errno);
    if (!S_ISREG(entry->mode) || entry->nlink > 1)
        return metadata_recovery_fail(recovery, EINVAL);

    size_t output_length = strlen(entry->path) - (sizeof(suffix) - 1u);
    char* output_path = strndup(entry->path, output_length);
    if (!output_path)
        return metadata_recovery_fail(recovery, errno);

    int output_fd = bx_fetch_secure_path_open_existing_file(output_path);
    if (output_fd == -1) {
        int error_number = errno;
        free(output_path);
        if (error_number == ENOENT)
            return BX_WALK_CONTINUE;
        return metadata_recovery_fail(recovery, error_number);
    }
    if (close(output_fd) != 0) {
        int error_number = errno;
        free(output_path);
        return metadata_recovery_fail(recovery, error_number);
    }

    BxFetchMetadata metadata = {0};
    if (bx_fetch_metadata_load(output_path, &metadata) != 0) {
        int error_number = errno;
        free(output_path);
        return metadata_recovery_fail(recovery, error_number);
    }
    const char* local_path = metadata.local_path && metadata.local_path[0] != '\0' ? metadata.local_path : output_path;
    if (local_path == output_path && local_path[0] == '.' && local_path[1] == '/')
        local_path += 2;

    int rc = 0;
    if (metadata.origin_url && metadata.origin_url[0] != '\0')
        rc = recovery->visitor(recovery->userdata, metadata.origin_url, local_path);
    if (rc == 0 && metadata.redirect_target && metadata.redirect_target[0] != '\0')
        rc = recovery->visitor(recovery->userdata, metadata.redirect_target, local_path);

    int error_number = errno;
    bx_fetch_metadata_clear(&metadata);
    free(output_path);
    if (rc != 0)
        return metadata_recovery_fail(recovery, error_number);
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action visit_metadata_error(const char* path, int error_number, void* userdata) {
    (void)path;
    return metadata_recovery_fail(userdata, error_number);
}

int bx_fetch_metadata_visit_recovery_mappings(const struct bx_fetch_config* cfg, BxFetchMetadataMappingVisitor visitor, void* userdata) {
    if (!cfg || !visitor) {
        errno = EINVAL;
        return -1;
    }

    char* root = metadata_recovery_root(cfg);
    if (!root)
        return -1;
    int root_fd = bx_fetch_secure_path_open_existing_directory(root);
    if (root_fd == -1) {
        int error_number = errno;
        free(root);
        if (error_number == ENOENT)
            return 0;
        errno = error_number;
        return -1;
    }

    DIR* root_dir = fdopendir(root_fd);
    if (!root_dir) {
        int error_number = errno;
        close(root_fd);
        free(root);
        errno = error_number;
        return -1;
    }

    MetadataRecovery recovery = {
        .visitor = visitor,
        .userdata = userdata,
    };
    const struct bx_walk_opts options = {
        .follow_symlinks = false,
        .follow_root_symlink = false,
        .stay_on_filesystem = true,
        .suppress_errors = true,
        .max_depth = -1,
        .cycle_mode = BX_WALK_CYCLE_DIR_REPEAT,
        .cycle_report = BX_WALK_CYCLE_ERROR,
    };
    const struct bx_walk_ops operations = {
        .visit = visit_metadata_sidecar,
        .error = visit_metadata_error,
    };
    int rc = bx_walk_opened_dir(root, root_dir, &options, &operations, &recovery);
    int error_number = recovery.error_number ? recovery.error_number : errno;
    free(root);
    if (rc != 0)
        errno = error_number ? error_number : EIO;
    return rc;
}
