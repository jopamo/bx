/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2013 Johann 'Myrkraverk' Oskarsson.
 * Copyright (c) 1992 Diomidis Spinellis.
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Diomidis Spinellis of Imperial College, University of London.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dispatch/applets.h"
#include "lib/option_scan.h"
#include "defs.h"
#include "extern.h"

/*
 * Linked list of units (strings and files) to be compiled
 */
struct s_compunit {
	struct s_compunit *next;
	enum e_cut {CU_FILE, CU_STRING} type;
	char *s;			/* Pointer to string or fname */
};

/*
 * Linked list pointer to compilation units and pointer to current
 * next pointer.
 */
static struct s_compunit *script, **cu_nextp = &script;

/*
 * Linked list of files to be processed
 */
struct s_flist {
	char *fname;
	struct s_flist *next;
};

/*
 * Linked list pointer to files and pointer to current
 * next pointer.
 */
static struct s_flist *files, **fl_nextp = &files;

FILE *infile;			/* Current input file */
FILE *outfile;			/* Current output file */

int aflag, nflag;
int rflags = 0;
int quit = 0;
static int rval;		/* Exit status */

static int ispan;		/* Whether inplace editing spans across files */

/*
 * Current file and line number; line numbers restart across compilation
 * units, but span across input files.  The latter is optional if editing
 * in place.
 */
const char *fname;		/* File name. */
const char *outfname;		/* Output file name */
static char oldfname[PATH_MAX];	/* Old file name (for in-place editing) */
static char tmpfname[PATH_MAX];	/* Temporary file name (for in-place editing) */
const char *inplace;		/* Inplace edit file extension. */
u_long linenum;

static void add_compunit(enum e_cut, char *);
static void add_file(char *);
/* Set only after mkstemp succeeds. */
static int have_tmp;
static void discard_tmp(void)
{
	if (have_tmp)
		unlink(tmpfname);
}

static void usage(void) __attribute__((noreturn));

int
bx_sed_main(int argc, char *argv[])
{
	static const struct bx_option_spec specs[] = {
		{ 'E', 'E', NULL, BX_OPTION_NO_VALUE },
		{ 'I', 'I', NULL, BX_OPTION_REQUIRED_VALUE },
		{ 'a', 'a', NULL, BX_OPTION_NO_VALUE },
		{ 'e', 'e', NULL, BX_OPTION_REQUIRED_VALUE },
		{ 'f', 'f', NULL, BX_OPTION_REQUIRED_VALUE },
		{ 'i', 'i', NULL, BX_OPTION_OPTIONAL_VALUE },
		{ 'l', 'l', NULL, BX_OPTION_NO_VALUE },
		{ 'n', 'n', NULL, BX_OPTION_NO_VALUE },
		{ 'r', 'r', NULL, BX_OPTION_NO_VALUE },
		{ 'u', 'u', NULL, BX_OPTION_NO_VALUE },
	};
	int fflagstdin = 0, buffering = 0;
	char *temp_arg;

	(void) setlocale(LC_ALL, "");

	if (atexit(discard_tmp) != 0)
		errx(1, "cannot register temporary file cleanup");
	inplace = NULL;

	char **operands = calloc((size_t)argc, sizeof(*operands));
	if (operands == NULL)
		err(1, "calloc");
	size_t count = 0, skip_empty = 0;
	struct bx_option_cursor cursor;
	bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED,
	    specs, sizeof(specs) / sizeof(specs[0]));
	struct bx_option_result option;
	while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
		if (option.event == BX_OPTION_OPERAND) {
			if (option.index != skip_empty)
				operands[count++] = argv[option.index];
			continue;
		}
		if (option.event != BX_OPTION_MATCH) {
			if (option.event == BX_OPTION_MISSING_VALUE)
				warnx("option requires an argument -- '%c'", option.short_name);
			else if (option.short_name)
				warnx("invalid option -- '%c'", option.short_name);
			else
				warnx("unrecognized option '%s'", option.token);
			usage();
		}
		switch (option.spec->id) {
		case 'r':		/* Gnu sed compat */
		case 'E':
			rflags = REG_EXTENDED;
			break;
		case 'I':
			inplace = option.value;
			ispan = 1;	/* span across input files */
			break;
		case 'a':
			aflag = 1;
			break;
		case 'e':
			if (asprintf(&temp_arg, "%s\n", option.value) == -1)
				err(1, "asprintf");
			add_compunit(CU_STRING, temp_arg);
			break;
		case 'f':
			if (strcmp(option.value, "-") == 0)
				fflagstdin = 1;
			add_compunit(CU_FILE, option.value);
			break;
		case 'i':
			/* GNU attached suffix; also accept BSD's explicit empty suffix. */
			if (option.value == NULL && option.index + 1 < (size_t)argc &&
			    argv[option.index + 1][0] == '\0')
				skip_empty = option.index + 1;
			inplace = option.value ? option.value : "";
			ispan = 0;	/* don't span across input files */
			break;
		case 'l':
			buffering = 'l';
			break;
		case 'n':
			nflag = 1;
			break;
		case 'u':
			buffering = 'u';
			break;
		default:
			usage();
		}
	}

	size_t first_file = 0;
	if (script == NULL && count != 0) {
		if (asprintf(&temp_arg, "%s\n", operands[0]) == -1)
			err(1, "asprintf");
		add_compunit(CU_STRING, temp_arg);
		first_file = 1;
	}

	if (script == NULL)
		usage();
	if (inplace != NULL) {
		if (strchr(inplace, '/') != NULL)
			errx(1, "backup suffix must not contain '/'");
		if (first_file == count && !fflagstdin)
			errx(1, "-I or -i may not be used with stdin");
		for (size_t i = first_file; i < count; i++)
			if (operands[i][0] == '\0')
				errx(1, "empty in-place input file name");
	}
	if (buffering == 'l' && setvbuf(stdout, NULL, _IOLBF, 0) != 0)
		warnx("setting line buffered output failed");
	if (buffering == 'u' && setvbuf(stdout, NULL, _IONBF, 0) != 0)
		warnx("setting unbuffered output failed");
	compile();

	if (first_file != count)
		for (size_t i = first_file; i < count; i++)
			add_file(operands[i]);
	else if (fflagstdin)
		exit(rval);
	else
		add_file(NULL);
	free(operands);
	process();
	cfclose(prog, NULL);
	if (fclose(stdout))
		err(1, "stdout");
	exit(rval);
}

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s script [-Ealnru] [-i[extension]] [file ...]\n"
	    "\t%s [-Ealnu] [-i[extension]] [-e script] ... [-f script_file]"
	    " ... [file ...]\n", "sed", "sed");
	exit(1);
}

