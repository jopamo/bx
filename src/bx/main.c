#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include "applets/base/which/which.h"
#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

typedef int (*applet_main_t)(int argc, char** argv);

struct applet {
    const char* name;
    applet_main_t main;
};

/*
 * Boot-critical dispatch order:
 * init is reserved as the first slot by design.
 * switch_root is intentionally kept as the first post-init applet.
 */
static const struct applet boot_critical_applets[] = {
    {"init", bx_init_main},
    {"switch_root", bx_switch_root_main},
};

static const struct applet applets[] = {
    {"ash", bx_ash_main},
    {"sh", bx_ash_main},
    {"bc", bx_bc_main},
    {"cat", bx_cat_main},
    {"cut", bx_cut_main},
    {"date", bx_date_main},
    {"which", bx_which_main},
    {"ln", bx_ln_main},
    {"link", bx_link_main},
    {"unlink", bx_unlink_main},
    {"readlink", bx_readlink_main},
    {"realpath", bx_realpath_main},
    {"basename", bx_basename_main},
    {"dirname", bx_dirname_main},
    {"dircolors", bx_dircolors_main},
    {"pathchk", bx_pathchk_main},
    {"ls", bx_ls_main},
    {"dir", bx_dir_main},
    {"vdir", bx_vdir_main},
    {"stat", bx_stat_main},
    {"df", bx_df_main},
    {"du", bx_du_main},
    {"sync", bx_sync_main},
    {"dd", bx_dd_main},
    {"printf", bx_printf_main},
    {"ed", bx_ed_main},
    {"env", bx_env_main},
    {"printenv", bx_printenv_main},
    {"pwd", bx_pwd_main},
    {"tty", bx_tty_main},
    {"stty", bx_stty_main},
    {"screen", bx_screen_main},
    {"getty", bx_getty_main},
    {"setsid", bx_setsid_main},
    {"dhcp", bx_dhcp_main},
    {"udhcpc", bx_dhcp_main},
    {"nice", bx_nice_main},
    {"nohup", bx_nohup_main},
    {"timeout", bx_timeout_main},
    {"chroot", bx_chroot_main},
    {"mount", bx_mount_main},
    {"umount", bx_umount_main},
    {"fuser", bx_fuser_main},
    {"kill", bx_kill_main},
    {"killall", bx_killall_main},
    {"peekfd", bx_peekfd_main},
    {"ps", bx_ps_main},
    {"pslog", bx_pslog_main},
    {"pstree", bx_pstree_main},
    {"prtstat", bx_prtstat_main},
    {"dmesg", bx_dmesg_main},
    {"reboot", bx_reboot_main},
    {"halt", bx_reboot_main},
    {"poweroff", bx_reboot_main},
    {"chmod", bx_chmod_main},
    {"chown", bx_chown_main},
    {"chgrp", bx_chgrp_main},
    {"rm", bx_rm_main},
    {"mkdir", bx_mkdir_main},
    {"rmdir", bx_rmdir_main},
    {"mkfifo", bx_mkfifo_main},
    {"mknod", bx_mknod_main},
    {"mktemp", bx_mktemp_main},
    {"touch", bx_touch_main},
    {"truncate", bx_truncate_main},
    {"shred", bx_shred_main},
    {"install", bx_install_main},
    {"cp", bx_cp_main},
    {"mv", bx_mv_main},
    {"nl", bx_nl_main},
    {"od", bx_od_main},
    {"paste", bx_paste_main},
    {"nproc", bx_nproc_main},
    {"numfmt", bx_numfmt_main},
    {"nc", bx_nc_main},
    {"netcat", bx_nc_main},
    {"wget", bx_wget_main},
    {"traceroute", bx_traceroute_main},
    {"ping", bx_ping_main},
    {"expr", bx_expr_main},
    {"fold", bx_fold_main},
    {"head", bx_head_main},
    {"hostid", bx_hostid_main},
    {"echo", bx_echo_main},
    {"expand", bx_expand_main},
    {"id", bx_id_main},
    {"logname", bx_logname_main},
    {"join", bx_join_main},
    {"comm", bx_comm_main},
    {"cksum", bx_cksum_main},
    {"md5sum", bx_md5sum_main},
    {"sha1sum", bx_sha1sum_main},
    {"base64", bx_base64_main},
    {"shuf", bx_shuf_main},
    {"yes", bx_yes_main},
    {"whoami", bx_whoami_main},
    {"sleep", bx_sleep_main},
    {"uname", bx_uname_main},
    {"wc", bx_wc_main},
    {"uniq", bx_uniq_main},
    {"unexpand", bx_unexpand_main},
    {"sum", bx_sum_main},
    {"tac", bx_tac_main},
    {"tail", bx_tail_main},
    {"tee", bx_tee_main},
    {"test", bx_test_main},
    {"[", bx_test_main},
    {"tr", bx_tr_main},
    {"seq", bx_seq_main},
    {"split", bx_split_main},
    {"sort", bx_sort_main},
    {"tar", bx_tar_main},
    {"cpio", bx_cpio_main},
    {"true", bx_true_main},
    {"false", bx_false_main},
    {"grep", bx_grep_main},
    {"egrep", bx_grep_main},
    {"fgrep", bx_grep_main},
    {"rg", bx_rg_main},
    {"bxgrep", bx_grep_main},
    {"bxrg", bx_rg_main},
    {"fd", bx_fd_main},
    {"find", bx_find_main},
    {"xargs", bx_xargs_main},
};

