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
 * Lower a bounded markdown subset to man(7) source.
 * This stays intentionally bounded and targets manual-page conversion,
 * not full CommonMark or full GitHub rendering behavior.
 */
#include "config.h"

#include <sys/types.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mandoc_aux.h"
#include "main.h"

enum mk_font {
	MK_FONT_NONE = 0,
	MK_FONT_BOLD,
	MK_FONT_ITALIC,
	MK_FONT_CODE
};

struct mk_buf {
	char	*buf;
	size_t	 len;
	size_t	 cap;
};

struct mk_title {
	char	*title;
	char	*sec;
};

struct mk_lines {
	char	*storage;
	char	**lines;
	size_t	 count;
};

struct mk_cells {
	char	**v;
	size_t	 n;
};

static	void	 mk_append(struct mk_buf *, const char *, size_t);
static	void	 mk_append_char(struct mk_buf *, char);
static	void	 mk_append_cstr(struct mk_buf *, const char *);
static	void	 mk_cells_append(struct mk_cells *, char *);
static	void	 mk_cells_free(struct mk_cells *);
static	void	 mk_emit_font(struct mk_buf *, enum mk_font);
static	void	 mk_emit_heading(struct mk_buf *, const char *, const char *);
static	void	 mk_emit_inline(struct mk_buf *, const char *);
static	void	 mk_emit_macro(struct mk_buf *, const char *);
static	void	 mk_emit_raw_line(struct mk_buf *, const char *);
static	void	 mk_emit_table(struct mk_buf *, const struct mk_cells *,
			   const char *, struct mk_cells *, size_t);
static	void	 mk_emit_text_line(struct mk_buf *, const char *);
static	void	 mk_free_lines(struct mk_lines *);
static	int	 mk_is_autolink_target(const char *);
static	int	 mk_is_blank(const char *);
static	int	 mk_is_fence(const char *, char *, size_t *);
static	int	 mk_is_ordered_item(const char *, const char **);
static	int	 mk_is_rule(const char *);
static	int	 mk_is_setext_underline(const char *);
static	int	 mk_parse_atx_heading(const char *, size_t *, char **);
static	int	 mk_parse_blockquote(const char *, const char **);
static	int	 mk_parse_bullet_item(const char *, const char **);
static	int	 mk_parse_pipe_cells(const char *, struct mk_cells *);
static	int	 mk_parse_table_align(const struct mk_cells *, char **);
static	int	 mk_parse_title(const char *, struct mk_title *);
static	void	 mk_split_lines(const char *, size_t, struct mk_lines *);
static	char	*mk_slice_trim(const char *, size_t);
static	char	*mk_strip_hash_closer(const char *);
static	const char *mk_trim_left(const char *);


static void
mk_append(struct mk_buf *buf, const char *data, size_t len)
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
mk_append_char(struct mk_buf *buf, char ch)
{

	mk_append(buf, &ch, 1);
}

static void
mk_append_cstr(struct mk_buf *buf, const char *str)
{

	mk_append(buf, str, strlen(str));
}

static void
mk_cells_append(struct mk_cells *cells, char *cell)
{

	cells->v = mandoc_reallocarray(cells->v, cells->n + 1,
	    sizeof(*cells->v));
	cells->v[cells->n++] = cell;
}

static void
mk_cells_free(struct mk_cells *cells)
{
	size_t	 i;

	for (i = 0; i < cells->n; i++)
		free(cells->v[i]);
	free(cells->v);
	cells->v = NULL;
	cells->n = 0;
}

static int
mk_is_blank(const char *line)
{

	while (*line == ' ' || *line == '\t')
		line++;
	return *line == '\0';
}

static const char *
mk_trim_left(const char *line)
{

	while (*line == ' ' || *line == '\t')
		line++;
	return line;
}

static char *
mk_slice_trim(const char *line, size_t len)
{
	size_t	 start, end;

	start = 0;
	end = len;
	while (start < len &&
	    (line[start] == ' ' || line[start] == '\t'))
		start++;
	while (end > start &&
	    (line[end - 1] == ' ' || line[end - 1] == '\t'))
		end--;
	return mandoc_strndup(line + start, end - start);
}

static void
mk_emit_font(struct mk_buf *buf, enum mk_font font)
{
	static const char *const fonts[] = {
		"\\fP",
		"\\fB",
		"\\fI",
		"\\fB"
	};

	mk_append_cstr(buf, fonts[font]);
}