/*
 * Like fgets, but go through the chain of compilation units chaining them
 * together.  Empty strings and files are ignored.
 */
char *
cu_fgets(char *buf, int n, int *more)
{
	static enum {ST_EOF, ST_FILE, ST_STRING} state = ST_EOF;
	static FILE *f;		/* Current open file */
	static char *s;		/* Current pointer inside string */
	static char string_ident[30];
	char *p;

again:
	switch (state) {
	case ST_EOF:
		if (script == NULL) {
			if (more != NULL)
				*more = 0;
			return (NULL);
		}
		linenum = 0;
		switch (script->type) {
		case CU_FILE:
			if (strcmp(script->s, "-") == 0) {
				f = stdin;
				fname = "stdin";
			} else {
				if ((f = fopen(script->s, "r")) == NULL)
				        err(1, "%s", script->s);
				fname = script->s;
			}
			state = ST_FILE;
			goto again;
		case CU_STRING:
			if (((size_t)snprintf(string_ident,
			    sizeof(string_ident), "\"%s\"", script->s)) >=
			    sizeof(string_ident) - 1)
				(void)strcpy(string_ident +
				    sizeof(string_ident) - 6, " ...\"");
			fname = string_ident;
			s = script->s;
			state = ST_STRING;
			goto again;
		default:
			abort();
		}
	case ST_FILE:
		if ((p = fgets(buf, n, f)) != NULL) {
			linenum++;
			if (linenum == 1 && buf[0] == '#' && buf[1] == 'n')
				nflag = 1;
			if (more != NULL)
				*more = !feof(f);
			return (p);
		}
		script = script->next;
		(void)fclose(f);
		state = ST_EOF;
		goto again;
	case ST_STRING:
		if (linenum == 0 && s[0] == '#' && s[1] == 'n')
			nflag = 1;
		p = buf;
		for (;;) {
			if (n-- <= 1) {
				*p = '\0';
				linenum++;
				if (more != NULL)
					*more = 1;
				return (buf);
			}
			switch (*s) {
			case '\0':
				state = ST_EOF;
				if (s == script->s) {
					script = script->next;
					goto again;
				} else {
					script = script->next;
					*p = '\0';
					linenum++;
					if (more != NULL)
						*more = 0;
					return (buf);
				}
			case '\n':
				*p++ = '\n';
				*p = '\0';
				s++;
				linenum++;
				if (more != NULL)
					*more = 0;
				return (buf);
			default:
				*p++ = *s++;
			}
		}
	}
	/* NOTREACHED */
	return (NULL);
}

/*
 * Like fgets, but go through the list of files chaining them together.
 * Set len to the length of the line.
 */
