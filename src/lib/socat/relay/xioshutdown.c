/* source: xioshutdown.c */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

/* this is the source of the extended shutdown function */


#include "xiosysincludes.h"
#include "xioopen.h"


int xioshutdown(xiofile_t *sock, int how) {
   int result = 0;

   if (sock->tag == XIO_TAG_INVALID || sock->tag & XIO_TAG_CLOSED) {
      Error("xioshutdown(): invalid file descriptor");
      errno = EINVAL;
      return -1;
   }

   if (sock->tag == XIO_TAG_DUAL) {
      if ((how+1)&1) {
	 result = xioshutdown((xiofile_t *)sock->dual.stream[0], 0);
      }
      if ((how+1)&2) {
	 result |= xioshutdown((xiofile_t *)sock->dual.stream[1], 1);
      }
      return result;
   }

   switch (sock->stream.howtoshut) {
      char writenull;
   case XIOSHUT_NONE:
      return 0;
   case XIOSHUT_CLOSE:
      if (Close(sock->stream.fd) < 0) {
	 Info2("close(%d): %s",
	       sock->stream.fd, strerror(errno));
      }
      return 0;
#if _WITH_SOCKET
   case XIOSHUT_DOWN:
      result = Shutdown(sock->stream.fd, how);
      if (result < 0) {
	 int level, _errno = errno;
	 switch (_errno) {
	 case EPIPE:
	 case ECONNRESET:
	    level = E_ERROR;
	    break;
	 default:
	    level = E_INFO; /* old behaviour */
	    break;
	 }
	 Msg3(level, "shutdown(%d, %d): %s",
	      sock->stream.fd, how, strerror(_errno));
	 errno = _errno;
	 return -1;
      }
      return 0;
   case XIOSHUT_NULL:
      writenull = '\0'; 	/* assign something to make gcc happy */
      /* send an empty packet; only useful on datagram sockets? */
      xiowrite(sock, &writenull, 0);
      return 0;
#endif /* _WITH_SOCKET */
   default: ;
   }
   /* XIOSHUT_UNSPEC passes on */

   if (false) {
      ;
#if WITH_OPENSSL
   } else if ((sock->stream.dtype & XIODATA_MASK) == XIODATA_OPENSSL) {
      xioshutdown_openssl(&sock->stream, how);
#endif /* WITH_OPENSSL */

   } else if ((sock->stream.dtype & XIODATA_MASK) == XIODATA_PIPE) {
      if ((how+1)&1) {
	 if (Close(sock->stream.fd) < 0) {
	    Info2("close(%d): %s",
		  sock->stream.fd, strerror(errno));
	 }
      }
      if ((how+1)&2) {
	 if (Close(sock->stream.para.bipipe.fdout) < 0) {
	    Info2("close(%d): %s",
		  sock->stream.para.bipipe.fdout, strerror(errno));
	 }
      }

   } else if ((sock->stream.dtype & XIODATA_MASK) == XIODATA_2PIPE) {
      if ((how+1)&1) {
	 if (Close(sock->stream.fd) < 0) {
	    Info2("close(%d): %s",
		  sock->stream.fd, strerror(errno));
	 }
      }
      if ((how+1)&2) {
	 if (Close(sock->stream.para.exec.fdout) < 0) {
	    Info2("close(%d): %s",
		  sock->stream.para.exec.fdout, strerror(errno));
	 }
      }
#if _WITH_SOCKET
   } else if (sock->stream.howtoend == END_SHUTDOWN) {
      if ((result = Shutdown(sock->stream.fd, how)) < 0) {
	 Info3("shutdown(%d, %d): %s",
	       sock->stream.fd, how, strerror(errno));
      }
   } else if (sock->stream.howtoend == END_SHUTDOWN_KILL) {
      if ((result = Shutdown(sock->stream.fd, how)) < 0) {
	 Info3("shutdown(%d, %d): %s",
	       sock->stream.fd, how, strerror(errno));
      }
   } else if ((sock->stream.dtype & XIODATA_MASK) ==
	      (XIODATA_RECVFROM & XIODATA_MASK)) {
      if (how >= 1) {
	 if (Close(sock->stream.fd) < 0) {
	    Info2("close(%d): %s",
		  sock->stream.fd, strerror(errno));
	 }
	 sock->stream.eof = 2;
	 sock->stream.fd = -1;
      }
#endif /* _WITH_SOCKET */
#if 0
   } else {
      Error1("xioshutdown(): bad data type specification %d", sock->stream.dtype);
      return -1;
#endif

   }
#if 0
   else if (sock->stream.howtoend == END_CLOSE &&
	      sock->stream.dtype == DATA_STREAM) {
      return result;
   }
#if WITH_TERMIOS
   if (sock->stream.ttyvalid) {
      if (Tcsetattr(sock->stream.fd, TCSAFLUSH, &sock->stream.savetty) < 0) {
	 Warn2("cannot restore terminal settings on fd %d: %s",
	       sock->stream.fd, strerror(errno));
      }
   }
#endif /* WITH_TERMIOS */
#endif

   return result;
}