static int
mk_is_autolink_target(const char *text)
{
	size_t	 i;
	int	 saw_at;

	if (strncmp(text, "http://", 7) == 0 ||
	    strncmp(text, "https://", 8) == 0 ||
	    strncmp(text, "mailto:", 7) == 0)
		return 1;
	saw_at = 0;
	for (i = 0; text[i] != '\0'; i++) {
		if (isspace((unsigned char)text[i]))
			return 0;
		if (text[i] == '@')
			saw_at = 1;
	}
	return saw_at;
}

static char *
mk_strip_hash_closer(const char *text)
{
	size_t	 len;

	len = strlen(text);
	while (len > 0 && isspace((unsigned char)text[len - 1]))
		len--;
	while (len > 0 && text[len - 1] == '#')
		len--;
	if (len > 0 && isspace((unsigned char)text[len - 1]))
		return mk_slice_trim(text, len);
	return mandoc_strdup(text);
}

static void
mk_emit_inline(struct mk_buf *buf, const char *text)
{
	enum mk_font	 font, next_font;
	char		*label, *link, *clean;
	const char	*cp, *end, *link_end;

	font = MK_FONT_NONE;
	for (cp = text; *cp != '\0'; cp++) {
		if (cp == text && (*cp == '.' || *cp == '\''))
			mk_append_cstr(buf, "\\&");

		if (cp[0] == '!' && cp[1] == '[' &&
		    (end = strchr(cp + 2, ']')) != NULL &&
		    end[1] == '(' && (link_end = strchr(end + 2, ')')) != NULL) {
			label = mk_slice_trim(cp + 2, (size_t)(end - (cp + 2)));
			link = mk_slice_trim(end + 2,
			    (size_t)(link_end - (end + 2)));
			if (*label != '\0')
				mk_append(buf, label, strlen(label));
			if (*link != '\0') {
				if (*label != '\0')
					mk_append_char(buf, ' ');
				mk_append_cstr(buf, "<");
				mk_append(buf, link, strlen(link));
				mk_append_cstr(buf, ">");
			}
			free(label);
			free(link);
			cp = link_end;
			continue;
		}
		if (cp[0] == '[' && (end = strchr(cp, ']')) != NULL &&
		    end[1] == '(' && (link_end = strchr(end + 2, ')')) != NULL) {
			label = mk_slice_trim(cp + 1, (size_t)(end - (cp + 1)));
			link = mk_slice_trim(end + 2,
			    (size_t)(link_end - (end + 2)));
			if (*label != '\0')
				mk_append(buf, label, strlen(label));
			if (*link != '\0') {
				if (*label != '\0')
					mk_append_char(buf, ' ');
				mk_append_cstr(buf, "<");
				mk_append(buf, link, strlen(link));
				mk_append_cstr(buf, ">");
			}
			free(label);
			free(link);
			cp = link_end;
			continue;
		}
		if (*cp == '<' && (end = strchr(cp + 1, '>')) != NULL) {
			clean = mk_slice_trim(cp + 1, (size_t)(end - (cp + 1)));
			if (mk_is_autolink_target(clean)) {
				mk_append(buf, clean, strlen(clean));
				free(clean);
				cp = end;
				continue;
			}
			free(clean);
		}
		if (cp[0] == '~' && cp[1] == '~') {
			cp++;
			continue;
		}
		if ((cp[0] == '*' && cp[1] == '*') ||
		    (cp[0] == '_' && cp[1] == '_')) {
			next_font = font == MK_FONT_BOLD ?
			    MK_FONT_NONE : MK_FONT_BOLD;
			if (font != MK_FONT_NONE)
				mk_emit_font(buf, MK_FONT_NONE);
			if (next_font != MK_FONT_NONE)
				mk_emit_font(buf, next_font);
			font = next_font;
			cp++;
			continue;
		}
		if (*cp == '*' || *cp == '_') {
			next_font = font == MK_FONT_ITALIC ?
			    MK_FONT_NONE : MK_FONT_ITALIC;
			if (font != MK_FONT_NONE)
				mk_emit_font(buf, MK_FONT_NONE);
			if (next_font != MK_FONT_NONE)
				mk_emit_font(buf, next_font);
			font = next_font;
			continue;
		}
		if (*cp == '`') {
			next_font = font == MK_FONT_CODE ?
			    MK_FONT_NONE : MK_FONT_CODE;
			if (font != MK_FONT_NONE)
				mk_emit_font(buf, MK_FONT_NONE);
			if (next_font != MK_FONT_NONE)
				mk_emit_font(buf, next_font);
			font = next_font;
			continue;
		}
		if (*cp == '\\' && cp[1] != '\0') {
			cp++;
			mk_append_char(buf, *cp);
			continue;
		}
		if (*cp == '\\')
			mk_append_char(buf, '\\');
		mk_append_char(buf, *cp);
	}
	if (font != MK_FONT_NONE)
		mk_emit_font(buf, MK_FONT_NONE);
}

