/* Copyright Paul Moses */
/* Published under the GNU General Public License V.2, see file COPYING */

/* Socat child lifecycle and SIGCHLD integration. */


#include "xiosysincludes.h"
#include "xioopen.h"


#define XIO_CHILD_CAPACITY (XIO_MAXSOCK * 2)
#define XIO_CHILD_POLL_NSEC 10000000L
#define XIO_CHILD_TERM_MSEC 500

enum xio_child_state {
   XIO_CHILD_FREE,
   XIO_CHILD_RESERVED,
   XIO_CHILD_RUNNING,
   XIO_CHILD_TERMINATING,
   XIO_CHILD_EXITED,
   XIO_CHILD_CONSUMED
};

struct xio_child {
   enum xio_child_state state;
   struct single *stream;
   pid_t pid;
   int wait_status;
};

static struct xio_child children[XIO_CHILD_CAPACITY];
static volatile sig_atomic_t child_reap_pending;
int engine_result = EXIT_SUCCESS;


static struct xio_child *xio_child_by_pid(pid_t pid) {
   size_t i;

   for (i = 0; i < XIO_CHILD_CAPACITY; ++i) {
      if (children[i].state != XIO_CHILD_FREE &&
	  children[i].state != XIO_CHILD_CONSUMED &&
	  children[i].pid == pid) {
	 return &children[i];
      }
   }
   return NULL;
}

static struct xio_child *xio_child_by_stream(struct single *stream) {
   size_t i;

   for (i = 0; i < XIO_CHILD_CAPACITY; ++i) {
      if (children[i].state != XIO_CHILD_FREE &&
	  children[i].state != XIO_CHILD_CONSUMED &&
	  children[i].stream == stream) {
	 return &children[i];
      }
   }
   return NULL;
}

static void xio_child_record_exit(struct xio_child *child, int status) {
   bool parent_terminated = child->state == XIO_CHILD_TERMINATING;
   struct single *stream = child->stream;

   child->wait_status = status;
   child->state = XIO_CHILD_EXITED;
   stream->para.exec.pid = 0;

   if (WIFEXITED(status)) {
      int level = E_INFO;

      if (WEXITSTATUS(status) != 0 && !parent_terminated) {
	 level = E_WARN;
	 engine_result = EXIT_FAILURE;
      }
      Msg2(level, "waitpid(): child "F_pid" exited with status %d",
	   child->pid, WEXITSTATUS(status));
   } else if (WIFSIGNALED(status)) {
      int level = parent_terminated ? E_INFO : E_WARN;

      Msg2(level, "waitpid(): child "F_pid" exited on signal %d",
	   child->pid, WTERMSIG(status));
      if (!parent_terminated) {
	 engine_result = EXIT_FAILURE;
      }
   } else {
      Warn1("waitpid(): cannot determine status of child "F_pid, child->pid);
   }

   if (stream->sigchild != NULL) {
      (*stream->sigchild)(stream);
   }
}

static void xio_child_publish_lost(struct xio_child *child) {
   Warn1("waitpid(): lost ownership of child "F_pid, child->pid);
   child->stream->para.exec.pid = 0;
   child->state = XIO_CHILD_EXITED;
   engine_result = EXIT_FAILURE;
   if (child->stream->sigchild != NULL) {
      (*child->stream->sigchild)(child->stream);
   }
}

void xio_child_reset(void) {
   memset(children, 0, sizeof(children));
   child_reap_pending = 0;
}

int xio_child_reserve(struct single *stream) {
   size_t i;

   if (stream == NULL || xio_child_by_stream(stream) != NULL) {
      errno = EINVAL;
      return -1;
   }

   for (i = 0; i < XIO_CHILD_CAPACITY; ++i) {
      if (children[i].state == XIO_CHILD_FREE ||
	  children[i].state == XIO_CHILD_CONSUMED) {
	 children[i].state = XIO_CHILD_RESERVED;
	 children[i].stream = stream;
	 children[i].pid = 0;
	 children[i].wait_status = 0;
	 return 0;
      }
   }

   errno = ENOSPC;
   return -1;
}

