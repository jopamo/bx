/* source: filan_main.c */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

const char copyright[] = "filan by Gerhard Rieger and contributors - see http://www.dest-unreach.org/socat/";

#include "config.h"
#include "xioconfig.h"
#include "sysincludes.h"

#include "mytypes.h"
#include "compat.h"
#include "error.h"
#include "sycls.h"
#include "filan.h"


#define WITH_HELP 1


struct filan_data {
   FILE *fdout;
   const char *outfname;
   const char *filename;
   unsigned int m;
   unsigned int n;
   int style;
} ;

struct filan_data filan_data;


static void filan_usage(FILE *fd);
static int filan_once(struct filan_data *data);
static void sigaction_sigwinch(int signum, siginfo_t *siginfo, void *data);


int main(int argc, const char *argv[]) {
   const char **arg1, *a0, *a;
   struct filan_data *data = &filan_data;
   const char *waittimetxt;
   struct timespec waittime = { 0, 0 };
   unsigned long fildes;
#if defined(SIGWINCH)
   bool sigwinch_loop = false;
#endif
   int rc;

   data->fdout = stdout;
   data->outfname = NULL;
   data->filename = NULL;
   data->m = 0;		/* first FD (default) */
   data->n = FD_SETSIZE;	/* last excl. */
   data->style = 0;

   diag_set('I', NULL);
   diag_set('p', strchr(argv[0], '/') ? strrchr(argv[0], '/')+1 : argv[0]);

   arg1 = argv+1;  --argc;
   while (arg1[0] && (arg1[0][0] == '-')) {
      switch (arg1[0][1]) {
#if WITH_HELP
      case '?': case 'h':
	 filan_usage(stdout); exit(0);
#endif
#if LATER
      case 'V': filan_version(stdout); exit(0);
#endif
      case 'L': filan_followsymlinks = true; break;
      case 'd': diag_set('d', NULL); break;
      case 's': data->style = arg1[0][1]; break;
      case 'S': data->style = arg1[0][1]; break;
      case 'r': filan_rawoutput = true; break;
#if defined(SIGWINCH)
      case 'W': sigwinch_loop = true; break;
#endif
      case 'i':  if (arg1[0][2]) {
	    a = a0 = *arg1+2;
         } else {
	    ++arg1, --argc;
	    if ((a = a0 = *arg1) == NULL) {
	       Error("option -i requires an argument");
	       filan_usage(stderr); exit(1);
	    }
	 }
         data->m = strtoul(a, (char **)&a, 0);
	 if (a == a0) {
	    Error1("not a numerical arg in \"-i %s\"", a0);
	 }
	 if (*a != '\0') {
	    Error1("trailing garbage in \"-i %s\"", a0);
	 }
	 data->n = data->m;
	 break;
      case 'n': if (arg1[0][2]) {
	    a = a0 = *arg1+2;
	 } else {
	    ++arg1, --argc;
	    if ((a = a0 = *arg1) == NULL) {
	       Error("option -n requires an argument");
	       filan_usage(stderr); exit(1);
	    }
	 }
         data->n = strtoul(a, (char **)&a, 0);
	 if (a == a0) {
	    Error1("not a numerical arg in \"-n %s\"", a0);
	 }
	 if (*a != '\0') {
	    Error1("trailing garbage in \"-n %s\"", a0);
	 }
	 break;
      case 'f': if (arg1[0][2]) {
	    data->filename = *arg1+2;
	 } else {
	    ++arg1, --argc;
	    if ((data->filename = *arg1) == NULL) {
	       Error("option -f requires an argument");
	       filan_usage(stderr); exit(1);
	    }
	 }
	 break;
      case 'T': if (arg1[0][2]) {
	    waittimetxt = *arg1+2;
	 } else {
	    ++arg1, --argc;
	    if ((waittimetxt = *arg1) == NULL) {
	       Error("option -T requires an argument");
	       filan_usage(stderr); exit(1);
	    }
	 }
	 {
	    double waittimedbl;
	    waittimedbl = strtod(waittimetxt, NULL);
	    waittime.tv_sec  = waittimedbl;
	    waittime.tv_nsec = (waittimedbl-waittime.tv_sec) * 1000000000;
	 }
	 break;
      case 'o':  if (arg1[0][2]) {
            data->outfname = *arg1+2;
         } else {
            ++arg1, --argc;
            if ((data->outfname = *arg1) == NULL) {
               Error("option -o requires an argument");
               filan_usage(stderr); exit(1);
            }
         }
         break;
      case '\0': break;
      default:
	 diag_set_int('e', E_FATAL);
	 Error1("unknown option %s", arg1[0]);
#if WITH_HELP
	 filan_usage(stderr);
#endif
	 exit(1);
      }
#if 0
      if (arg1[0][1] == '\0')
	 break;
#endif
      ++arg1; --argc;
   }
   if (argc != 0) {
      Error1("%d superfluous arguments", argc);
      filan_usage(stderr);
      exit(1);
   }
   if (data->outfname) {
      /* special cases */
      if (!strcmp(data->outfname,"stdin")) { data->fdout = stdin; }
      else if (!strcmp(data->outfname,"stdout")) { data->fdout = stdout; }
      else if (!strcmp(data->outfname,"stderr")) { data->fdout = stderr; }
      /* file descriptor */
      else if (*data->outfname == '+') {
	 a  = data->outfname+1;
	 fildes = strtoul(a, (char **)&a, 0);
	 if ((data->fdout = fdopen(fildes, "w")) == NULL) {
	    Error2("can't fdopen file descriptor %lu: %s\n", fildes, strerror(errno));
	    exit(1);
	 }
      } else {
	 /* file name */
	 if ((data->fdout = fopen(data->outfname, "w")) == NULL) {
	    Error2("can't fopen '%s': %s\n",
		   data->outfname, strerror(errno));
	    exit(1);
	 }
      }
   }

   Nanosleep(&waittime, NULL);

   rc = filan_once(data);

#if defined(SIGWINCH)
   if (sigwinch_loop) {
      struct sigaction act;
      sigset_t mask;

      sigemptyset(&mask);
      act.sa_sigaction = sigaction_sigwinch;
      act.sa_mask   = mask;
      act.sa_flags  = SA_SIGINFO;
      Sigaction(SIGWINCH, &act, NULL);
   }
   while (sigwinch_loop) {
      int rc;

      rc = Select(0, NULL, NULL, NULL, NULL);
      if (rc < 0 && errno == EINTR)
	 continue;
      continue;
   }
#endif /* defined(SIGWINCH) */

   return rc;
}


