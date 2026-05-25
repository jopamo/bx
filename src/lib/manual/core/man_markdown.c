/* $OpenBSD$ */
/*
 * Copyright (c) 2026
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
 *
 * Markdown formatter for man(7) source used by mandoc(1).
 * This targets a bounded markdown subset rather than full CommonMark
 * or the full GitHub site post-processing pipeline.
 */
#include "config.h"

#include <sys/types.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mandoc_aux.h"
#include "libmandoc.h"
#include "roff.h"
#include "main.h"

enum md_font {
	MD_FONT_NONE = 0,
	MD_FONT_BOLD,
	MD_FONT_ITALIC
};

struct md_buf {
	char	*buf;
	size_t	 len;
	size_t	 cap;
};

struct md_state {
	char	*ip_prefix;
	char	*tp_tag;
	int	 tp_state;
	int	 in_code;
	int	 last_blank;
};

static	void	 md_buf_append(struct md_buf *, const char *, size_t);
static	void	 md_buf_append_char(struct md_buf *, char);
static	void	 md_buf_append_font(struct md_buf *, enum md_font);
static	char	*md_escape_table_cell(const char *);
static	void	 md_flush_table_row(char **, size_t);
static	char	*md_macro_line(const char *, const char *);
static	char	*md_next_arg(const char **);
static	int	 md_parse_tbl_format(const char *, char **, size_t *);
static	int	 md_parse_tbl_row(const char *, char ***, size_t *);
static	char	*md_prefix_for_ip(const char *);
static	char	*md_render_roff(const char *);
static	void	 md_state_flush_tp(struct md_state *);
static	void	 md_write_blank(struct md_state *);
static	void	 md_write_heading(struct md_state *, int, const char *);
static	void	 md_write_line(struct md_state *, const char *, const char *);
static	int	 md_write_table_block(struct md_state *, const struct buf **);


static void
md_buf_append(struct md_buf *buf, const char *data, size_t len)
{
	size_t	 need;

	need = buf->len + len + 1;
	if (need > buf->cap) {
		buf->cap = need * 2;
		buf->buf = mandoc_reallocarray(buf->buf, buf->cap,
		    sizeof(*buf->buf));
	}
	memcpy(buf->buf + buf->len, data, len);
	buf->len += len;
	buf->buf[buf->len] = '\0';
}

static void
md_buf_append_char(struct md_buf *buf, char ch)
{

	md_buf_append(buf, &ch, 1);
}

static void
md_buf_append_font(struct md_buf *buf, enum md_font font)
{
	static const char *const markers[] = {
		"",
		"**",
		"*"
	};

	md_buf_append(buf, markers[font], strlen(markers[font]));
}

static char *
md_next_arg(const char **cpp)
{
	struct md_buf	 buf;
	const char	*cp;
	char		 quote;

	cp = *cpp;
	while (*cp == ' ' || *cp == '\t')
		cp++;
	if (*cp == '\0') {
		*cpp = cp;
		return NULL;
	}

	memset(&buf, 0, sizeof(buf));
	quote = *cp == '"' ? *cp++ : '\0';
	while (*cp != '\0') {
		if (quote != '\0') {
			if (*cp == quote) {
				cp++;
				break;
			}
		} else if (*cp == ' ' || *cp == '\t')
			break;

		if (*cp == '\\' && cp[1] != '\0')
			cp++;
		md_buf_append_char(&buf, *cp++);
	}
	while (*cp == ' ' || *cp == '\t')
		cp++;
	*cpp = cp;

	if (buf.buf == NULL)
		return mandoc_strdup("");
	return buf.buf;
}

