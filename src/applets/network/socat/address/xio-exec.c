/* source: xio-exec.c */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

/* this file contains the source for opening addresses of exec type */

#include "xiosysincludes.h"
#include "xioopen.h"
#include "lib/argv_packer.h"

#include "xio-progcall.h"
#include "xio-exec.h"

#if WITH_EXEC

static int xioopen_exec(int argc, const char *argv[], struct opt *opts, int xioflags, xiofile_t *xfd, const struct addrdesc *addrdesc);

const struct addrdesc xioaddr_exec = { "EXEC",   3, xioopen_exec, GROUP_FD|GROUP_FORK|GROUP_EXEC|GROUP_SOCKET|GROUP_SOCK_UNIX|GROUP_TERMIOS|GROUP_FIFO|GROUP_PTY|GROUP_PARENT, 0, 0, 0 HELP(":<command-line>") };

const struct optdesc opt_dash = { "dash", "login", OPT_DASH, GROUP_EXEC, PH_PREEXEC, TYPE_BOOL, OFUNC_SPEC, 0, 0, 0 };

static int xioopen_exec(
	int argc,
	const char *argv[],
	struct opt *opts,
	int xioflags,	/* XIO_RDONLY, XIO_MAYCHILD etc. */
	xiofile_t *xfd,
	const struct addrdesc *addrdesc)
{
   struct single *sfd = &xfd->stream;
   int status;
   bool dash = false;
   int duptostderr;
   int numleft;
   char **pargv = NULL;
   char *executable = NULL;

   if (argc != 2) {
      xio_syntax(argv[0], 1, argc-1, addrdesc->syntax);
      return STAT_NORETRY;
   }

   retropt_bool(opts, OPT_DASH, &dash);

   if (bx_argv_parse_command(argv[1], &pargv) < 0) {
      Error1("cannot parse EXEC command line \"%s\"", argv[1]);
      return STAT_NORETRY;
   }
   executable = strdup(pargv[0]);
   if (executable == NULL) {
      Error("out of memory while preparing EXEC command");
      bx_argv_free(pargv);
      return STAT_RETRYLATER;
   }
   if (dash) {
      size_t argv0_len = strlen(pargv[0]);
      char *argv0 = malloc(argv0_len + 2u);
      if (argv0 == NULL) {
	 Error("out of memory while preparing EXEC login command");
	 free(executable);
	 bx_argv_free(pargv);
	 return STAT_RETRYLATER;
      }
      argv0[0] = '-';
      memcpy(argv0 + 1, pargv[0], argv0_len + 1u);
      free(pargv[0]);
      pargv[0] = argv0;
   }
   if (xio_progcall_check_argv(pargv) < 0) {
      free(executable);
      bx_argv_free(pargv);
      return STAT_NORETRY;
   }

   status =
      _xioopen_foxec(xioflags, sfd, addrdesc->groups, &opts, &duptostderr);
   if (status < 0) {
      free(executable);
      bx_argv_free(pargv);
      return status;
   }
   if (status == 0) {	/* child */
      char *path = NULL;

      if (setopt_path(opts, &path) < 0) {
	 /* this could be dangerous, so let us abort this child... */
	 Exit(1);
      }

      dropopts(opts, PH_PASTEXEC);
      if ((numleft = leftopts(opts)) > 0) {
	 showleft(opts);
	 Error1("INTERNAL: %d option(s) remained unused", numleft);
	 return STAT_NORETRY;
      }

      /* only now redirect stderr */
      if (duptostderr >= 0) {
	 diag_dup();
	 Dup2(duptostderr, 2);
      }
      Notice1("executing \"%s\"", executable);
      int exec_error = xio_progcall_exec(executable, pargv);
      errno = exec_error;
      Error2("exec(\"%s\"): %s", executable, strerror(exec_error));
      Exit(1);	/* this child process */
   }

   /* parent */
   free(executable);
   bx_argv_free(pargv);
   _xio_openlate(sfd, opts);
   return 0;
}
#endif /* WITH_EXEC */