static const char shebang_applet_prefix[] = "--bx-applet-shebang=";

static const char* get_basename(const char* path) {
    const char* base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static applet_main_t find_applet(const char* name) {
    for (size_t i = 0; i < sizeof(boot_critical_applets) / sizeof(boot_critical_applets[0]); i++) {
        if (strcmp(boot_critical_applets[i].name, name) == 0) {
            return boot_critical_applets[i].main;
        }
    }

    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(applets[i].name, name) == 0) {
            return applets[i].main;
        }
    }
    return NULL;
}

static const char* get_shebang_applet(const char* arg) {
    size_t prefix_len = sizeof(shebang_applet_prefix) - 1;

    if (!arg || strncmp(arg, shebang_applet_prefix, prefix_len) != 0) {
        return NULL;
    }

    const char* name = arg + prefix_len;
    return (name[0] != '\0') ? name : NULL;
}

static int run_shebang_applet(applet_main_t applet_main, int argc, char** argv) {
    int applet_argc = argc - 2;
    char** applet_argv = xmalloc(((size_t)applet_argc + 1) * sizeof(*applet_argv));
    char* applet_argv0 = xstrdup(get_basename(argv[2]));

    applet_argv[0] = applet_argv0;
    for (int i = 1; i < applet_argc; i++) {
        applet_argv[i] = argv[i + 2];
    }
    applet_argv[applet_argc] = NULL;

    int rc = applet_main(applet_argc, applet_argv);
    free(applet_argv0);
    free(applet_argv);
    return rc;
}

static bool path_is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static char* resolve_self_path(const char* argv0) {
    char proc_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
    if (len >= 0) {
        proc_path[len] = '\0';
        return xstrdup(proc_path);
    }

    return xstrdup(argv0 != NULL ? argv0 : "bx");
}

static int install_one_applet_shortcut(const char* bx_path, const char* install_dir, const char* applet_name, bool symlink_mode) {
    size_t dir_len = strlen(install_dir);
    size_t applet_len = strlen(applet_name);
    size_t needs_slash = (dir_len > 0 && install_dir[dir_len - 1] == '/') ? 0 : 1;
    size_t path_len = dir_len + needs_slash + applet_len + 1;

    char* destination_path = xmalloc(path_len);
    memcpy(destination_path, install_dir, dir_len);
    if (needs_slash) {
        destination_path[dir_len] = '/';
    }
    memcpy(destination_path + dir_len + needs_slash, applet_name, applet_len);
    destination_path[path_len - 1] = '\0';

    struct stat st;
    if (lstat(destination_path, &st) == 0) {
        free(destination_path);
        return 0;
    }
    if (errno != ENOENT) {
        bx_perror(destination_path);
        free(destination_path);
        return 1;
    }

    int rc;
    if (symlink_mode) {
        rc = symlink(bx_path, destination_path);
    }
    else {
        rc = link(bx_path, destination_path);
    }

    if (rc != 0) {
        bx_perror(destination_path);
        free(destination_path);
        return 1;
    }

    free(destination_path);
    return 0;
}