static void
mk_emit_raw_line(struct mk_buf *buf, const char *text)
{

	mk_append(buf, text, strlen(text));
	mk_append_char(buf, '\n');
}

static void
mk_emit_text_line(struct mk_buf *buf, const char *text)
{

	mk_emit_inline(buf, text);
	mk_append_char(buf, '\n');
}

static void
mk_emit_macro(struct mk_buf *buf, const char *macro)
{

	mk_append_char(buf, '.');
	mk_append_cstr(buf, macro);
	mk_append_char(buf, '\n');
}

static void
mk_emit_heading(struct mk_buf *buf, const char *macro, const char *text)
{

	mk_append_char(buf, '.');
	mk_append_cstr(buf, macro);
	mk_append_cstr(buf, " \"");
	for (; *text != '\0'; text++) {
		if (*text == '\\' || *text == '"')
			mk_append_char(buf, '\\');
		mk_append_char(buf, *text);
	}
	mk_append_cstr(buf, "\"\n");
}

static int
mk_parse_atx_heading(const char *line, size_t *depth, char **text)
{
	const char	*cp;

	cp = line;
	while (*cp == ' ')
		cp++;
	if ((size_t)(cp - line) > 3 || *cp != '#')
		return 0;
	for (*depth = 0; cp[*depth] == '#'; (*depth)++)
		/* Nothing. */ ;
	if (*depth == 0 || *depth > 6 || cp[*depth] != ' ')
		return 0;
	*text = mk_strip_hash_closer(cp + *depth + 1);
	return 1;
}

static int
mk_is_setext_underline(const char *line)
{
	const char	*cp;
	char		 ch;

	cp = mk_trim_left(line);
	if (*cp != '=' && *cp != '-')
		return 0;
	ch = *cp;
	while (*cp == ch)
		cp++;
	while (*cp == ' ' || *cp == '\t')
		cp++;
	return *cp == '\0' ? (ch == '=' ? 1 : 2) : 0;
}

static int
mk_parse_title(const char *line, struct mk_title *title)
{
	const char	*lp, *rp;
	char		*head;
	size_t		 depth;
	char		*text;

	if (!mk_parse_atx_heading(line, &depth, &text) || depth != 1)
		return 0;
	rp = strrchr(text, ')');
	lp = rp == NULL ? NULL : strrchr(text, '(');
	if (lp == NULL || rp == NULL || lp >= rp) {
		free(text);
		return 0;
	}
	head = mk_slice_trim(text, (size_t)(lp - text));
	if (*head == '\0') {
		free(head);
		free(text);
		return 0;
	}
	title->title = head;
	title->sec = mk_slice_trim(lp + 1, (size_t)(rp - lp - 1));
	free(text);
	if (*title->sec == '\0') {
		free(title->title);
		free(title->sec);
		title->title = title->sec = NULL;
		return 0;
	}
	return 1;
}

static int
mk_is_ordered_item(const char *line, const char **text)
{
	const char	*cp;

	cp = mk_trim_left(line);
	while (isdigit((unsigned char)*cp))
		cp++;
	if (cp == mk_trim_left(line) || cp[0] != '.' || cp[1] != ' ')
		return 0;
	*text = cp + 2;
	return 1;
}

static int
mk_parse_bullet_item(const char *line, const char **text)
{
	const char	*cp;

	cp = mk_trim_left(line);
	if ((*cp != '-' && *cp != '*' && *cp != '+') || cp[1] != ' ')
		return 0;
	*text = cp + 2;
	return 1;
}

static int
mk_parse_blockquote(const char *line, const char **text)
{
	const char	*cp;

	cp = mk_trim_left(line);
	if (*cp != '>')
		return 0;
	cp++;
	if (*cp == ' ')
		cp++;
	*text = cp;
	return 1;
}

