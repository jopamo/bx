#ifndef BX_APPLETS_SYSTEM_PSMISC_PROCFS_H
#define BX_APPLETS_SYSTEM_PSMISC_PROCFS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BX_PROC_READ_CMDLINE 0x01u
#define BX_PROC_READ_EXE 0x02u

struct bx_proc_stat {
    pid_t pid;
    char* comm;
    char state;
    pid_t ppid;
    pid_t pgrp;
    pid_t session;
    long tty_nr;
    long tpgid;
    unsigned long flags;
    unsigned long long utime_ticks;
    unsigned long long stime_ticks;
    long priority;
    long nice;
    long num_threads;
    unsigned long long starttime_ticks;
    unsigned long long vsize_bytes;
    long long rss_pages;
};

struct bx_proc_info {
    pid_t pid;
    pid_t ppid;
    pid_t pgrp;
    pid_t session;
    uid_t uid;
    char state;
    unsigned long long starttime_ticks;
    char* comm;
    char* cmdline;
    char* exe;
};

struct bx_proc_list {
    struct bx_proc_info* items;
    size_t len;
    size_t cap;
};

struct bx_proc_fd_entry {
    int fd;
    char* target;
    struct stat st;
    bool have_stat;
    unsigned long flags;
    bool have_flags;
    off_t position;
    bool have_position;
};

struct bx_proc_fd_list {
    struct bx_proc_fd_entry* items;
    size_t len;
    size_t cap;
};

bool bx_proc_parse_pid_arg(const char* text, pid_t* pid_out);

bool bx_proc_read_text_file(pid_t pid, const char* leaf, char** text_out, bool* vanished_out);
bool bx_proc_readlink_leaf(pid_t pid, const char* leaf, char** target_out, bool* vanished_out);
bool bx_proc_read_stat(pid_t pid, struct bx_proc_stat* stat_out, bool* vanished_out);
bool bx_proc_read_uid(pid_t pid, uid_t* uid_out, bool* vanished_out);
bool bx_proc_read_gid(pid_t pid, gid_t* gid_out, bool* vanished_out);
bool bx_proc_read_cmdline(pid_t pid, char** cmdline_out, bool* vanished_out);
bool bx_proc_read_exe(pid_t pid, char** exe_out, bool* vanished_out);
/* Verify the procfs IDs are in the caller's signaling namespace. */
bool bx_proc_signal_namespace_matches(void);
/* Use only after verifying the namespace above. */
bool bx_proc_read_ns_pid(pid_t pid, pid_t* ns_pid_out, bool* vanished_out);
bool bx_proc_read_ns_pgid(pid_t pid, pid_t* ns_pgid_out, bool* vanished_out);
/* Recheck a process snapshot immediately before delivery; pgrp=0 ignores PGID. */
bool bx_proc_signal_target_matches(pid_t pid, unsigned long long starttime_ticks, pid_t pgrp);
/* Caller owns the returned pidfd. Refuse delivery if the snapshot changed. */
int bx_proc_pin_signal_target(pid_t pid, unsigned long long starttime_ticks);
bool bx_proc_read_info(pid_t pid, unsigned flags, struct bx_proc_info* info_out, bool* vanished_out);
bool bx_proc_list_read(struct bx_proc_list* list, unsigned flags);
bool bx_proc_read_fds(pid_t pid, struct bx_proc_fd_list* list, bool* vanished_out);
bool bx_proc_uptime_seconds(double* uptime_out);
long bx_proc_clock_ticks_per_second(void);
char* bx_proc_basename_dup(const char* path);
pid_t bx_proc_self_host_pid(void);

void bx_proc_stat_free(struct bx_proc_stat* stat_info);
void bx_proc_info_free(struct bx_proc_info* info);
void bx_proc_list_free(struct bx_proc_list* list);
void bx_proc_fd_list_free(struct bx_proc_fd_list* list);

#endif /* BX_APPLETS_SYSTEM_PSMISC_PROCFS_H */
