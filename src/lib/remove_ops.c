#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "remove_ops.h"
#include "lib/dir_cycle.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "lib/same_file.h"
#include "bx/diag.h"

static void bx_remove_diag_changed(const char* path, struct bx_diag_ctx* diag) {
    bx_diag(diag, "refusing to remove '%s': path changed during recursive removal", path);
}

static void bx_remove_diag_cycle(const char* path, struct bx_diag_ctx* diag) {
    bx_diag(diag, "refusing to remove '%s': directory cycle detected during recursive removal", path);
}

static bool bx_remove_stat_matches_expected(const char* path, const struct stat* expected, const struct stat* actual, struct bx_diag_ctx* diag) {
    if (expected == NULL || bx_same_file(expected, actual)) {
        return true;
    }
    bx_remove_diag_changed(path, diag);
    return false;
}

static bool bx_remove_open_error_is_changed_path(const char* path, const struct stat* expected) {
    struct stat current;

    if (expected == NULL) {
        return false;
    }
    if (lstat(path, &current) != 0) {
        return false;
    }
    return !bx_same_file(expected, &current);
}

static int bx_remove_open_dir_path(const char* path, const struct stat* expected, struct stat* opened_st, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_nofollow_cloexec(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        if (bx_remove_open_error_is_changed_path(path, expected)) {
            bx_remove_diag_changed(path, diag);
        }
        else {
            bx_perror_path(diag, path);
        }
        return -1;
    }

    if (fstat(fd, opened_st) != 0) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return -1;
    }
    if (!S_ISDIR(opened_st->st_mode)) {
        errno = ENOTDIR;
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return -1;
    }
    if (!bx_remove_stat_matches_expected(path, expected, opened_st, diag)) {
        bx_fd_cleanup(&fd);
        return -1;
    }
    return fd;
}

static int bx_remove_open_dir_child(int parent_fd, const char* name, const char* path, const struct stat* expected, struct stat* opened_st, struct bx_diag_ctx* diag) {
    int fd = bx_fd_openat_child_nofollow(parent_fd, name, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        struct stat current;
        if (expected != NULL && bx_fd_fstatat_child_nofollow(parent_fd, name, &current) == 0 && !bx_same_file(expected, &current)) {
            bx_remove_diag_changed(path, diag);
        }
        else {
            bx_perror_path(diag, path);
        }
        return -1;
    }

    if (fstat(fd, opened_st) != 0) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return -1;
    }
    if (!S_ISDIR(opened_st->st_mode)) {
        errno = ENOTDIR;
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return -1;
    }
    if (!bx_remove_stat_matches_expected(path, expected, opened_st, diag)) {
        bx_fd_cleanup(&fd);
        return -1;
    }
    return fd;
}

static bool bx_remove_verify_path_entry(const char* path, const struct stat* expected, struct bx_diag_ctx* diag) {
    struct stat current;

    if (lstat(path, &current) != 0) {
        bx_perror_path(diag, path);
        return false;
    }
    return bx_remove_stat_matches_expected(path, expected, &current, diag);
}

static bool bx_remove_verify_child_entry(int parent_fd, const char* name, const char* path, const struct stat* expected, struct bx_diag_ctx* diag) {
    struct stat current;

    if (bx_fd_fstatat_child_nofollow(parent_fd, name, &current) != 0) {
        bx_perror_path(diag, path);
        return false;
    }
    return bx_remove_stat_matches_expected(path, expected, &current, diag);
}

static bool bx_remove_recursive_opened_dir(int fd,
                                           const struct stat* opened_st,
                                           const char* path,
                                           int parent_fd,
                                           const char* name_in_parent,
                                           bool one_file_system,
                                           dev_t root_dev,
                                           bool top_level,
                                           struct bx_dir_stack* ancestor_stack,
                                           struct bx_diag_ctx* diag,
                                           bx_remove_report_removed_fn report_removed,
                                           void* report_removed_user_data);