static int
mk_is_rule(const char *line)
{
	const char	*cp;
	char		 ch;
	int		 count;

	cp = mk_trim_left(line);
	if (*cp != '-' && *cp != '*' && *cp != '_')
		return 0;
	ch = *cp;
	count = 0;
	for (; *cp != '\0'; cp++) {
		if (*cp == ch)
			count++;
		else if (*cp != ' ' && *cp != '\t')
			return 0;
	}
	return count >= 3;
}

static int
mk_is_fence(const char *line, char *fence_ch, size_t *fence_len)
{
	const char	*cp;
	size_t		 indent, len;
	char		 ch;

	cp = line;
	for (indent = 0; *cp == ' '; indent++, cp++)
		/* Nothing. */ ;
	if (indent > 3 || (*cp != '`' && *cp != '~'))
		return 0;
	ch = *cp;
	for (len = 0; cp[len] == ch; len++)
		/* Nothing. */ ;
	if (len < 3)
		return 0;
	*fence_ch = ch;
	*fence_len = len;
	return 1;
}

static void
mk_split_lines(const char *input, size_t sz, struct mk_lines *out)
{
	size_t	 i, line_no;

	memset(out, 0, sizeof(*out));
	out->storage = mandoc_malloc(sz + 1);
	memcpy(out->storage, input, sz);
	out->storage[sz] = '\0';
	out->count = 1;
	for (i = 0; i < sz; i++)
		if (out->storage[i] == '\n')
			out->count++;
	out->lines = mandoc_reallocarray(NULL, out->count, sizeof(*out->lines));
	out->lines[0] = out->storage;
	for (i = line_no = 0; i < sz; i++) {
		if (out->storage[i] != '\n')
			continue;
		out->storage[i] = '\0';
		if (i > 0 && out->storage[i - 1] == '\r')
			out->storage[i - 1] = '\0';
		if (++line_no < out->count)
			out->lines[line_no] = out->storage + i + 1;
	}
	if (sz > 0 && out->storage[sz - 1] == '\r')
		out->storage[sz - 1] = '\0';
}

static void
mk_free_lines(struct mk_lines *lines)
{

	free(lines->lines);
	free(lines->storage);
	lines->lines = NULL;
	lines->storage = NULL;
	lines->count = 0;
}

static int
mk_parse_pipe_cells(const char *line, struct mk_cells *cells)
{
	struct mk_buf	 cell;
	const char	*cp;
	char		*trimmed;
	int		 saw_bar;
	int		 drop_first, drop_last;
	size_t		 i, start;

	memset(cells, 0, sizeof(*cells));
	memset(&cell, 0, sizeof(cell));
	cp = line;
	saw_bar = 0;
	while (*cp != '\0') {
		if (*cp == '\\' && cp[1] != '\0') {
			mk_append_char(&cell, cp[1]);
			cp += 2;
			continue;
		}
		if (*cp == '|') {
			saw_bar = 1;
			trimmed = mk_slice_trim(cell.buf == NULL ? "" : cell.buf,
			    cell.len);
			mk_cells_append(cells, trimmed);
			free(cell.buf);
			memset(&cell, 0, sizeof(cell));
			cp++;
			continue;
		}
		mk_append_char(&cell, *cp++);
	}
	trimmed = mk_slice_trim(cell.buf == NULL ? "" : cell.buf, cell.len);
	mk_cells_append(cells, trimmed);
	free(cell.buf);
	if (!saw_bar)
		return 0;

	drop_first = line[0] == '|' && cells->n > 0 && cells->v[0][0] == '\0';
	drop_last = cells->n > 0 && cells->v[cells->n - 1][0] == '\0';
	if (drop_last) {
		for (start = strlen(line); start > 0; start--) {
			if (line[start - 1] == '|')
				break;
			if (!isspace((unsigned char)line[start - 1])) {
				drop_last = 0;
				break;
			}
		}
	}
	if (drop_first || drop_last) {
		struct mk_cells	 kept;

		memset(&kept, 0, sizeof(kept));
		for (i = 0; i < cells->n; i++) {
			if (drop_first && i == 0) {
				free(cells->v[i]);
				continue;
			}
			if (drop_last && i + 1 == cells->n) {
				free(cells->v[i]);
				continue;
			}
			mk_cells_append(&kept, cells->v[i]);
		}
		free(cells->v);
		*cells = kept;
	}
	return cells->n > 0;
}

