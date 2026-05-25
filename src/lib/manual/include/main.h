/*	$Id: main.h,v 1.30 2019/03/03 13:02:11 schwarze Exp $ */
/*
 * Copyright (c) 2009, 2010, 2011 Kristaps Dzonsons <kristaps@bsd.lv>
 * Copyright (c) 2014, 2015, 2019 Ingo Schwarze <schwarze@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHORS DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

struct	roff_meta;
struct	manoutput;
struct	buf;

enum	form {
	FORM_SRC = 1,	/* Format is mdoc(7) or man(7). */
	FORM_CAT,	/* Manual page is preformatted. */
	FORM_NONE	/* Format is unknown. */
};

enum	argmode {
	ARG_FILE = 0,
	ARG_NAME
};

struct	manpage {
	char		*file; /* to be prefixed by manpath */
	char		*names; /* a list of names with sections */
	char		*output; /* user-defined additional output */
	unsigned long long bits; /* name type mask */
	size_t		 ipath; /* number of the manpath */
	int		 sec; /* section number, 10 means invalid */
	enum form	 form;
};

struct	mansearch {
	const char	*arch; /* architecture/NULL */
	const char	*sec; /* mansection/NULL */
	enum argmode	 argmode; /* interpretation of arguments */
	int		 firstmatch; /* stop after the first match */
};

#define	NAME_FILE	 0x0000004000000010ULL
#define	NAME_MASK	 0x000000000000001fULL

/*
 * Definitions for main.c-visible output device functions.
 * ascii_alloc() is named as such in anticipation of latin1_alloc()
 * and so on, all of which map into the terminal output routines with
 * different character settings.
 */

void		  man_mdoc(void *, const struct roff_meta *);

void		 *locale_alloc(const struct manoutput *);
void		 *utf8_alloc(const struct manoutput *);
void		 *ascii_alloc(const struct manoutput *);
void		  ascii_free(void *);

void		  terminal_mdoc(void *, const struct roff_meta *);
void		  terminal_man(void *, const struct roff_meta *);
void		  terminal_sepline(void *);

void		  markdown_man(const struct roff_meta *, const struct buf *);
void		  markdown_mdoc(void *, const struct roff_meta *);
char		 *man_from_markdown(const char *, size_t);
