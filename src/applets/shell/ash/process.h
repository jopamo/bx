#ifndef BX_APPLETS_SHELL_ASH_PROCESS_H
#define BX_APPLETS_SHELL_ASH_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct ash_shell;

enum ash_process_role {
    ASH_PROCESS_EXTERNAL_COMMAND,
    ASH_PROCESS_PIPELINE_MEMBER,
    ASH_PROCESS_SUBSHELL,
    ASH_PROCESS_COMMAND_SUBSTITUTION,
    ASH_PROCESS_PROCESS_SUBSTITUTION,
    ASH_PROCESS_COPROCESS,
    ASH_PROCESS_ASYNC_COMMAND,
    ASH_PROCESS_APPLET_CHILD,
};

enum ash_child_state {
    ASH_CHILD_PREPARED,
    ASH_CHILD_RUNNING,
    ASH_CHILD_STOPPED,
    ASH_CHILD_EXITED,
    ASH_CHILD_SIGNALED,
};

/* Kernel-child identity and collected status, owned by one logical process. */
struct ash_child {
    enum ash_child_state state;
    pid_t pid;
    pid_t process_group;
    int wait_status;
};

/* A logical job member; its role exists independently of syntax nodes. */
struct ash_process {
    enum ash_process_role role;
    struct ash_child child;
};

enum ash_job_kind {
    ASH_JOB_FOREGROUND_COMMAND,
    ASH_JOB_PIPELINE,
    ASH_JOB_ASYNC,
    ASH_JOB_COMMAND_SUBSTITUTION,
    ASH_JOB_PROCESS_SUBSTITUTION,
    ASH_JOB_COPROCESS,
    ASH_JOB_APPLET_CHILD,
};

enum ash_job_state {
    ASH_JOB_PREPARING,
    ASH_JOB_RUNNING,
    ASH_JOB_STOPPED,
    ASH_JOB_COMPLETED,
};

enum ash_job_visibility {
    ASH_JOB_PRIVATE,
    ASH_JOB_PUBLISHED,
};

/*
 * Jobs are independent of syntax nodes. The shell context owns this graph;
 * ASTs and expanded commands may be borrowed by child callbacks but never
 * become lifecycle authority.
 *
 * PREPARING is private candidate state. Commit makes the job active only
 * after every process has a registered PID. PUBLISHED jobs are the only jobs
 * visible to wait/job-control lookup.
 */
struct ash_job {
    struct ash_job* next;
    struct ash_shell* owner;
    unsigned long id;
    enum ash_job_kind kind;
    enum ash_job_state state;
    enum ash_job_visibility visibility;
    bool foreground;
    pid_t process_group;
    struct ash_process* processes;
    size_t process_count;
    size_t process_capacity;
    size_t status_process;
};

typedef int (*ash_child_callback)(void* user_data);

struct ash_job* ash_job_create(
    struct ash_shell* shell,
    enum ash_job_kind kind,
    bool foreground
);
int ash_job_add_process(
    struct ash_job* job,
    enum ash_process_role role,
    size_t* process_index
);
int ash_job_register_process(
    struct ash_job* job,
    size_t process_index,
    pid_t pid,
    pid_t process_group
);
int ash_job_start_process(
    struct ash_job* job,
    enum ash_process_role role,
    ash_child_callback callback,
    void* user_data,
    size_t* process_index
);
bool ash_job_commit(
    struct ash_job* job,
    enum ash_job_visibility visibility
);
int ash_job_wait(struct ash_job* job, int* exit_status);
int ash_job_signal(const struct ash_job* job, int signal_number);
void ash_job_abort(struct ash_job* job);
bool ash_job_release(struct ash_job* job);

int ash_jobs_wait_pid(
    struct ash_shell* shell,
    pid_t pid,
    int* exit_status
);
int ash_jobs_wait_all(struct ash_shell* shell, int* exit_status);
bool ash_jobs_invariants(const struct ash_shell* shell);
void ash_jobs_destroy(struct ash_shell* shell);

bool ash_child_record_wait_status(
    struct ash_job* job,
    size_t process_index,
    int wait_status
);
int ash_child_exit_status(const struct ash_child* child);
pid_t ash_job_last_pid(const struct ash_job* job);

/*
 * A fork child does not own the parent's job records. Detach inherited
 * lifecycle truth before nested execution creates child-local jobs.
 */
void ash_jobs_detach_after_fork(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_PROCESS_H */
