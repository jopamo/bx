/* Source: xio-stall.c */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

/* This file contains the source for opening addresses of STALL type */

#include "xiosysincludes.h"
#include "xioopen.h"

#include "xio-stall.h"


#if WITH_STALL

static int xioopen_stall(int argc, const char *argv[], struct opt *opts, int xioflags, xiofile_t *fd, const struct addrdesc *addrdesc);


const struct addrdesc xioaddr_stall = { "STALL",   3, xioopen_stall,  GROUP_FD|GROUP_FIFO, 0, 0, 0 HELP("") };


/* Process an "stall" address without args */
static int xioopen_stall(
	int argc,
	const char *argv[],
	struct opt *opts0,
	int xioflags,
	xiofile_t *xfd,
	const struct addrdesc *addrdesc)
{
	struct single *sfd = &xfd->stream;
	struct opt *opts1;
	int filedes0[2]; 	/* read only stall/idle pipe */
	int filedes1[2]; 	/* write only stall pipe */
	int result;

	if (argc != 1) {
		xio_syntax(argv[0], 1, argc-1,addrdesc->syntax);
		return STAT_NORETRY;
	}

	if (applyopts_single(sfd, opts0, PH_INIT) < 0)
		return -1;
	applyopts(sfd, -1, opts0, PH_INIT);
	applyopts(sfd, -1, opts0, PH_EARLY);

	/* Here we copy options for second FD */
	if ((opts1 = copyopts(opts0, GROUP_ALL)) == NULL) {
		return STAT_NORETRY;
	}

	if      ((xioflags & XIO_ACCMODE) == XIO_RDWR)
		sfd->tag = XIO_TAG_RDWR;
	else if ((xioflags & XIO_ACCMODE) == XIO_RDONLY)
		sfd->tag = XIO_TAG_RDONLY;
	else if ((xioflags & XIO_ACCMODE) == XIO_WRONLY)
		sfd->tag = XIO_TAG_WRONLY;

	sfd->dtype             = XIODATA_2PIPE;

	if (sfd->tag == XIO_TAG_RDWR || sfd->tag == XIO_TAG_RDONLY) {
		Info("creating pipe for stalled reading");
		if (Pipe(filedes0) != 0) {
			Error2("pipe(%p): %s", filedes0, strerror(errno));
			return -1;
		}
		applyopts_cloexec(filedes0[0], opts0);
		sfd->fd = filedes0[0];
	}

	if (sfd->tag == XIO_TAG_RDWR || sfd->tag == XIO_TAG_WRONLY) {
		Info("creating pipe for stalled writing");
		if (Pipe(filedes1) != 0) {
			Error2("pipe(%p): %s", filedes1, strerror(errno));
			return -1;
		}
		applyopts_cloexec(filedes1[1], opts1);
		sfd->para.bipipe.fdout = filedes1[1];
	}

	if (sfd->tag == XIO_TAG_RDWR || sfd->tag == XIO_TAG_WRONLY) {
		int fflags;		/* save flags, temp.O_NONBLOCK */
		size_t pipesz = 0;
		void *zeros;


		/* Fill the dead end pipe with dummy data so it will never be writable
		   in the transfer loop */
#if defined(F_GETPIPE_SZ)
		pipesz = Fcntl(filedes1[1], F_GETPIPE_SZ);
#endif
#define GUESS_PIPESZ ((size_t)65536)	/* value on Linux */
		if (pipesz <= 0) {
			Info1("could not determine pipe size, guessing: "F_Zu, GUESS_PIPESZ);
			pipesz = GUESS_PIPESZ;
		}
		if (fflags = Fcntl_i(filedes1[1], F_GETFL, 0) < 0)
			Notice2("fcntl(%d, F_GETFL, 0): %s",
				filedes1[1], strerror(errno));
		if (Fcntl_i(filedes1[1], F_SETFL, fflags|O_NONBLOCK) < 0)
			Notice3("fcntl(%d, F_SETFL, %d): %s",
				filedes1[1], fflags|O_NONBLOCK, strerror(errno));

		zeros = Calloc(1, pipesz);
		if (zeros == NULL)
			return STAT_RETRYLATER;
		while (1) {
			ssize_t writt;
			writt = Write(filedes1[1], zeros, pipesz);
			if (writt < 0 && errno == EWOULDBLOCK) {
				Info3("write(%d, zeros, "F_Zu"): %s",
				      filedes1[1], pipesz, strerror(errno));
				break;
			}
			if (writt < 0) {
				int _errno = errno;
				Error3("write(%d, , "F_Zu"): %s",
				       filedes1[1], pipesz, strerror(errno));
				errno = _errno; 	/* not yet used */
				return STAT_NORETRY;
			}
			if (writt == pipesz)
				continue;
			if (writt < pipesz)
				break;
		}
		if (Fcntl_i(filedes1[1], F_SETFL, fflags) < 0)
			Notice3("fcntl(%d, F_SETFL, %d): %s",
				filedes1[1], fflags, strerror(errno));
	}

	/* One-time and input-direction options, no second application */
	retropt_bool(opts0, OPT_IGNOREEOF, &sfd->ignoreeof);

	/* Apply options to first FD */
	if ((result = applyopts(sfd, -1, opts0, PH_ALL)) < 0) {
		return result;
	}
	if ((result = applyopts_single(sfd, opts0, PH_ALL)) < 0) {
		return result;
	}

	switch (xioflags & XIO_ACCMODE) {
	case XIO_RDONLY: Notice("reading from stall pipe"); break;
	case XIO_WRONLY: Notice("writing to nothing"); break;
	case XIO_RDWR:   Notice("reading from stall pipe, writing to nothing"); break;
	}

	return 0;
}

#endif /* WITH_STALL */