#if defined(SIGWINCH)
void sigaction_sigwinch(
	int signum,
	siginfo_t *siginfo,
	void *blah)
{
   struct filan_data *data = &filan_data;
   filan_once(data);
}
#endif


static int filan_once(struct filan_data *data)
{
   unsigned int i;

   if (data->style == 0) {
      /* This style gives detailed infos, but requires a file descriptor */
      if (data->filename) {
#if LATER /* this is just in case that S_ISSOCK does not work */
	 struct stat buf;
	 int fd;

	 if (Stat(data->filename, &buf) < 0) {
	    Error3("stat(\"%s\", %p): %s", data->filename, &buf, strerror(errno));
	 }
	 /* note: when S_ISSOCK was undefined, it always gives 0 */
	 if (S_ISSOCK(buf.st_mode)) {
	    Error("cannot analyze UNIX domain socket");
	 }
#endif
	 filan_file(data->filename, data->fdout);
      } else {
	 if (data->m == data->n) {
	    ++data->n;
	 }
	 for (i = data->m; i < data->n; ++i) {
	    filan_fd(i, data->fdout);
	 }
      }
   } else {
      /* this style gives only type and path / socket addresses, and works from
	 file descriptor or filename (with restrictions) */
      if (data->filename) {
	 /* filename: NULL means yet unknown; "" means no name at all */
#if LATER
	 int fd;
	 if ((fd =
	      Open(data->filename, O_RDONLY|O_NOCTTY|O_NONBLOCK
#ifdef O_LARGEFILE
		   |O_LARGEFILE
#endif
		   , 0700))
	     < 0) {
	    Debug2("open(\"%s\", O_RDONLY|O_NOCTTY|O_NONBLOCK|O_LARGEFILE, 0700): %s",
		   data->filename, strerror(errno));
	 }
	 fdname(data->filename, fd, data->fdout, NULL, data->style);
#endif
	 fdname(data->filename, -1, data->fdout, NULL, data->style);
      } else {
	 if (data->m == data->n) {
	    fdname("", data->m, data->fdout, NULL, data->style);
	 } else {
	    for (i = data->m; i < data->n; ++i) {
	       fdname("", i, data->fdout, "%5u ", data->style);
	    }
	 }
      }
   }
   if (data->outfname && data->fdout != stdout && data->fdout != stderr) {
      fclose(data->fdout);
   }
   return 0;
}


#if WITH_HELP
static void filan_usage(FILE *fd) {
   fputs(copyright, fd); fputc('\n', fd);
   fputs("Analyze file descriptors of the process\n", fd);
   fputs("Usage:\n", fd);
   fputs("filan [options]\n", fd);
   fputs("   options:\n", fd);
#if LATER
   fputs("      -V     print version information to stdout, and exit\n", fd);
#endif
#if WITH_HELP
   fputs("      -?|-h          print this help text\n", fd);
   fputs("      -d             increase verbosity (use up to 4 times)\n", fd);
#endif
#if 0
   fputs("      -ly[facility]  log to syslog, using facility (default is daemon)\n", fd);
   fputs("      -lf<logfile>   log to file\n", fd);
   fputs("      -ls            log to stderr (default if no other log)\n", fd);
#endif
   fputs("      -i<fdnum>      only analyze this fd\n", fd);
   fprintf(fd, "      -n<fdnum>      analyze all fds from 0 up to fdnum-1 (default: %u)\n", FD_SETSIZE);
   fputs("      -s             simple output with just type and socket address or path\n", fd);
   fputs("      -S             like -s but improved format and contents\n", fd);
/*   fputs("      -c             alternate device visualization\n", fd);*/
   fputs("      -f<filename>   analyze file system entry\n", fd);
   fputs("      -T<seconds>    wait before analyzing, useful to connect with debugger\n", fd);
   fputs("      -r             raw output for time stamps and rdev\n", fd);
   fputs("      -L             follow symbolic links instead of showing their properties\n", fd);
#if defined(SIGWINCH)
   fputs("      -W             after printing FDs info, wait and reprint info on SIGWINCH\n", fd);
#endif
   fputs("      -o<filename>   output goes to filename, that can be:\n", fd);
   fputs("                     a regular file name, the output goes to that\n", fd);
   fputs("                     +<filedes> , output goes to the file descriptor (which must be open writable)\n", fd);
   fputs("                     the 3 special names stdin stdout and stderr\n", fd);
}
#endif /* WITH_HELP */