static char *
md_render_roff(const char *text)
{
	struct md_buf	 buf;
	enum md_font	 font, next_font;
	size_t		 i;

	memset(&buf, 0, sizeof(buf));
	font = MD_FONT_NONE;
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] == '\\') {
			switch (text[i + 1]) {
			case '&':
				i++;
				continue;
			case '-':
				md_buf_append_char(&buf, '-');
				i++;
				continue;
			case '\\':
				md_buf_append_char(&buf, '\\');
				i++;
				continue;
			case 'e':
				md_buf_append_char(&buf, '\\');
				i++;
				continue;
			case '(':
				if (text[i + 2] == 'b' && text[i + 3] == 'u') {
					md_buf_append_char(&buf, '*');
					i += 3;
					continue;
				}
				if (text[i + 2] == 'e' && text[i + 3] == 'm') {
					md_buf_append(&buf, "--", 2);
					i += 3;
					continue;
				}
				break;
			case 'f':
				next_font = font;
				if (text[i + 2] == 'B')
					next_font = MD_FONT_BOLD;
				else if (text[i + 2] == 'I')
					next_font = MD_FONT_ITALIC;
				else if (text[i + 2] == 'P' ||
				    text[i + 2] == 'R')
					next_font = MD_FONT_NONE;
				else if (text[i + 2] == '(' &&
				    text[i + 3] != '\0' && text[i + 4] != '\0') {
					next_font = text[i + 3] == 'B' ?
					    MD_FONT_BOLD : MD_FONT_NONE;
					i += 2;
				}
				if (next_font != font) {
					if (font != MD_FONT_NONE)
						md_buf_append_font(&buf, font);
					if (next_font != MD_FONT_NONE)
						md_buf_append_font(&buf,
						    next_font);
					font = next_font;
				}
				i += 2;
				continue;
			default:
				if (text[i + 1] != '\0') {
					i++;
					md_buf_append_char(&buf, text[i]);
					continue;
				}
				break;
			}
		}

		if (strchr("*_`", text[i]) != NULL)
			md_buf_append_char(&buf, '\\');
		md_buf_append_char(&buf, text[i]);
	}
	if (font != MD_FONT_NONE)
		md_buf_append_font(&buf, font);
	if (buf.buf == NULL)
		return mandoc_strdup("");
	return buf.buf;
}

static char *
md_prefix_for_ip(const char *tag)
{
	char	*text, *prefix;
	size_t	 len;

	if (strcmp(tag, "\\(bu") == 0 || strcmp(tag, "(bu") == 0 ||
	    strcmp(tag, "*") == 0)
		return mandoc_strdup("- ");
	text = md_render_roff(tag);
	len = strlen(text);
	if (len > 0 && isdigit((unsigned char)text[0]) &&
	    text[len - 1] == '.')
		prefix = mandoc_strdup(text);
	else {
		mandoc_asprintf(&prefix, "- %s:", text);
	}
	free(text);
	if (prefix[0] != '-' && prefix[0] != '*') {
		text = prefix;
		mandoc_asprintf(&prefix, "%s ", text);
		free(text);
		return prefix;
	}
	text = prefix;
	mandoc_asprintf(&prefix, "%s ", text);
	free(text);
	return prefix;
}

static char *
md_macro_line(const char *macro, const char *rest)
{
	struct md_buf	 buf;
	char		*arg;
	const char	*cp;
	int		 bold, italic;
	size_t		 arg_index;

	memset(&buf, 0, sizeof(buf));
	cp = rest;
	arg_index = 0;
	while ((arg = md_next_arg(&cp)) != NULL) {
		if (buf.len > 0)
			md_buf_append_char(&buf, ' ');
		bold = italic = 0;
		if (strcmp(macro, "B") == 0)
			bold = 1;
		else if (strcmp(macro, "I") == 0)
			italic = 1;
		else if (strcmp(macro, "BI") == 0 || strcmp(macro, "IB") == 0 ||
		    strcmp(macro, "BR") == 0 || strcmp(macro, "RB") == 0 ||
		    strcmp(macro, "IR") == 0 || strcmp(macro, "RI") == 0) {
			if ((strcmp(macro, "BI") == 0 ||
			     strcmp(macro, "BR") == 0) && arg_index % 2 == 0)
				bold = 1;
			else if ((strcmp(macro, "IB") == 0 ||
			     strcmp(macro, "IR") == 0) && arg_index % 2 == 0)
				italic = 1;
			else if ((strcmp(macro, "RB") == 0 ||
			     strcmp(macro, "RI") == 0) && arg_index % 2 == 1)
				bold = 1;
			else if ((strcmp(macro, "BR") == 0 ||
			     strcmp(macro, "IR") == 0) && arg_index % 2 == 1)
				italic = 1;
		}
		if (bold)
			md_buf_append(&buf, "**", 2);
		else if (italic)
			md_buf_append_char(&buf, '*');
		md_buf_append(&buf, arg, strlen(arg));
		if (bold)
			md_buf_append(&buf, "**", 2);
		else if (italic)
			md_buf_append_char(&buf, '*');
		arg_index++;
		free(arg);
	}
	if (buf.buf == NULL)
		return mandoc_strdup("");
	return buf.buf;
}

