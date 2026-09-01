#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "applets/shell/ash/process.h"
#include "applets/shell/ash/shell_context.h"
#include "lib/child_runner.h"

static bool ash_process_role_valid(enum ash_process_role role) {
    return role >= ASH_PROCESS_EXTERNAL_COMMAND &&
        role <= ASH_PROCESS_APPLET_CHILD;
}

static bool ash_job_kind_valid(enum ash_job_kind kind) {
    return kind >= ASH_JOB_FOREGROUND_COMMAND &&
        kind <= ASH_JOB_APPLET_CHILD;
}

static bool ash_job_foreground_valid(
    enum ash_job_kind kind,
    bool foreground
) {
    if (kind == ASH_JOB_ASYNC ||
        kind == ASH_JOB_PROCESS_SUBSTITUTION ||
        kind == ASH_JOB_COPROCESS) {
        return !foreground;
    }
    if (kind == ASH_JOB_FOREGROUND_COMMAND ||
        kind == ASH_JOB_PIPELINE ||
        kind == ASH_JOB_COMMAND_SUBSTITUTION) {
        return foreground;
    }
    return true;
}

static bool ash_job_role_valid(
    enum ash_job_kind kind,
    enum ash_process_role role
) {
    switch (kind) {
        case ASH_JOB_FOREGROUND_COMMAND:
            return role == ASH_PROCESS_EXTERNAL_COMMAND ||
                role == ASH_PROCESS_SUBSHELL;
        case ASH_JOB_PIPELINE:
            return role == ASH_PROCESS_PIPELINE_MEMBER;
        case ASH_JOB_ASYNC:
            return role == ASH_PROCESS_ASYNC_COMMAND;
        case ASH_JOB_COMMAND_SUBSTITUTION:
            return role == ASH_PROCESS_COMMAND_SUBSTITUTION;
        case ASH_JOB_PROCESS_SUBSTITUTION:
            return role == ASH_PROCESS_PROCESS_SUBSTITUTION;
        case ASH_JOB_COPROCESS:
            return role == ASH_PROCESS_COPROCESS;
        case ASH_JOB_APPLET_CHILD:
            return role == ASH_PROCESS_APPLET_CHILD;
    }
    return false;
}

static bool ash_process_terminal(const struct ash_process* process) {
    return process->child.state == ASH_CHILD_EXITED ||
        process->child.state == ASH_CHILD_SIGNALED;
}

static bool ash_child_invariants(const struct ash_child* child) {
    if (child == NULL || child->state < ASH_CHILD_PREPARED ||
        child->state > ASH_CHILD_SIGNALED ||
        child->process_group < 0) {
        return false;
    }
    switch (child->state) {
        case ASH_CHILD_PREPARED:
            return child->pid == 0 && child->process_group == 0 &&
                child->wait_status == 0;
        case ASH_CHILD_RUNNING:
            return child->pid > 0;
        case ASH_CHILD_STOPPED:
            return child->pid > 0 && WIFSTOPPED(child->wait_status);
        case ASH_CHILD_EXITED:
            return child->pid > 0 && WIFEXITED(child->wait_status);
        case ASH_CHILD_SIGNALED:
            return child->pid > 0 && WIFSIGNALED(child->wait_status);
    }
    return false;
}

static bool ash_job_processes_invariants(const struct ash_job* job) {
    if (job->process_count > job->process_capacity ||
        (job->process_capacity == 0u) != (job->processes == NULL) ||
        (job->process_count == 0u && job->status_process != 0u) ||
        (job->process_count != 0u &&
         job->status_process >= job->process_count)) {
        return false;
    }

    bool running = false;
    bool stopped = false;
    for (size_t i = 0u; i < job->process_count; i++) {
        const struct ash_process* process = &job->processes[i];
        if (!ash_process_role_valid(process->role) ||
            !ash_job_role_valid(job->kind, process->role) ||
            !ash_child_invariants(&process->child) ||
            (job->process_group > 0 &&
             process->child.process_group > 0 &&
             process->child.process_group != job->process_group)) {
            return false;
        }
        running |= process->child.state == ASH_CHILD_RUNNING;
        stopped |= process->child.state == ASH_CHILD_STOPPED;
    }

    if (job->state == ASH_JOB_PREPARING) {
        return true;
    }
    if (job->process_count == 0u) {
        return false;
    }
    for (size_t i = 0u; i < job->process_count; i++) {
        if (job->processes[i].child.state == ASH_CHILD_PREPARED) {
            return false;
        }
    }
    enum ash_job_state derived = running ?
        ASH_JOB_RUNNING :
        (stopped ? ASH_JOB_STOPPED : ASH_JOB_COMPLETED);
    return job->state == derived;
}