void xio_child_cancel_reservation(struct single *stream) {
   struct xio_child *child = xio_child_by_stream(stream);

   if (child != NULL && child->state == XIO_CHILD_RESERVED) {
      child->state = XIO_CHILD_CONSUMED;
      child->stream = NULL;
   }
}

int xio_child_publish_pid(struct single *stream, pid_t pid) {
   struct xio_child *child = xio_child_by_stream(stream);

   if (child == NULL || child->state != XIO_CHILD_RESERVED || pid <= 0 ||
       xio_child_by_pid(pid) != NULL) {
      errno = EINVAL;
      return -1;
   }
   child->pid = pid;
   child->state = XIO_CHILD_RUNNING;
   stream->para.exec.pid = pid;
   return 0;
}

void xio_child_abort_launch(struct single *stream, pid_t pid) {
   struct xio_child *child = xio_child_by_stream(stream);

   if (child != NULL && child->state == XIO_CHILD_RESERVED && pid > 0) {
      child->pid = pid;
      child->state = XIO_CHILD_RUNNING;
      stream->para.exec.pid = pid;
      xio_child_close(stream);
   }
}

/* Reaping, diagnostics, callbacks, and status publication all happen in
   ordinary control flow. The physical signal handler only wakes that flow. */
static void xio_child_observe(pid_t pid, int status) {
   struct xio_child *child = xio_child_by_pid(pid);

   if (child != NULL) {
      xio_child_record_exit(child, status);
      return;
   }

   Info1("general child "F_pid" terminated", pid);
   if (num_child > 0) {
      --num_child;
      Info1("number of children decreased to %d", num_child);
   }
}

void xio_child_reap(void) {
   child_reap_pending = 0;
   for (;;) {
      int status;
      pid_t pid = Waitpid(-1, &status, WNOHANG);

      if (pid == 0) {
	 return;
      }
      if (pid < 0) {
	 if (errno == EINTR) {
	    continue;
	 }
	 if (errno != ECHILD) {
	    Warn1("waitpid(-1, {}, WNOHANG): "F_strerror, status);
	 }
	 return;
      }
      xio_child_observe(pid, status);
   }
}

void xio_child_reap_pending(void) {
   if (child_reap_pending != 0) {
      xio_child_reap();
   }
}

void xio_child_wait_general(void) {
   xio_child_reap();
   while (num_child > 0) {
      int status;
      pid_t pid = Waitpid(-1, &status, 0);

      if (pid > 0) {
	 xio_child_observe(pid, status);
      } else if (pid < 0 && errno == EINTR) {
	 continue;
      } else if (pid < 0 && errno == ECHILD) {
	 num_child = 0;
      } else if (pid < 0) {
	 Warn1("waitpid(-1, {}, 0): "F_strerror, status);
	 break;
      }
   }
}

static int64_t xio_child_now_msec(void) {
   struct timespec now;

   if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
      return -1;
   }
   return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int64_t xio_child_timeout_msec(const struct timeval *timeout) {
   if (timeout == NULL || timeout->tv_sec < 0 || timeout->tv_usec < 0) {
      return 0;
   }
   if ((uint64_t)timeout->tv_sec > (uint64_t)INT64_MAX / 1000) {
      return INT64_MAX;
   }
   return (int64_t)timeout->tv_sec * 1000 +
      (timeout->tv_usec + 999) / 1000;
}

static bool xio_child_wait(struct xio_child *child, int64_t timeout_msec) {
   int64_t start = xio_child_now_msec();
   struct timespec pause = { 0, XIO_CHILD_POLL_NSEC };

   while (child->state == XIO_CHILD_RUNNING ||
	  child->state == XIO_CHILD_TERMINATING) {
      int status;
      pid_t pid = Waitpid(child->pid, &status, WNOHANG);

      if (pid == child->pid) {
	 xio_child_record_exit(child, status);
	 return true;
      }
      if (pid < 0) {
	 if (errno == EINTR) {
	    continue;
	 }
	 if (errno == ECHILD) {
	    xio_child_publish_lost(child);
	 } else {
	    Warn2("waitpid("F_pid", {}, WNOHANG): %s",
		  child->pid, strerror(errno));
	 }
	 return true;
      }
      if (timeout_msec <= 0) {
	 return false;
      }
      if (start >= 0) {
	 int64_t now = xio_child_now_msec();
	 if (now < 0 || now - start >= timeout_msec) {
	    return false;
	 }
      }
      Nanosleep(&pause, NULL);
   }
   return true;
}