static void
md_write_blank(struct md_state *st)
{

	if (st->last_blank)
		return;
	putchar('\n');
	st->last_blank = 1;
}

static void
md_write_line(struct md_state *st, const char *prefix, const char *text)
{

	if (prefix != NULL)
		fputs(prefix, stdout);
	if (text != NULL)
		fputs(text, stdout);
	putchar('\n');
	st->last_blank = 0;
}

static void
md_write_heading(struct md_state *st, int level, const char *text)
{
	const char	*prefix;

	md_state_flush_tp(st);
	md_write_blank(st);
	prefix = level > 2 ? "### " : "## ";
	md_write_line(st, prefix, text);
	md_write_blank(st);
}

static void
md_state_flush_tp(struct md_state *st)
{

	if (st->tp_state == 2 && st->tp_tag != NULL) {
		printf("- **%s**\n", st->tp_tag);
		st->last_blank = 0;
	}
	free(st->tp_tag);
	st->tp_tag = NULL;
	st->tp_state = 0;
}

static char *
md_escape_table_cell(const char *text)
{
	struct md_buf	 buf;
	size_t		 i;

	memset(&buf, 0, sizeof(buf));
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] == '\\' || text[i] == '|')
			md_buf_append_char(&buf, '\\');
		md_buf_append_char(&buf, text[i]);
	}
	if (buf.buf == NULL)
		return mandoc_strdup("");
	return buf.buf;
}

static void
md_flush_table_row(char **cells, size_t count)
{
	size_t	 i;

	for (i = 0; i < count; i++)
		free(cells[i]);
	free(cells);
}

static int
md_parse_tbl_format(const char *line, char **aligns, size_t *count)
{
	char	*parsed;
	size_t	 i, n;

	n = 0;
	for (i = 0; line[i] != '\0' && line[i] != '.'; i++)
		if (line[i] == 'l' || line[i] == 'c' || line[i] == 'r')
			n++;
	if (line[i] != '.' || n == 0)
		return 0;
	parsed = mandoc_malloc(n + 1);
	n = 0;
	for (i = 0; line[i] != '.'; i++)
		if (line[i] == 'l' || line[i] == 'c' || line[i] == 'r')
			parsed[n++] = line[i];
	parsed[n] = '\0';
	*aligns = parsed;
	*count = n;
	return 1;
}

static int
md_parse_tbl_row(const char *line, char ***cells, size_t *count)
{
	char		**row;
	char		*cell;
	const char	*cp, *tab;
	size_t		 n;

	row = NULL;
	n = 0;
	cp = line;
	for (;;) {
		tab = strchr(cp, '\t');
		if (tab == NULL)
			cell = mandoc_strdup(cp);
		else
			cell = mandoc_strndup(cp, (size_t)(tab - cp));
		row = mandoc_reallocarray(row, n + 1, sizeof(*row));
		row[n++] = cell;
		if (tab == NULL)
			break;
		cp = tab + 1;
	}
	*cells = row;
	*count = n;
	return 1;
}

