#include "lib/text_buffer.h"
#include "portable_sbuf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct sbuf {
	struct bx_text_buffer buffer;
	int failed;
};

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
	bx_text_buffer_destroy(&s->buffer);
	free(s);
}

int
bx_rsync_sbuf_bcat(struct sbuf *s, const void *data, size_t len)
{
	if (s->failed)
		return -1;
	if (!bx_text_buffer_append_span(&s->buffer, data, len)) {
		s->failed = 1;
		return -1;
	}
	return 0;
}

int
bx_rsync_sbuf_cat(struct sbuf *s, const char *str)
{
	if (s->failed)
		return -1;
	if (!bx_text_buffer_append_text(&s->buffer, str)) {
		s->failed = 1;
		return -1;
	}
	return 0;
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

	if (s->failed)
		return -1;
	va_start(ap, fmt);
	va_copy(copy, ap);
	count = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (count < 0 || s->buffer.length == SIZE_MAX ||
	    (size_t)count > SIZE_MAX - s->buffer.length - 1) {
		if (count >= 0)
			errno = EOVERFLOW;
		s->failed = 1;
		va_end(ap);
		return -1;
	}
	if (!bx_text_buffer_reserve(&s->buffer,
	    s->buffer.length + (size_t)count + 1)) {
		s->failed = 1;
		va_end(ap);
		return -1;
	}
	vsnprintf(s->buffer.data + s->buffer.length,
	    s->buffer.capacity - s->buffer.length, fmt, ap);
	va_end(ap);
	s->buffer.length += (size_t)count;
	return 0;
}

int
bx_rsync_sbuf_finish(struct sbuf *s)
{
	if (s->failed)
		return -1;
	if (!bx_text_buffer_reserve(&s->buffer, s->buffer.length + 1)) {
		s->failed = 1;
		return -1;
	}
	s->buffer.data[s->buffer.length] = '\0';
	return 0;
}

char *
bx_rsync_sbuf_data(struct sbuf *s)
{
	return s->buffer.data;
}

size_t
bx_rsync_sbuf_len(const struct sbuf *s)
{
	return s->buffer.length;
}
