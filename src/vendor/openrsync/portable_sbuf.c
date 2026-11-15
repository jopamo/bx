#include "portable_sbuf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sbuf {
	char *data;
	size_t len;
	size_t cap;
	int failed;
};

static int
sbuf_reserve(struct sbuf *s, size_t extra)
{
	size_t need, cap;
	char *newdata;

	if (s->failed)
		return -1;
	if (extra > SIZE_MAX - s->len - 1) {
		errno = EOVERFLOW;
		s->failed = 1;
		return -1;
	}
	need = s->len + extra + 1;
	if (need <= s->cap)
		return 0;
	cap = s->cap ? s->cap : 128;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	newdata = realloc(s->data, cap);
	if (newdata == NULL) {
		s->failed = 1;
		return -1;
	}
	s->data = newdata;
	s->cap = cap;
	return 0;
}

struct sbuf *
bx_rsync_sbuf_new_auto(void)
{
	return calloc(1, sizeof(struct sbuf));
}

void
bx_rsync_sbuf_delete(struct sbuf *s)
{
	if (s == NULL)
		return;
	free(s->data);
	free(s);
}

int
bx_rsync_sbuf_bcat(struct sbuf *s, const void *data, size_t len)
{
	if (sbuf_reserve(s, len) == -1)
		return -1;
	memcpy(s->data + s->len, data, len);
	s->len += len;
	return 0;
}

int
bx_rsync_sbuf_cat(struct sbuf *s, const char *str)
{
	return bx_rsync_sbuf_bcat(s, str, strlen(str));
}

int
bx_rsync_sbuf_putc(struct sbuf *s, int ch)
{
	char c = (char)ch;
	return bx_rsync_sbuf_bcat(s, &c, 1);
}

int
bx_rsync_sbuf_printf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap, copy;
	int count;

	va_start(ap, fmt);
	va_copy(copy, ap);
	count = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (count < 0 || sbuf_reserve(s, (size_t)count) == -1) {
		s->failed = 1;
		va_end(ap);
		return -1;
	}
	vsnprintf(s->data + s->len, s->cap - s->len, fmt, ap);
	va_end(ap);
	s->len += (size_t)count;
	return 0;
}

int
bx_rsync_sbuf_finish(struct sbuf *s)
{
	if (sbuf_reserve(s, 0) == -1)
		return -1;
	s->data[s->len] = '\0';
	return 0;
}

char *
bx_rsync_sbuf_data(struct sbuf *s)
{
	return s->data;
}

size_t
bx_rsync_sbuf_len(const struct sbuf *s)
{
	return s->len;
}