static int install_missing_applets(const char* bx_path, const char* install_dir, bool symlink_mode) {
    if (!path_is_directory(install_dir)) {
        bx_err("install target is not a directory: %s", install_dir);
        return 1;
    }

    int status = 0;
    for (size_t i = 0; i < sizeof(boot_critical_applets) / sizeof(boot_critical_applets[0]); i++) {
        if (install_one_applet_shortcut(bx_path, install_dir, boot_critical_applets[i].name, symlink_mode) != 0) {
            status = 1;
        }
    }
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (install_one_applet_shortcut(bx_path, install_dir, applets[i].name, symlink_mode) != 0) {
            status = 1;
        }
    }

    return status;
}

static int run_install_mode(int argc, char** argv) {
    bool symlink_mode = false;
    const char* install_dir = ".";
    bool install_dir_set = false;

    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            printf("usage: bx --install [-s|--symlink] [DIR]\n");
            printf("Install missing applet shortcuts into DIR (default: .).\n");
            printf("By default hard links are created; -s creates symlinks.\n");
            return 0;
        }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--symlink") == 0) {
            symlink_mode = true;
            continue;
        }
        if (arg[0] == '-') {
            bx_err("unknown --install option: %s", arg);
            return 1;
        }
        if (install_dir_set) {
            bx_err("unexpected --install operand: %s", arg);
            return 1;
        }
        install_dir = arg;
        install_dir_set = true;
    }

    char* bx_path = resolve_self_path(argv[0]);
    int rc = install_missing_applets(bx_path, install_dir, symlink_mode);
    free(bx_path);
    return rc;
}

int main(int argc, char** argv) {
    if (argc < 1)
        return 1;

    const char* progname = get_basename(argv[0]);
    applet_main_t applet_main = find_applet(progname);
    if (!applet_main && progname[0] == '-' && progname[1] != '\0') {
        applet_main = find_applet(progname + 1);
    }

    if (applet_main) {
        return applet_main(argc, argv);
    }

    const char* shebang_applet = (argc >= 2) ? get_shebang_applet(argv[1]) : NULL;
    if (shebang_applet) {
        applet_main = find_applet(shebang_applet);
        if (!applet_main) {
            bx_err("unknown applet in shebang wrapper: %s", shebang_applet);
            return 1;
        }
        if (argc < 3) {
            bx_err("invalid shebang wrapper invocation");
            return 1;
        }
        return run_shebang_applet(applet_main, argc, argv);
    }

    if (argc < 2) {
        goto usage;
    }

    if (strcmp(argv[1], "--help") == 0) {
        goto usage;
    }

    if (strcmp(argv[1], "--install") == 0) {
        return run_install_mode(argc, argv);
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("bx version %s\n", BX_VERSION);
        return 0;
    }

    applet_main = find_applet(argv[1]);
    if (applet_main) {
        return applet_main(argc - 1, argv + 1);
    }

    bx_err("unknown subcommand: %s", argv[1]);
    return 1;

usage:
    printf("usage: bx SUBCOMMAND [options] ...\n");
    printf("       bx --install [-s|--symlink] [DIR]\n");
    printf("\n");
    printf("--install creates shortcuts only for applets missing in DIR.\n");
    printf("Use -s to create symlinks (default is hard links).\n");
    printf("\n");
    printf("Currently supported subcommands:\n");
    for (size_t i = 0; i < sizeof(boot_critical_applets) / sizeof(boot_critical_applets[0]); i++) {
        printf("  %s\n", boot_critical_applets[i].name);
    }
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        printf("  %s\n", applets[i].name);
    }
    return 0;
}
