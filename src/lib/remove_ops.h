#ifndef BX_COMMON_REMOVE_OPS_H
#define BX_COMMON_REMOVE_OPS_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "bx/diag.h"

typedef void (*bx_remove_report_removed_fn)(const char* path, bool is_directory, void* user_data);

bool bx_remove_recursive(const char* path, struct bx_diag_ctx* diag);
/* Borrow an anchored parent and a single child name. Reopen saved locators
 * while descending; descriptor use does not grow with directory depth. */
bool bx_remove_recursive_at(int parent_fd, const char* name, const char* display_path,
                             const struct stat* expected, struct bx_diag_ctx* diag);
bool bx_remove_recursive_expected(const char* path, const struct stat* expected, struct bx_diag_ctx* diag);
bool bx_remove_recursive_one_file_system(const char* path, dev_t root_dev, struct bx_diag_ctx* diag);
bool bx_remove_recursive_one_file_system_expected(const char* path, const struct stat* expected, dev_t root_dev, struct bx_diag_ctx* diag);
bool bx_remove_recursive_report(const char* path, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data);
bool bx_remove_recursive_expected_report(const char* path, const struct stat* expected, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data);
bool bx_remove_recursive_one_file_system_report(const char* path, dev_t root_dev, struct bx_diag_ctx* diag, bx_remove_report_removed_fn report_removed, void* report_removed_user_data);
bool bx_remove_recursive_one_file_system_expected_report(const char* path,
                                                         const struct stat* expected,
                                                         dev_t root_dev,
                                                         struct bx_diag_ctx* diag,
                                                         bx_remove_report_removed_fn report_removed,
                                                         void* report_removed_user_data);

#endif /* BX_COMMON_REMOVE_OPS_H */
