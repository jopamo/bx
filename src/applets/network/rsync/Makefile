include Makefile.configure

CPPFLAGS += -I. -I../../..

OBJS	   = batch.o \
	     blocks.o \
	     cleanup.o \
	     client.o \
	     compats.o \
	     copy.o \
	     daemon.o \
	     daemon_cfg.o \
	     daemon_misc.o \
	     downloader.o \
	     fargs.o \
	     flist.o \
	     fmap.o \
	     hash.o \
	     ids.o \
	     io.o \
	     log.o \
	     md4.o \
	     misc.o \
	     mkpath.o \
	     mktemp.o \
	     platform.o \
	     bx_open_resolve.o \
	     bx_rsync_sbuf.o \
	     bx_text_buffer.o \
	     bx_rsync_mode.o \
	     bx_file_info_fmt.o \
	     bx_rsync_humanize.o \
	     bx_size_parse.o \
	     receiver.o \
	     rmatch.o \
	     rules.o \
	     sender.o \
	     server.o \
	     session.o \
	     socket.o \
	     symlinks.o \
	     uploader.o

ALLOBJS	   = $(OBJS) \
	     main.o
all: openrsync

asan: CFLAGS += -O0 -g3 -fsanitize=address,undefined
asan: LDFLAGS += -fsanitize=address,undefined
asan: openrsync

debug: CFLAGS += -O0 -g3
debug: openrsync

openrsync: $(ALLOBJS)
	$(CC) $(LDFLAGS) -o $@ $(ALLOBJS) -lm -lz $(LDADD_BSD) $(LDADD_LIB_SOCKET) $(LDADD_SCAN_SCALED) $(LDADD_B64_NTOP) $(LDADD_CRYPT)

sender.o downloader.o: compression.h

bx_rsync_sbuf.o: sbuf.c portable_sbuf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ sbuf.c

bx_text_buffer.o: ../../../lib/text_buffer.c ../../../lib/text_buffer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include config.h -c -o $@ ../../../lib/text_buffer.c

bx_open_resolve.o: ../../../lib/open_resolve.c ../../../lib/open_resolve.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ ../../../lib/open_resolve.c

bx_rsync_mode.o: mode.c ../../../lib/file_info_fmt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include config.h -c -o $@ mode.c

bx_file_info_fmt.o: ../../../lib/file_info_fmt.c ../../../lib/file_info_fmt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include config.h -c -o $@ ../../../lib/file_info_fmt.c

bx_rsync_humanize.o: humanize.c portable_util.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ humanize.c

bx_size_parse.o: ../../../lib/size_parse.c ../../../lib/size_parse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -include config.h -c -o $@ ../../../lib/size_parse.c

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	mkdir -p $(DESTDIR)$(MANDIR)/man5
	$(INSTALL_MAN) openrsync.1 $(DESTDIR)$(MANDIR)/man1
	$(INSTALL_MAN) rsync.5 rsyncd.5 rsyncd.conf.5 $(DESTDIR)$(MANDIR)/man5
	$(INSTALL_PROGRAM) openrsync $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/openrsync
	rm -f $(DESTDIR)$(MANDIR)/man1/openrsync.1
	rm -f $(DESTDIR)$(MANDIR)/man5/rsync.5
	rm -f $(DESTDIR)$(MANDIR)/man5/rsyncd.5
	rm -f $(DESTDIR)$(MANDIR)/man5/rsyncd.conf.5

clean:
	rm -f $(ALLOBJS) openrsync

distclean: clean
	rm -f Makefile.configure config.h config.log

$(ALLOBJS): extern.h config.h

blocks.o downloader.o hash.o md4.o: md4.h

# Suppress some warnings on imported code as they're extremely
# noisy and detract from the warnings we desire to see:
#
${ZLIBOBJS}: CFLAGS+=-Wno-deprecated-non-prototype -Wno-strict-prototypes