static int
mk_parse_table_align(const struct mk_cells *cells, char **aligns)
{
	size_t	 i, j, len;
	int	 has_dash, left, right;
	char	*parsed;

	parsed = mandoc_malloc(cells->n + 1);
	for (i = 0; i < cells->n; i++) {
		const char	*cell = cells->v[i];

		len = strlen(cell);
		left = len > 0 && cell[0] == ':';
		right = len > 0 && cell[len - 1] == ':';
		j = left ? 1 : 0;
		len -= right ? 1 : 0;
		has_dash = 0;
		for (; j < len; j++) {
			if (cell[j] == '-')
				has_dash = 1;
			else if (cell[j] != ' ' && cell[j] != '\t') {
				free(parsed);
				return 0;
			}
		}
		if (!has_dash) {
			free(parsed);
			return 0;
		}
		parsed[i] = left && right ? 'c' : right ? 'r' : 'l';
	}
	parsed[cells->n] = '\0';
	*aligns = parsed;
	return 1;
}

static void
mk_emit_table(struct mk_buf *out, const struct mk_cells *header,
    const char *aligns, struct mk_cells *rows, size_t row_count)
{
	size_t	 i, j;

	mk_append_cstr(out, ".TS\nallbox;\n");
	for (i = 0; i < header->n; i++) {
		if (i > 0)
			mk_append_char(out, ' ');
		mk_append_char(out, aligns[i]);
	}
	mk_append_cstr(out, ".\n");
	for (i = 0; i < header->n; i++) {
		if (i > 0)
			mk_append_char(out, '\t');
		mk_emit_inline(out, header->v[i]);
	}
	mk_append_cstr(out, "\n_\n");
	for (i = 0; i < row_count; i++) {
		for (j = 0; j < header->n; j++) {
			if (j > 0)
				mk_append_char(out, '\t');
			if (j < rows[i].n)
				mk_emit_inline(out, rows[i].v[j]);
		}
		mk_append_char(out, '\n');
	}
	mk_append_cstr(out, ".TE\n");
}