int
mf_fgets(SPACE *sp, enum e_spflag spflag)
{
	static struct stat sb;
	struct stat now;
	ssize_t len;
	char *dirbuf;
	static char *p = NULL;
	static size_t plen = 0;
	int c;
	static int firstfile;

	if (infile == NULL) {
		/* stdin? */
		if (files->fname == NULL) {
			infile = stdin;
			fname = "stdin";
			outfile = stdout;
			outfname = "stdout";
		}
		firstfile = 1;
	}

	for (;;) {
		if (infile != NULL && (c = getc(infile)) != EOF && !quit) {
			(void)ungetc(c, infile);
			break;
		}
		/* If we are here then either eof or no files are open yet */
		if (infile != NULL && ferror(infile))
			err(1, "%s", fname);
		if (infile == stdin) {
			sp->len = 0;
			return (0);
		}
		if (infile != NULL) {
			fclose(infile);
			if (have_tmp) {
				if (lstat(fname, &now) != 0 || now.st_dev != sb.st_dev || now.st_ino != sb.st_ino)
					errx(1, "%s: file changed during edit", fname);
				if (fflush(outfile) != 0)
					err(1, "%s", tmpfname);
				if (fchown(fileno(outfile), sb.st_uid, sb.st_gid) != 0 ||
				    fchmod(fileno(outfile), sb.st_mode & 0777) != 0)
					err(1, "%s", tmpfname);
				if (fclose(outfile) != 0)
					err(1, "%s", tmpfname);
				outfile = NULL;
				if (*oldfname != '\0') {
					if (unlink(oldfname) != 0 && errno != ENOENT)
						err(1, "%s", oldfname);
					if (link(fname, oldfname) != 0)
						err(1, "%s", oldfname);
				}
				if (rename(tmpfname, fname) != 0)
					err(1, "%s", fname);
				have_tmp = 0;
			}
			outfname = NULL;
		}
		if (firstfile == 0)
			files = files->next;
		else
			firstfile = 0;
		if (files == NULL || quit) {
			sp->len = 0;
			return (0);
		}
		fname = files->fname;
		if (inplace != NULL) {
			int fd = open(fname, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
			if (fd < 0 || fstat(fd, &sb) != 0)
				err(1, "%s", fname);
			if (!S_ISREG(sb.st_mode))
				errx(1, "%s: in-place editing requires a regular file", fname);
			if ((infile = fdopen(fd, "r")) == NULL)
				err(1, "%s", fname);
			if (*inplace != '\0') {
				len = snprintf(oldfname, sizeof(oldfname), "%s%s", fname, inplace);
				if (len < 0 || len >= (ssize_t)sizeof(oldfname))
					errx(1, "%s: backup name too long", fname);
			}
			if ((dirbuf = strdup(fname)) == NULL)
				err(1, "strdup");
			len = snprintf(tmpfname, sizeof(tmpfname), "%s/.bx-sed.XXXXXX", dirname(dirbuf));
			free(dirbuf);
			if (len < 0 || len >= (ssize_t)sizeof(tmpfname))
				errx(1, "%s: temporary name too long", fname);
			if ((fd = mkstemp(tmpfname)) < 0)
				err(1, "%s", fname);
			have_tmp = 1;
			if ((outfile = fdopen(fd, "w")) == NULL)
				err(1, "%s", tmpfname);
			outfname = tmpfname;
			if (!ispan) {
				linenum = 0;
				resetstate();
			}
		} else {
			outfile = stdout;
			outfname = "stdout";
		}
		if (inplace == NULL && (infile = fopen(fname, "r")) == NULL) {
			warn("%s", fname);
			rval = 1;
			continue;
		}
	}
	/*
	 * We are here only when infile is open and we still have something
	 * to read from it.
	 *
	 * Use getline() so that we can handle essentially infinite input
	 * data.  The p and plen are static so each invocation gives
	 * getline() the same buffer which is expanded as needed.
	 */
	len = getline(&p, &plen, infile);
	if (len == -1)
		err(1, "%s", fname);
	if (len != 0 && p[len - 1] == '\n') {
		sp->append_newline = 1;
		len--;
	} else if (!lastline()) {
		sp->append_newline = 1;
	} else {
		sp->append_newline = 0;
	}
	cspace(sp, p, len, spflag);

	linenum++;

	return (1);
}

/*
 * Add a compilation unit to the linked list
 */
static void
add_compunit(enum e_cut type, char *s)
{
	struct s_compunit *cu;

	if ((cu = malloc(sizeof(struct s_compunit))) == NULL)
		err(1, "malloc");
	cu->type = type;
	cu->s = s;
	cu->next = NULL;
	*cu_nextp = cu;
	cu_nextp = &cu->next;
}

/*
 * Add a file to the linked list
 */
static void
add_file(char *s)
{
	struct s_flist *fp;

	if ((fp = malloc(sizeof(struct s_flist))) == NULL)
		err(1, "malloc");
	fp->next = NULL;
	*fl_nextp = fp;
	fp->fname = s;
	fl_nextp = &fp->next;
}

static int
next_files_have_lines(void)
{
	struct s_flist *file;
	FILE *file_fd;
	int ch;

	file = files;
	while ((file = file->next) != NULL) {
		if ((file_fd = fopen(file->fname, "r")) == NULL)
			continue;

		if ((ch = getc(file_fd)) != EOF) {
			/*
			 * This next file has content, therefore current
			 * file doesn't contains the last line.
			 */
			ungetc(ch, file_fd);
			fclose(file_fd);
			return (1);
		}

		fclose(file_fd);
	}

	return (0);
}

int
lastline(void)
{
	int ch;

	if (feof(infile)) {
		return !(
		    (inplace == NULL || ispan) &&
		    next_files_have_lines());
	}
	if ((ch = getc(infile)) == EOF) {
		return !(
		    (inplace == NULL || ispan) &&
		    next_files_have_lines());
	}
	ungetc(ch, infile);
	return (0);
}