static void xio_child_consume(struct xio_child *child) {
   child->stream->para.exec.pid = 0;
   child->state = XIO_CHILD_CONSUMED;
   child->stream = NULL;
   child->pid = 0;
   child->wait_status = 0;
}

static void xio_child_drain_stream(struct single *stream,
				   const struct timeval *timeout) {
   struct xio_child *child = xio_child_by_stream(stream);

   if (child == NULL || child->state == XIO_CHILD_EXITED) {
      return;
   }
   xio_child_wait(child, xio_child_timeout_msec(timeout));
}

void xio_child_drain(xiofile_t *xfd, const struct timeval *timeout) {
   xio_child_reap();
   if (xfd == NULL) {
      return;
   }
   if (xfd->tag == XIO_TAG_DUAL) {
      xio_child_drain_stream(xfd->dual.stream[0], timeout);
      xio_child_drain_stream(xfd->dual.stream[1], timeout);
   } else {
      xio_child_drain_stream(&xfd->stream, timeout);
   }
}

void xio_child_close(struct single *stream) {
   struct xio_child *child;
   struct timeval term_timeout = {
      XIO_CHILD_TERM_MSEC / 1000,
      (XIO_CHILD_TERM_MSEC % 1000) * 1000
   };

   xio_child_reap();
   child = xio_child_by_stream(stream);
   if (child == NULL) {
      stream->para.exec.pid = 0;
      return;
   }
   if (child->state == XIO_CHILD_EXITED) {
      xio_child_consume(child);
      return;
   }

   child->state = XIO_CHILD_TERMINATING;
   if (Kill(child->pid, SIGTERM) < 0 && errno != ESRCH) {
      Warn2("kill("F_pid", SIGTERM): %s", child->pid, strerror(errno));
   }
   if (!xio_child_wait(child, xio_child_timeout_msec(&term_timeout))) {
      if (Kill(child->pid, SIGKILL) < 0 && errno != ESRCH) {
	 Warn2("kill("F_pid", SIGKILL): %s", child->pid, strerror(errno));
      }
      while (child->state == XIO_CHILD_TERMINATING) {
	 int status;
	 pid_t pid = Waitpid(child->pid, &status, 0);

	 if (pid == child->pid) {
	    xio_child_record_exit(child, status);
	    break;
	 }
	 if (pid < 0 && errno == EINTR) {
	    continue;
	 }
	 if (pid < 0 && errno == ECHILD) {
	    xio_child_publish_lost(child);
	 } else if (pid < 0) {
	    Warn2("waitpid("F_pid", {}, 0): %s",
		  child->pid, strerror(errno));
	 }
	 break;
      }
   }
   xio_child_consume(child);
}

/* Register a callback for an xio descriptor's command child. */
int xiosetsigchild(xiofile_t *xfd, int (*callback)(struct single *)) {
   if (xfd->tag != XIO_TAG_DUAL) {
      xfd->stream.sigchild = callback;
   } else {
      xfd->dual.stream[0]->sigchild = callback;
      xfd->dual.stream[1]->sigchild = callback;
   }
   xio_child_reap();
   return 0;
}

static void childdied(int signum) {
   (void)signum;
   child_reap_pending = 1;
}

int xiosetchilddied(void) {
#if HAVE_SIGACTION
   struct sigaction act;
   memset(&act, 0, sizeof(struct sigaction));
   act.sa_flags = SA_NOCLDSTOP;
   act.sa_handler = childdied;
   sigfillset(&act.sa_mask);
   if (Sigaction(SIGCHLD, &act, NULL) < 0) {
      Warn2("sigaction(SIGCHLD, %p, NULL): %s", childdied, strerror(errno));
   }
#else /* HAVE_SIGACTION */
   if (Signal(SIGCHLD, childdied) == SIG_ERR) {
      Warn2("signal(SIGCHLD, %p): %s", childdied, strerror(errno));
   }
#endif /* !HAVE_SIGACTION */
   return 0;
}
