#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "which.h"
#include "applets.h"
#include "diag.h"
#include "libbx.h"

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
    {"env", bx_env_main},
    {"printenv", bx_printenv_main},
    {"pwd", bx_pwd_main},
    {"tty", bx_tty_main},
    {"stty", bx_stty_main},
    {"getty", bx_getty_main},
    {"nice", bx_nice_main},
    {"nohup", bx_nohup_main},
    {"timeout", bx_timeout_main},
    {"chroot", bx_chroot_main},
    {"mount", bx_mount_main},
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
    {"true", bx_true_main},
    {"false", bx_false_main},
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
