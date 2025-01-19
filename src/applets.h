#ifndef BX_APPLETS_H
#define BX_APPLETS_H

int bx_true_main(int argc, char** argv);
int bx_false_main(int argc, char** argv);
int bx_ln_main(int argc, char** argv);
int bx_link_main(int argc, char** argv);
int bx_unlink_main(int argc, char** argv);
int bx_readlink_main(int argc, char** argv);
int bx_realpath_main(int argc, char** argv);
int bx_basename_main(int argc, char** argv);
int bx_dirname_main(int argc, char** argv);
int bx_pathchk_main(int argc, char** argv);
int bx_stat_main(int argc, char** argv);
int bx_df_main(int argc, char** argv);
int bx_du_main(int argc, char** argv);
int bx_sync_main(int argc, char** argv);
int bx_env_main(int argc, char** argv);
int bx_chmod_main(int argc, char** argv);
int bx_chown_main(int argc, char** argv);
int bx_chgrp_main(int argc, char** argv);
int bx_rm_main(int argc, char** argv);
int bx_mkdir_main(int argc, char** argv);
int bx_rmdir_main(int argc, char** argv);
int bx_mkfifo_main(int argc, char** argv);
int bx_mknod_main(int argc, char** argv);
int bx_mktemp_main(int argc, char** argv);
int bx_touch_main(int argc, char** argv);
int bx_truncate_main(int argc, char** argv);
int bx_shred_main(int argc, char** argv);
int bx_install_main(int argc, char** argv);
int bx_cp_main(int argc, char** argv);
int bx_mv_main(int argc, char** argv);
int bx_wget_main(int argc, char** argv);
int bx_expr_main(int argc, char** argv);
int bx_md5sum_main(int argc, char** argv);
int bx_base64_main(int argc, char** argv);

#endif /* BX_APPLETS_H */