static bool bx_remove_recursive_child(int parent_fd,
                                      const char* name,
                                      const char* path,
                                      const struct stat* expected,
                                      bool one_file_system,
                                      dev_t root_dev,
                                      struct bx_dir_stack* ancestor_stack,
                                      struct bx_diag_ctx* diag,
                                      bx_remove_report_removed_fn report_removed,
                                      void* report_removed_user_data) {
    struct stat st;

    if (bx_fd_fstatat_child_nofollow(parent_fd, name, &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }
    if (!bx_remove_stat_matches_expected(path, expected, &st, diag)) {
        return false;
    }

    if (one_file_system && S_ISDIR(st.st_mode) && st.st_dev != root_dev) {
        return true;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (!bx_remove_verify_child_entry(parent_fd, name, path, &st, diag)) {
            return false;
        }
        if (bx_fd_unlinkat_child(parent_fd, name, 0) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
        if (report_removed != NULL) {
            report_removed(path, false, report_removed_user_data);
        }
        return true;
    }

    if (bx_dir_stack_contains(ancestor_stack, &st)) {
        bx_remove_diag_cycle(path, diag);
        return false;
    }

    struct stat opened_st;
    int child_fd = bx_remove_open_dir_child(parent_fd, name, path, &st, &opened_st, diag);
    if (child_fd < 0) {
        return false;
    }
    return bx_remove_recursive_opened_dir(child_fd, &opened_st, path, parent_fd, name, one_file_system, root_dev, false, ancestor_stack, diag, report_removed, report_removed_user_data);
}

static bool bx_remove_recursive_opened_dir(int fd,
                                           const struct stat* opened_st,
                                           const char* path,
                                           int parent_fd,
                                           const char* name_in_parent,
                                           bool one_file_system,
                                           dev_t root_dev,
                                           bool top_level,
                                           struct bx_dir_stack* ancestor_stack,
                                           struct bx_diag_ctx* diag,
                                           bx_remove_report_removed_fn report_removed,
                                           void* report_removed_user_data) {
    DIR* dir = fdopendir(fd);
    if (dir == NULL) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return false;
    }

    int dir_fd = dirfd(dir);
    bool ok = true;
    if (dir_fd < 0) {
        bx_perror_path(diag, path);
        ok = false;
    }

    struct bx_dir_stack stack_entry = {
        .dev = opened_st->st_dev,
        .ino = opened_st->st_ino,
        .parent = ancestor_stack,
    };

    if (dir_fd >= 0) {
        for (;;) {
            errno = 0;
            struct dirent* entry = readdir(dir);
            if (entry == NULL) {
                if (errno != 0) {
                    bx_perror_path(diag, path);
                    ok = false;
                }
                break;
            }
            if (bx_path_is_dot_or_dotdot(entry->d_name)) {
                continue;
            }

            char* child_path = bx_path_join(path, entry->d_name);
            if (!bx_remove_recursive_child(dir_fd, entry->d_name, child_path, NULL, one_file_system, root_dev, &stack_entry, diag, report_removed, report_removed_user_data)) {
                ok = false;
            }
            free(child_path);
        }
    }

    if (closedir(dir) != 0) {
        bx_perror_path(diag, path);
        ok = false;
    }

    if (!ok) {
        return false;
    }

    if (top_level) {
        if (!bx_remove_verify_path_entry(path, opened_st, diag)) {
            return false;
        }
        if (rmdir(path) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
    }
    else {
        if (!bx_remove_verify_child_entry(parent_fd, name_in_parent, path, opened_st, diag)) {
            return false;
        }
        if (bx_fd_unlinkat_child(parent_fd, name_in_parent, AT_REMOVEDIR) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
    }

    if (report_removed != NULL) {
        report_removed(path, true, report_removed_user_data);
    }
    return true;
}

static bool
bx_remove_recursive_impl(const char* path, const struct stat* expected, bool one_file_system, dev_t root_dev, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT && expected == NULL) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }
    if (!bx_remove_stat_matches_expected(path, expected, &st, diag)) {
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
        if (report_removed != NULL) {
            report_removed(path, false, report_removed_user_data);
        }
        return true;
    }

    struct stat opened_st;
    int fd = bx_remove_open_dir_path(path, &st, &opened_st, diag);
    if (fd < 0) {
        return false;
    }
    return bx_remove_recursive_opened_dir(fd, &opened_st, path, AT_FDCWD, NULL, one_file_system, root_dev, true, NULL, diag, report_removed, report_removed_user_data);
}

bool bx_remove_recursive(const char* path, struct bx_diag_ctx* diag) {
    return bx_remove_recursive_impl(path, NULL, false, 0, diag, NULL, NULL);
}

bool bx_remove_recursive_expected(const char* path, const struct stat* expected, struct bx_diag_ctx* diag) {
    return bx_remove_recursive_impl(path, expected, false, 0, diag, NULL, NULL);
}

bool bx_remove_recursive_one_file_system(const char* path, dev_t root_dev, struct bx_diag_ctx* diag) {
    return bx_remove_recursive_impl(path, NULL, true, root_dev, diag, NULL, NULL);
}

bool bx_remove_recursive_one_file_system_expected(const char* path, const struct stat* expected, dev_t root_dev, struct bx_diag_ctx* diag) {
    return bx_remove_recursive_impl(path, expected, true, root_dev, diag, NULL, NULL);
}

bool bx_remove_recursive_report(const char* path, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data) {
    return bx_remove_recursive_impl(path, NULL, false, 0, diag, report_removed, report_removed_user_data);
}

bool bx_remove_recursive_expected_report(const char* path, const struct stat* expected, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data) {
    return bx_remove_recursive_impl(path, expected, false, 0, diag, report_removed, report_removed_user_data);
}

bool bx_remove_recursive_one_file_system_report(const char* path, dev_t root_dev, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data) {
    return bx_remove_recursive_impl(path, NULL, true, root_dev, diag, report_removed, report_removed_user_data);
}

bool bx_remove_recursive_one_file_system_expected_report(const char* path,
                                                         const struct stat* expected,
                                                         dev_t root_dev,
                                                         struct bx_diag_ctx* diag,
                                                         bx_remove_report_removed_fn report_removed,
                                                         void* report_removed_user_data) {
    return bx_remove_recursive_impl(path, expected, true, root_dev, diag, report_removed, report_removed_user_data);
}