static int
md_write_table_block(struct md_state *st, const struct buf **curp)
{
	const struct buf	*cur;
	char			*aligns, **header, **row, *rendered, *escaped;
	size_t			 ncol, count, i;
	int			 parsed;

	cur = (*curp)->next;
	aligns = NULL;
	ncol = 0;
	parsed = 0;
	while (cur != NULL) {
		if (strcmp(cur->buf, ".TE") == 0)
			break;
		if (md_parse_tbl_format(cur->buf, &aligns, &ncol)) {
			parsed = 1;
			break;
		}
		cur = cur->next;
	}
	if (!parsed || cur == NULL) {
		while (cur != NULL && strcmp(cur->buf, ".TE") != 0)
			cur = cur->next;
		*curp = cur == NULL ? *curp : cur;
		free(aligns);
		return 0;
	}

	cur = cur->next;
	if (cur == NULL || strcmp(cur->buf, ".TE") == 0) {
		*curp = cur == NULL ? *curp : cur;
		free(aligns);
		return 0;
	}
	md_parse_tbl_row(cur->buf, &header, &count);
	if (count < ncol) {
		header = mandoc_reallocarray(header, ncol, sizeof(*header));
		for (i = count; i < ncol; i++)
			header[i] = mandoc_strdup("");
	} else
		ncol = count;
	cur = cur->next;
	if (cur != NULL && strcmp(cur->buf, "_") == 0)
		cur = cur->next;

	md_write_blank(st);
	fputs("|", stdout);
	for (i = 0; i < ncol; i++) {
		rendered = md_render_roff(header[i]);
		escaped = md_escape_table_cell(rendered);
		printf(" %s |", escaped);
		free(rendered);
		free(escaped);
	}
	putchar('\n');
	fputs("|", stdout);
	for (i = 0; i < ncol; i++)
		printf(" %s |", aligns[i] == 'c' ? ":---:" :
		    aligns[i] == 'r' ? "---:" : ":---");
	putchar('\n');
	st->last_blank = 0;

	while (cur != NULL && strcmp(cur->buf, ".TE") != 0) {
		md_parse_tbl_row(cur->buf, &row, &count);
		if (count < ncol) {
			row = mandoc_reallocarray(row, ncol, sizeof(*row));
			for (i = count; i < ncol; i++)
				row[i] = mandoc_strdup("");
		}
		fputs("|", stdout);
		for (i = 0; i < ncol; i++) {
			rendered = md_render_roff(row[i]);
			escaped = md_escape_table_cell(rendered);
			printf(" %s |", escaped);
			free(rendered);
			free(escaped);
		}
		putchar('\n');
		st->last_blank = 0;
		md_flush_table_row(row, count < ncol ? ncol : count);
		cur = cur->next;
	}
	md_write_blank(st);
	md_flush_table_row(header, ncol);
	free(aligns);
	*curp = cur == NULL ? *curp : cur;
	return cur != NULL;
}