static bool ash_job_list_acyclic(const struct ash_job* jobs) {
    const struct ash_job* slow = jobs;
    const struct ash_job* fast = jobs;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

bool ash_jobs_invariants(const struct ash_shell* shell) {
    if (shell == NULL || !ash_job_list_acyclic(shell->jobs)) {
        return false;
    }

    for (const struct ash_job* job = shell->jobs;
         job != NULL;
         job = job->next) {
        if (job->owner != shell ||
            !ash_job_kind_valid(job->kind) ||
            !ash_job_foreground_valid(job->kind, job->foreground) ||
            job->state < ASH_JOB_PREPARING ||
            job->state > ASH_JOB_COMPLETED ||
            (job->visibility != ASH_JOB_PRIVATE &&
             job->visibility != ASH_JOB_PUBLISHED) ||
            job->process_group < 0 ||
            (job->visibility == ASH_JOB_PRIVATE && job->id != 0u) ||
            (job->visibility == ASH_JOB_PUBLISHED &&
             (job->id == 0u || job->foreground ||
              job->state == ASH_JOB_PREPARING)) ||
            (job->state == ASH_JOB_PREPARING &&
             job->visibility != ASH_JOB_PRIVATE) ||
            !ash_job_processes_invariants(job)) {
            return false;
        }

        for (const struct ash_job* duplicate = job->next;
             duplicate != NULL;
             duplicate = duplicate->next) {
            if (job->id != 0u && job->id == duplicate->id) {
                return false;
            }
        }
        for (size_t i = 0u; i < job->process_count; i++) {
            const struct ash_child* child = &job->processes[i].child;
            if (child->state != ASH_CHILD_RUNNING &&
                child->state != ASH_CHILD_STOPPED) {
                continue;
            }
            for (const struct ash_job* other_job = shell->jobs;
                 other_job != NULL;
                 other_job = other_job->next) {
                for (size_t j = 0u;
                     j < other_job->process_count;
                     j++) {
                    const struct ash_child* other =
                        &other_job->processes[j].child;
                    if (other == child ||
                        (other->state != ASH_CHILD_RUNNING &&
                         other->state != ASH_CHILD_STOPPED)) {
                        continue;
                    }
                    if (other->pid == child->pid) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool ash_job_owned(const struct ash_job* job) {
    if (job == NULL || job->owner == NULL) {
        return false;
    }
    for (const struct ash_job* current = job->owner->jobs;
         current != NULL;
         current = current->next) {
        if (current == job) {
            return true;
        }
    }
    return false;
}

static bool ash_pid_is_active(
    const struct ash_shell* shell,
    pid_t pid,
    const struct ash_process* except
) {
    for (const struct ash_job* job = shell->jobs;
         job != NULL;
         job = job->next) {
        for (size_t i = 0u; i < job->process_count; i++) {
            const struct ash_process* process = &job->processes[i];
            if (process != except && process->child.pid == pid &&
                (process->child.state == ASH_CHILD_RUNNING ||
                 process->child.state == ASH_CHILD_STOPPED)) {
                return true;
            }
        }
    }
    return false;
}

static unsigned long ash_job_allocate_id(struct ash_shell* shell) {
    for (unsigned long attempts = 0u; attempts < ULONG_MAX; attempts++) {
        unsigned long candidate = shell->next_job_id++;
        if (candidate == 0u) {
            continue;
        }
        bool used = false;
        for (const struct ash_job* job = shell->jobs;
             job != NULL;
             job = job->next) {
            if (job->id == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            return candidate;
        }
    }
    errno = EOVERFLOW;
    return 0u;
}

struct ash_job* ash_job_create(
    struct ash_shell* shell,
    enum ash_job_kind kind,
    bool foreground
) {
    if (shell == NULL || !ash_job_kind_valid(kind) ||
        !ash_job_foreground_valid(kind, foreground)) {
        errno = EINVAL;
        return NULL;
    }
    assert(ash_jobs_invariants(shell));
    struct ash_job* job = calloc(1u, sizeof(*job));
    if (job == NULL) {
        return NULL;
    }
    *job = (struct ash_job){
        .next = shell->jobs,
        .owner = shell,
        .kind = kind,
        .state = ASH_JOB_PREPARING,
        .visibility = ASH_JOB_PRIVATE,
        .foreground = foreground,
    };
    shell->jobs = job;
    assert(ash_jobs_invariants(shell));
    return job;
}

int ash_job_add_process(
    struct ash_job* job,
    enum ash_process_role role,
    size_t* process_index
) {
    if (!ash_job_owned(job) || job->state != ASH_JOB_PREPARING ||
        !ash_process_role_valid(role) || process_index == NULL ||
        !ash_job_role_valid(job->kind, role)) {
        errno = EINVAL;
        return -1;
    }
    if (job->process_count == SIZE_MAX) {
        errno = ENOMEM;
        return -1;
    }
    size_t needed = job->process_count + 1u;
    if (needed > job->process_capacity) {
        size_t capacity = job->process_capacity == 0u ?
            4u : job->process_capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2u) {
                capacity = needed;
                break;
            }
            capacity *= 2u;
        }
        if (capacity > SIZE_MAX / sizeof(*job->processes)) {
            errno = ENOMEM;
            return -1;
        }
        struct ash_process* processes = realloc(
            job->processes,
            capacity * sizeof(*processes)
        );
        if (processes == NULL) {
            return -1;
        }
        job->processes = processes;
        job->process_capacity = capacity;
    }
    *process_index = job->process_count++;
    job->processes[*process_index] = (struct ash_process){
        .role = role,
        .child = {
            .state = ASH_CHILD_PREPARED,
        },
    };
    job->status_process = *process_index;
    assert(ash_jobs_invariants(job->owner));
    return 0;
}

int ash_job_register_process(
    struct ash_job* job,
    size_t process_index,
    pid_t pid,
    pid_t process_group
) {
    if (!ash_job_owned(job) || job->state != ASH_JOB_PREPARING ||
        process_index >= job->process_count || pid <= 0 ||
        process_group < 0 ||
        (process_group > 0 && job->process_group > 0 &&
         process_group != job->process_group)) {
        errno = EINVAL;
        return -1;
    }
    struct ash_process* process = &job->processes[process_index];
    if (process->child.state != ASH_CHILD_PREPARED ||
        ash_pid_is_active(job->owner, pid, process)) {
        errno = EINVAL;
        return -1;
    }
    process->child.pid = pid;
    process->child.process_group = process_group;
    process->child.state = ASH_CHILD_RUNNING;
    if (job->process_group == 0 && process_group > 0) {
        job->process_group = process_group;
    }
    assert(ash_jobs_invariants(job->owner));
    return 0;
}

static void ash_job_remove_last_process(struct ash_job* job) {
    assert(job->process_count != 0u);
    job->process_count--;
    job->processes[job->process_count] = (struct ash_process){0};
    job->status_process = job->process_count == 0u ?
        0u : job->process_count - 1u;
}

int ash_job_start_process(
    struct ash_job* job,
    enum ash_process_role role,
    ash_child_callback callback,
    void* user_data,
    size_t* process_index
) {
    if (callback == NULL || process_index == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ash_job_add_process(job, role, process_index) != 0) {
        return -1;
    }
    pid_t pid = bx_child_fork_callback_start(callback, user_data);
    if (pid < 0) {
        ash_job_remove_last_process(job);
        assert(ash_jobs_invariants(job->owner));
        return -1;
    }
    if (ash_job_register_process(
            job,
            *process_index,
            pid,
            0
        ) != 0) {
        int error = errno;
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        ash_job_remove_last_process(job);
        assert(ash_jobs_invariants(job->owner));
        errno = error;
        return -1;
    }
    return 0;
}

bool ash_job_commit(
    struct ash_job* job,
    enum ash_job_visibility visibility
) {
    if (!ash_job_owned(job) || job->state != ASH_JOB_PREPARING ||
        (visibility != ASH_JOB_PRIVATE &&
         visibility != ASH_JOB_PUBLISHED) ||
        (visibility == ASH_JOB_PUBLISHED && job->foreground) ||
        job->process_count == 0u) {
        return false;
    }
    for (size_t i = 0u; i < job->process_count; i++) {
        if (job->processes[i].child.state != ASH_CHILD_RUNNING) {
            return false;
        }
    }
    unsigned long id = 0u;
    if (visibility == ASH_JOB_PUBLISHED) {
        id = ash_job_allocate_id(job->owner);
        if (id == 0u) {
            return false;
        }
    }
    job->id = id;
    job->visibility = visibility;
    job->state = ASH_JOB_RUNNING;
    assert(ash_jobs_invariants(job->owner));
    return true;
}

static void ash_job_refresh_state(struct ash_job* job) {
    if (job->state == ASH_JOB_PREPARING) {
        return;
    }
    bool running = false;
    bool stopped = false;
    for (size_t i = 0u; i < job->process_count; i++) {
        running |= job->processes[i].child.state == ASH_CHILD_RUNNING;
        stopped |= job->processes[i].child.state == ASH_CHILD_STOPPED;
    }
    job->state = running ?
        ASH_JOB_RUNNING :
        (stopped ? ASH_JOB_STOPPED : ASH_JOB_COMPLETED);
}

bool ash_child_record_wait_status(
    struct ash_job* job,
    size_t process_index,
    int wait_status
) {
    if (!ash_job_owned(job) || process_index >= job->process_count) {
        return false;
    }
    struct ash_process* process = &job->processes[process_index];
    if (process->child.state != ASH_CHILD_RUNNING &&
        process->child.state != ASH_CHILD_STOPPED) {
        return false;
    }

    enum ash_child_state state;
    if (WIFEXITED(wait_status)) {
        state = ASH_CHILD_EXITED;
    }
    else if (WIFSIGNALED(wait_status)) {
        state = ASH_CHILD_SIGNALED;
    }
    else if (WIFSTOPPED(wait_status)) {
        state = ASH_CHILD_STOPPED;
    }
#ifdef WIFCONTINUED
    else if (WIFCONTINUED(wait_status)) {
        state = ASH_CHILD_RUNNING;
    }
#endif
    else {
        return false;
    }
    process->child.state = state;
    process->child.wait_status = wait_status;
    ash_job_refresh_state(job);
    assert(ash_jobs_invariants(job->owner));
    return true;
}

int ash_child_exit_status(const struct ash_child* child) {
    if (child == NULL) {
        return 1;
    }
    if (child->state == ASH_CHILD_EXITED) {
        return WEXITSTATUS(child->wait_status);
    }
    if (child->state == ASH_CHILD_SIGNALED) {
        return 128 + WTERMSIG(child->wait_status);
    }
    if (child->state == ASH_CHILD_STOPPED) {
        return 128 + WSTOPSIG(child->wait_status);
    }
    return 1;
}

static int ash_job_wait_process(
    struct ash_job* job,
    size_t process_index
) {
    struct ash_process* process = &job->processes[process_index];
    if (ash_process_terminal(process)) {
        return 0;
    }
    int wait_status;
    while (waitpid(process->child.pid, &wait_status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    if (!ash_child_record_wait_status(job, process_index, wait_status)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int ash_job_wait(struct ash_job* job, int* exit_status) {
    if (!ash_job_owned(job) || job->state == ASH_JOB_PREPARING ||
        exit_status == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0u; i < job->process_count; i++) {
        if (ash_job_wait_process(job, i) != 0) {
            return -1;
        }
    }
    *exit_status = ash_child_exit_status(
        &job->processes[job->status_process].child
    );
    return 0;
}

int ash_job_signal(const struct ash_job* job, int signal_number) {
    if (!ash_job_owned(job) || signal_number <= 0) {
        errno = EINVAL;
        return -1;
    }
    int result = 0;
    for (size_t i = 0u; i < job->process_count; i++) {
        const struct ash_process* process = &job->processes[i];
        if ((process->child.state == ASH_CHILD_RUNNING ||
             process->child.state == ASH_CHILD_STOPPED) &&
            kill(process->child.pid, signal_number) != 0 &&
            errno != ESRCH) {
            result = -1;
        }
    }
    return result;
}

static void ash_job_unlink(struct ash_job* job) {
    struct ash_job** link = &job->owner->jobs;
    while (*link != NULL && *link != job) {
        link = &(*link)->next;
    }
    if (*link == job) {
        *link = job->next;
    }
}

bool ash_job_release(struct ash_job* job) {
    if (!ash_job_owned(job)) {
        return false;
    }
    for (size_t i = 0u; i < job->process_count; i++) {
        enum ash_child_state state = job->processes[i].child.state;
        if (state == ASH_CHILD_RUNNING ||
            state == ASH_CHILD_STOPPED) {
            return false;
        }
    }
    struct ash_shell* owner = job->owner;
    (void)owner;
    ash_job_unlink(job);
    free(job->processes);
    free(job);
    assert(ash_jobs_invariants(owner));
    return true;
}

void ash_job_abort(struct ash_job* job) {
    if (!ash_job_owned(job)) {
        return;
    }
    struct ash_shell* owner = job->owner;
    (void)owner;
    /*
     * Abort is failure cleanup, not user-visible job termination. Use an
     * uncatchable signal so a stopped or signal-handling child cannot keep
     * rollback blocked indefinitely.
     */
    (void)ash_job_signal(job, SIGKILL);
    for (size_t i = 0u; i < job->process_count; i++) {
        struct ash_process* process = &job->processes[i];
        if (process->child.state == ASH_CHILD_RUNNING ||
            process->child.state == ASH_CHILD_STOPPED) {
            int wait_status;
            pid_t waited;
            do {
                waited = waitpid(process->child.pid, &wait_status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited == process->child.pid) {
                (void)ash_child_record_wait_status(
                    job,
                    i,
                    wait_status
                );
            }
        }
    }
    ash_job_unlink(job);
    free(job->processes);
    free(job);
    assert(ash_jobs_invariants(owner));
}

static struct ash_job* ash_jobs_find_pid(
    struct ash_shell* shell,
    pid_t pid,
    size_t* process_index
) {
    for (struct ash_job* job = shell->jobs;
         job != NULL;
         job = job->next) {
        if (job->visibility != ASH_JOB_PUBLISHED) {
            continue;
        }
        for (size_t i = 0u; i < job->process_count; i++) {
            if (job->processes[i].child.pid == pid) {
                *process_index = i;
                return job;
            }
        }
    }
    return NULL;
}

int ash_jobs_wait_pid(
    struct ash_shell* shell,
    pid_t pid,
    int* exit_status
) {
    if (shell == NULL || pid <= 0 || exit_status == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t process_index;
    struct ash_job* job = ash_jobs_find_pid(
        shell,
        pid,
        &process_index
    );
    if (job == NULL) {
        errno = ECHILD;
        return -1;
    }
    if (ash_job_wait_process(job, process_index) != 0) {
        return -1;
    }
    *exit_status = ash_child_exit_status(
        &job->processes[process_index].child
    );
    ash_job_refresh_state(job);
    if (job->state == ASH_JOB_COMPLETED) {
        (void)ash_job_release(job);
    }
    return 0;
}

int ash_jobs_wait_all(struct ash_shell* shell, int* exit_status) {
    if (shell == NULL || exit_status == NULL) {
        errno = EINVAL;
        return -1;
    }
    *exit_status = 0;
    while (true) {
        struct ash_job* job = shell->jobs;
        while (job != NULL &&
               job->visibility != ASH_JOB_PUBLISHED) {
            job = job->next;
        }
        if (job == NULL) {
            return 0;
        }
        if (ash_job_wait(job, exit_status) != 0) {
            return -1;
        }
        (void)ash_job_release(job);
    }
}

void ash_jobs_destroy(struct ash_shell* shell) {
    if (shell == NULL) {
        return;
    }
    assert(ash_jobs_invariants(shell));
    struct ash_job* job = shell->jobs;
    shell->jobs = NULL;
    while (job != NULL) {
        struct ash_job* next = job->next;
        for (size_t i = 0u; i < job->process_count; i++) {
            struct ash_process* process = &job->processes[i];
            if (process->child.state == ASH_CHILD_RUNNING ||
                process->child.state == ASH_CHILD_STOPPED) {
                (void)waitpid(process->child.pid, NULL, WNOHANG);
            }
        }
        free(job->processes);
        free(job);
        job = next;
    }
    assert(ash_jobs_invariants(shell));
}

pid_t ash_job_last_pid(const struct ash_job* job) {
    if (!ash_job_owned(job) || job->process_count == 0u) {
        return -1;
    }
    return job->processes[job->process_count - 1u].child.pid;
}

void ash_jobs_detach_after_fork(struct ash_shell* shell) {
    if (shell == NULL) {
        return;
    }
    assert(ash_jobs_invariants(shell));
    shell->jobs = NULL;
    shell->next_job_id = 1u;
    shell->last_async_pid = -1;
    shell->shell_pid = getpid();
    assert(ash_jobs_invariants(shell));
}