char *
man_from_markdown(const char *input, size_t sz)
{
	struct mk_buf	 out;
	struct mk_title	 title;
	struct mk_lines	 lines;
	struct mk_cells	 header, delim, *rows;
	char		*heading, *aligns;
	const char	*text, *item_text, *quote_text;
	size_t		 i, j, depth, row_count;
	char		 fence_ch;
	size_t		 fence_len;
	int		 saw_title, in_code, need_pp, after_head, setext_depth;

	memset(&out, 0, sizeof(out));
	memset(&title, 0, sizeof(title));
	memset(&lines, 0, sizeof(lines));
	mk_split_lines(input, sz, &lines);

	saw_title = in_code = need_pp = 0;
	after_head = 1;
	fence_ch = '\0';
	fence_len = 0;

	for (i = 0; i < lines.count; i++) {
		const char	*line;

		line = lines.lines[i];
		if (in_code) {
			char	 close_ch;
			size_t	 close_len;

			if (mk_is_fence(line, &close_ch, &close_len) &&
			    close_ch == fence_ch && close_len >= fence_len) {
				mk_emit_macro(&out, "EE");
				in_code = 0;
				need_pp = 0;
				after_head = 0;
				continue;
			}
			mk_emit_raw_line(&out, line);
			continue;
		}

		if (mk_is_blank(line)) {
			need_pp = saw_title && !after_head;
			continue;
		}

		if (!saw_title) {
			if (mk_parse_title(line, &title)) {
				mk_append_cstr(&out, ".TH \"");
				mk_append_cstr(&out, title.title);
				mk_append_cstr(&out, "\" \"");
				mk_append_cstr(&out, title.sec);
				mk_append_cstr(&out, "\"\n");
				saw_title = 1;
				after_head = 1;
				continue;
			}
			if (mk_parse_atx_heading(line, &depth, &heading) && depth == 1) {
				mk_append_cstr(&out, ".TH \"");
				mk_append_cstr(&out, heading);
				mk_append_cstr(&out, "\" \"7\"\n");
				free(heading);
				saw_title = 1;
				after_head = 1;
				continue;
			}
			if (i + 1 < lines.count &&
			    mk_is_setext_underline(lines.lines[i + 1]) == 1) {
				heading = mk_slice_trim(line, strlen(line));
				mk_append_cstr(&out, ".TH \"");
				mk_append_cstr(&out, heading);
				mk_append_cstr(&out, "\" \"7\"\n");
				free(heading);
				saw_title = 1;
				after_head = 1;
				i++;
				continue;
			}
			title.title = mandoc_strdup("MARKDOWN");
			title.sec = mandoc_strdup("7");
			mk_append_cstr(&out, ".TH \"MARKDOWN\" \"7\"\n");
			saw_title = 1;
			after_head = 0;
		}

		if (mk_is_fence(line, &fence_ch, &fence_len)) {
			if (need_pp && !after_head)
				mk_emit_macro(&out, "PP");
			mk_emit_macro(&out, "EX");
			in_code = 1;
			need_pp = 0;
			after_head = 0;
			continue;
		}

		if (mk_parse_atx_heading(line, &depth, &heading)) {
			mk_emit_heading(&out, depth <= 2 ? "SH" : "SS", heading);
			free(heading);
			need_pp = 0;
			after_head = 1;
			continue;
		}
		if (i + 1 < lines.count &&
		    !mk_is_blank(line) &&
		    (setext_depth = mk_is_setext_underline(lines.lines[i + 1])) != 0) {
			heading = mk_slice_trim(line, strlen(line));
			mk_emit_heading(&out, setext_depth <= 2 ? "SH" : "SS",
			    heading);
			free(heading);
			need_pp = 0;
			after_head = 1;
			i++;
			continue;
		}

		memset(&header, 0, sizeof(header));
		memset(&delim, 0, sizeof(delim));
		if (i + 1 < lines.count &&
		    mk_parse_pipe_cells(line, &header) &&
		    mk_parse_pipe_cells(lines.lines[i + 1], &delim) &&
		    header.n == delim.n && mk_parse_table_align(&delim, &aligns)) {
			rows = NULL;
			row_count = 0;
			for (j = i + 2; j < lines.count; j++) {
				struct mk_cells	 row;

				if (mk_is_blank(lines.lines[j]))
					break;
				if (!mk_parse_pipe_cells(lines.lines[j], &row))
					break;
				rows = mandoc_reallocarray(rows, row_count + 1,
				    sizeof(*rows));
				rows[row_count++] = row;
			}
			mk_emit_table(&out, &header, aligns, rows, row_count);
			{
				size_t next_i, k;

				next_i = j;
				for (k = 0; k < row_count; k++)
					mk_cells_free(&rows[k]);
				i = next_i - 1;
			}
			free(rows);
			free(aligns);
			mk_cells_free(&header);
			mk_cells_free(&delim);
			need_pp = 0;
			after_head = 0;
			continue;
		}
		mk_cells_free(&header);
		mk_cells_free(&delim);

		if (mk_parse_blockquote(line, &quote_text)) {
			int	 quote_pp;

			if (need_pp && !after_head)
				mk_emit_macro(&out, "PP");
			mk_emit_macro(&out, "RS");
			quote_pp = 0;
			for (j = i; j < lines.count; j++) {
				if (!mk_parse_blockquote(lines.lines[j], &quote_text))
					break;
				if (mk_is_blank(quote_text)) {
					quote_pp = 1;
					continue;
				}
				if (quote_pp)
					mk_emit_macro(&out, "PP");
				mk_emit_text_line(&out, quote_text);
				quote_pp = 0;
			}
			mk_emit_macro(&out, "RE");
			i = j - 1;
			need_pp = 0;
			after_head = 0;
			continue;
		}

		if (mk_parse_bullet_item(line, &item_text)) {
			mk_append_cstr(&out, ".IP \"\\(bu\" 2\n");
			mk_emit_text_line(&out, item_text);
			need_pp = 0;
			after_head = 0;
			continue;
		}
		if (mk_is_ordered_item(line, &item_text)) {
			const char	*cp;

			text = mk_trim_left(line);
			cp = text;
			while (isdigit((unsigned char)*cp))
				cp++;
			mk_append_cstr(&out, ".IP \"");
			mk_append(&out, text, (size_t)(cp - text + 1));
			mk_append_cstr(&out, "\" 3\n");
			mk_emit_text_line(&out, item_text);
			need_pp = 0;
			after_head = 0;
			continue;
		}
		if (mk_is_rule(line)) {
			mk_emit_macro(&out, "sp");
			need_pp = 0;
			after_head = 0;
			continue;
		}

		if (need_pp && !after_head)
			mk_emit_macro(&out, "PP");
		text = mk_trim_left(line);
		mk_emit_text_line(&out, text);
		need_pp = 0;
		after_head = 0;
	}

	if (in_code)
		mk_emit_macro(&out, "EE");
	if (out.buf == NULL)
		out.buf = mandoc_strdup(".TH \"MARKDOWN\" \"7\"\n");

	free(title.title);
	free(title.sec);
	mk_free_lines(&lines);
	return out.buf;
}
