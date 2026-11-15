/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (C) 1995 Wolfgang Solfrank
 * Copyright (c) 1995 Martin Husemann
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "lib/fat/compat.h"
#include <sys/cdefs.h>
#ifndef lint
__RCSID("$NetBSD: main.c,v 1.10 1997/10/01 02:18:14 enami Exp $");
#endif /* not lint */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <getopt.h>
#include <strings.h>

#include "lib/fat/repair/fsutil.h"
#include "lib/fat/repair/ext.h"
#include "applets.h"

int alwaysno;		/* assume "no" for all questions */
int alwaysyes;		/* assume "yes" for all questions */
int preen;		/* set when preening */
int rdonly;		/* device is opened read only (supersedes above) */
int skipclean;		/* skip clean file systems if preening */
int allow_mmap;		/* Allow the use of mmap(), if possible */
int boot_only;
int require_free_hint;

static bool dos_mode;
static bool automatic;
static bool detected;

static void
dos_help(void)
{
	puts("usage: fsck.fat [OPTIONS] DEVICE\n"
	     "  -n        non-interactive read-only check\n"
	     "  -a,-p,-y  automatic repair using conservative BSD repair choices\n"
	     "  -r        interactive repair (default)\n"
	     "  -b        read-only boot-sector check\n"
	     "  -v        verbose output\n"
	     "  -V        verify with a second read-only pass\n"
	     "  --help    show help\n"
	     "Unsupported recovery/variant options fail before opening DEVICE.\n"
	     "BSD options and exit statuses: fsck_msdosfs.");
}

static int
dos_check(int argc, char **argv)
{
	enum { HELP = 256, VARIANT };
	static const struct option options[] = {
		{"help", no_argument, NULL, HELP},
		{"variant", required_argument, NULL, VARIANT},
		{NULL, 0, NULL, 0}
	};
	int ch, mode = 0;
	bool verify = false;

	while ((ch = getopt_long(argc, argv, "abnprvyV", options, NULL)) != -1) {
		int next = 0;
		switch (ch) {
		case 'n': next = 1; break;
		case 'b': next = 1; boot_only = 1; break;
		case 'a': case 'p': case 'y': next = 2; break;
		case 'r': next = 3; break;
		case 'v': break;
		case 'V': verify = true; break;
		case HELP: dos_help(); return 0;
		case VARIANT:
			if (!strcasecmp(optarg, "standard"))
				break;
			/* fall through */
		default:
			fprintf(stderr, "unsupported option; target not opened (use --help)\n");
			return 2;
		}
		if (next) {
			if (mode && mode != next) {
				fprintf(stderr, "conflicting check/repair modes\n");
				return 2;
			}
			mode = next;
		}
	}
	if (argc - optind != 1) {
		dos_help();
		return 2;
	}
	alwaysno = mode == 1;
	automatic = mode == 2;
	require_free_hint = 0;
	skipclean = 0;
	setcdevname(argv[optind], 0);
	int result = checkfilesys(argv[optind], true);
	if (verify && result == 0) {
		alwaysno = 1;
		automatic = false;
		result = checkfilesys(argv[optind], true);
	}
	return result != 0 || detected ? 1 : 0;
}

static void usage(void) __dead2;

static void
usage(void)
{

	fprintf(stderr, "%s\n%s\n",
	    "usage: fsck_msdosfs -p [-f] filesystem ...",
	    "       fsck_msdosfs [-ny] filesystem ...");
	exit(1);
}

int
bx_fsck_fat_main(int argc, char **argv)
{
	int ret = 0, erg;
	int ch;

	skipclean = 1;
	allow_mmap = 1;
	require_free_hint = 1;
	const char *name = strrchr(argv[0], '/');
	name = name ? name + 1 : argv[0];
	dos_mode = strcmp(name, "fsck_msdosfs") != 0;
	if (dos_mode)
		return dos_check(argc, argv);
	while ((ch = getopt(argc, argv, "BCfFnpyM")) != -1) {
		switch (ch) {
		case 'B': /* for fsck_ffs compatibility */
		case 'C':
			break;
		case 'f':
			skipclean = 0;
			break;
		case 'F':
			/*
			 * We can never run in the background.  We must exit
			 * silently with a nonzero exit code so that fsck(8)
			 * can probe our support for -F.  The exit code
			 * doesn't really matter, but we use an unusual one
			 * in case someone tries -F directly.  The -F flag
			 * is intentionally left out of the usage message.
			 */
			exit(5);
		case 'n':
			alwaysno = 1;
			alwaysyes = 0;
			break;
		case 'y':
			alwaysyes = 1;
			alwaysno = 0;
			break;

		case 'p':
			preen = 1;
			break;

		case 'M':
			allow_mmap = 0;
			break;

		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (!argc)
		usage();

	while (--argc >= 0) {
		setcdevname(*argv, preen);
		erg = checkfilesys(*argv++, false);
		if (erg > ret)
			ret = erg;
	}

	return ret;
}


/*VARARGS*/
int
ask(int def, const char *fmt, ...)
{
	va_list ap;

	char prompt[256];
	int c;

	if (dos_mode)
		detected = true;
	if (automatic && !rdonly && !alwaysno)
		return def;

	if (alwaysyes || alwaysno || rdonly)
		def = (alwaysyes && !rdonly && !alwaysno);

	if (preen) {
		if (def)
			printf("FIXED\n");
		return def;
	}

	va_start(ap, fmt);
	vsnprintf(prompt, sizeof(prompt), fmt, ap);
	va_end(ap);
	if (alwaysyes || alwaysno || rdonly) {
		printf("%s? %s\n", prompt, def ? "yes" : "no");
		return def;
	}
	do {
		printf("%s? [yn] ", prompt);
		fflush(stdout);
		c = getchar();
		while (c != '\n' && getchar() != '\n')
			if (feof(stdin))
				return 0;
	} while (c != 'y' && c != 'Y' && c != 'n' && c != 'N');
	return c == 'y' || c == 'Y';
}