void
markdown_man(const struct roff_meta *meta, const struct buf *lines)
{
	struct md_state	 st;
	const struct buf	*cur;
	char			*line, *rendered, *tag;
	const char		*cp;
	size_t			 len;
	int			 header_done;

	memset(&st, 0, sizeof(st));
	st.last_blank = 1;
	header_done = 0;
	cur = lines;

	while (cur != NULL) {
		cp = cur->buf;
		if (st.in_code) {
			if (strncmp(cp, ".EE", 3) == 0 &&
			    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
				md_write_line(&st, NULL, "```");
				md_write_blank(&st);
				st.in_code = 0;
				cur = cur->next;
				continue;
			}
			md_write_line(&st, NULL, cp);
			cur = cur->next;
			continue;
		}

		if (strncmp(cp, ".TH", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			md_state_flush_tp(&st);
			if (meta->title != NULL && meta->msec != NULL) {
				printf("# %s(%s)\n\n", meta->title, meta->msec);
				st.last_blank = 1;
				header_done = 1;
			}
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".SH", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			cp += 3;
			while (*cp == ' ' || *cp == '\t')
				cp++;
			line = md_next_arg(&cp);
			md_write_heading(&st, 2, line);
			free(line);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".SS", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			cp += 3;
			while (*cp == ' ' || *cp == '\t')
				cp++;
			line = md_next_arg(&cp);
			md_write_heading(&st, 3, line);
			free(line);
			cur = cur->next;
			continue;
		}
		if (strcmp(cp, ".PP") == 0 || strcmp(cp, ".P") == 0 ||
		    strcmp(cp, ".LP") == 0) {
			md_state_flush_tp(&st);
			md_write_blank(&st);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".EX", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			md_state_flush_tp(&st);
			md_write_blank(&st);
			md_write_line(&st, NULL, "```");
			st.in_code = 1;
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".TS", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			md_state_flush_tp(&st);
			md_write_table_block(&st, &cur);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".IP", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			md_state_flush_tp(&st);
			cp += 3;
			tag = md_next_arg(&cp);
			free(st.ip_prefix);
			st.ip_prefix = md_prefix_for_ip(tag);
			free(tag);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".TP", 3) == 0 &&
		    (cp[3] == '\0' || isspace((unsigned char)cp[3]))) {
			md_state_flush_tp(&st);
			st.tp_state = 1;
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".RS", 3) == 0 || strncmp(cp, ".RE", 3) == 0) {
			md_state_flush_tp(&st);
			md_write_blank(&st);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".B", 2) == 0 &&
		    (cp[2] == '\0' || isspace((unsigned char)cp[2]))) {
			rendered = md_macro_line("B", cp + 2);
			md_write_line(&st, NULL, rendered);
			free(rendered);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".I", 2) == 0 &&
		    (cp[2] == '\0' || isspace((unsigned char)cp[2]))) {
			rendered = md_macro_line("I", cp + 2);
			md_write_line(&st, NULL, rendered);
			free(rendered);
			cur = cur->next;
			continue;
		}
		if (strncmp(cp, ".BI", 3) == 0 || strncmp(cp, ".IB", 3) == 0 ||
		    strncmp(cp, ".BR", 3) == 0 || strncmp(cp, ".RB", 3) == 0 ||
		    strncmp(cp, ".IR", 3) == 0 || strncmp(cp, ".RI", 3) == 0) {
			line = mandoc_strdup(cp + 1);
			line[2] = '\0';
			rendered = md_macro_line(line, cp + 3);
			md_write_line(&st, NULL, rendered);
			free(rendered);
			free(line);
			cur = cur->next;
			continue;
		}
		if (cp[0] == '.' && cp[1] != '\0' &&
		    isalpha((unsigned char)cp[1])) {
			cur = cur->next;
			continue;
		}

		rendered = md_render_roff(cp);
		len = strlen(rendered);
		if (len == 0) {
			free(rendered);
			md_write_blank(&st);
			cur = cur->next;
			continue;
		}
		if (!header_done && meta->title != NULL && meta->msec != NULL) {
			printf("# %s(%s)\n\n", meta->title, meta->msec);
			st.last_blank = 1;
			header_done = 1;
		}
		if (st.tp_state == 1) {
			free(st.tp_tag);
			st.tp_tag = rendered;
			st.tp_state = 2;
			cur = cur->next;
			continue;
		}
		if (st.tp_state == 2) {
			md_write_blank(&st);
			printf("- **%s**: %s\n", st.tp_tag, rendered);
			st.last_blank = 0;
			free(st.tp_tag);
			st.tp_tag = NULL;
			st.tp_state = 0;
			free(rendered);
			cur = cur->next;
			continue;
		}
		if (st.ip_prefix != NULL) {
			md_write_line(&st, st.ip_prefix, rendered);
			free(st.ip_prefix);
			st.ip_prefix = NULL;
		} else
			md_write_line(&st, NULL, rendered);
		free(rendered);
		cur = cur->next;
	}

	md_state_flush_tp(&st);
	if (st.in_code)
		md_write_line(&st, NULL, "```");
	free(st.ip_prefix);
	free(st.tp_tag);
}
