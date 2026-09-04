/* Source: xio-text.c */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

/* This file contains the source for opening addresses of TEXT type */

#include "xiosysincludes.h"
#include "xioopen.h"

#include "xio-stdio.h"

#include "xio-text.h"


#if WITH_TEXT

static int xioopen_text(int argc, const char *argv[], struct opt *opts, int xioflags, xiofile_t *fd, const struct addrdesc *addrdesc);


const struct addrdesc xioaddr_text = { "TEXT",   3, xioopen_text,  GROUP_FD|GROUP_FIFO, 0, 0, 0 HELP(":<string>") };


/* Process a "text" address with one text arg */
static int xioopen_text(
	int argc,
	const char *argv[],
	struct opt *opts,
	int xioflags,
	union bipipe *xfd,
	const struct addrdesc *addrdesc)
{
	struct single *sfd;
	int datapipe[2]; 	/* takes,holds,provides the text */
	int result;

	if (argc != 2) {
		xio_syntax(argv[0], 1, argc-1,addrdesc->syntax);
		return STAT_NORETRY;
	}

	if ((xioflags & XIO_ACCMODE) == XIO_RDONLY) {
		xfd->common.tag = XIO_TAG_RDONLY;
		sfd = &xfd->stream;

	} else if ((xioflags & XIO_ACCMODE) == XIO_WRONLY) {
#if WITH_STDIO
		const char *keyw_stdout[2] = { "STDOUT" };
		Warn("TEXT address in write-only context degenerates to STDOUT");
		return xioopen_stdio(1, keyw_stdout, opts, xioflags,
				    xfd, &xioaddr_stdout);

#else /* !WITH_STDIO */
		Error("STDIO is disabled, cannot run TEXT with output mode");
		return STAT_NORETRY;
#endif /* !WITH_STDIO */

	} else /* XIO_RDWR */ {
#if WITH_STDIO
		const char *keyw_stdout[2] = { "STDOUT" };
		int rc;
		xfd = xioopen_makedual(xfd, (struct single *)xfd);
		if (xfd == NULL)
			return STAT_NORETRY;
		xfd->dual.stream[0]->tag = XIO_TAG_RDONLY;
		xfd->dual.stream[1]->tag = XIO_TAG_WRONLY;
		xfd->dual.stream[1]->opts = copyopts(opts, GROUP_ALL);
		rc = xioopen_stdio(1, keyw_stdout, xfd->dual.stream[1]->opts,
				   (xioflags & ~XIO_ACCMODE) | XIO_WRONLY,
				   (union bipipe *)xfd->dual.stream[1],
				   &xioaddr_stdout);
		if (rc != STAT_OK)
			return rc;
		sfd = xfd->dual.stream[0];
#else /* !WITH_STDIO */
		Error("STDIO is disabled, cannot run TEXT with output mode");
		return STAT_NORETRY;
#endif /* !WITH_STDIO */
	}

	if (applyopts_single(sfd, opts, PH_INIT) < 0)
		return -1;

	applyopts(sfd, -1, opts, PH_INIT);
	applyopts(sfd, -1, opts, PH_EARLY);

	sfd->dtype             = XIODATA_PIPE;

	Info("creating pipe for text reading");
	if (Pipe(datapipe) != 0) {
		Error2("pipe(%p): %s", datapipe, strerror(errno));
		return -1;
	}
	sfd->fd                = datapipe[0];
	applyopts_cloexec(sfd->fd, opts);
	if (Fcntl_l(datapipe[1], F_SETFD, FD_CLOEXEC) < 0) {
		Warn2("fcntl(%d, F_SETFD, FD_CLOEXEC): %s",
		      datapipe[1], strerror(errno));
	}

	/* One-time and input-direction options, no second application */
	retropt_bool(opts, OPT_IGNOREEOF, &sfd->ignoreeof);

	/* Apply options to first FD */
	if ((result = applyopts(sfd, -1, opts, PH_ALL)) < 0) {
		return result;
	}
	if ((result = applyopts_single(sfd, opts, PH_ALL)) < 0) {
		return result;
	}

	Write(datapipe[1], sfd->argv[1], strlen(sfd->argv[1]));
	Close(datapipe[1]);
	Notice("reading from data pipe");

	return 0;
}

#endif /* WITH_TEXT */
