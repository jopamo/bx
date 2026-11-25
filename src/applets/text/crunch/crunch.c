/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent streaming implementation of the documented crunch interface. */
#define _GNU_SOURCE
#include <bzlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <lzma.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <zlib.h>
#include "dispatch/applets.h"

#define MAX_WORD 4096
struct set { wchar_t *text; size_t count; };
struct slot { const wchar_t *text; size_t count, index; int category; };
struct output {
    const char *path;
    const char *codec;
    FILE *file;
    gzFile gzip;
    BZFILE *bzip;
    lzma_stream lzma;
    uint64_t bytes, lines, byte_limit, line_limit;
    char *first, *last;
    bool split;
};

static void fail(const char *message) {
    fprintf(stderr, "crunch: %s\n", message);
    exit(1);
}
static void io_fail(void) { perror("crunch"); exit(1); }
static void *allocate(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) fail("allocation size overflow");
    void *p = calloc(n ? n : 1, size);
    if (!p) io_fail();
    return p;
}
static char *copy(const char *s) {
    char *p = strdup(s);
    if (!p) io_fail();
    return p;
}
static uint64_t number(const char *s, const char **tail) {
    if (*s < '0' || *s > '9') fail("expected an unsigned number");
    errno = 0;
    char *end;
    unsigned long long n = strtoull(s, &end, 10);
    if (errno) fail("number is too large");
    if (tail) *tail = end;
    else if (*end) fail("invalid number");
    return n;
}
static wchar_t *wide(const char *s) {
    mbstate_t state = {0};
    const char *p = s;
    size_t n = mbsrtowcs(NULL, &p, 0, &state);
    if (n == (size_t)-1) fail("invalid character encoding for this locale");
    if (n > MAX_WORD) fail("word or character set exceeds 4096 characters");
    wchar_t *w = allocate(n + 1, sizeof(*w));
    state = (mbstate_t){0};
    if (mbsrtowcs(w, &s, n + 1, &state) == (size_t)-1) fail("invalid character encoding");
    if (wcschr(w, L'\n') || wcschr(w, L'\r')) fail("newlines are not word characters");
    return w;
}
static char *narrow(const wchar_t *w) {
    mbstate_t state = {0};
    const wchar_t *p = w;
    size_t n = wcsrtombs(NULL, &p, 0, &state);
    if (n == (size_t)-1 || n > SIZE_MAX - 2) fail("cannot encode word");
    char *s = allocate(n + 2, 1);
    state = (mbstate_t){0};
    if (wcsrtombs(s, &w, n + 1, &state) == (size_t)-1) fail("cannot encode word");
    return s;
}
static struct set charset(const char *s) {
    struct set result = { .text = wide(s) };
    for (size_t i = 0; result.text[i]; i++) {
        bool found = false;
        for (size_t j = 0; j < result.count; j++) if (result.text[j] == result.text[i]) found = true;
        if (!found) result.text[result.count++] = result.text[i];
    }
    result.text[result.count] = 0;
    if (!result.count) fail("empty character set");
    return result;
}
static int category(wchar_t c) {
    static const wchar_t symbols[] = L"@,%^";
    const wchar_t *p = wcschr(symbols, c);
    return c && p ? (int)(p - symbols) : -1;
}
static char *load_set(const char *path, const char *name) {
    FILE *f = fopen(path, "r");
    if (!f) io_fail();
    char *line = NULL, *result = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t len = strcspn(p, " \t=\r\n");
        if (len != strlen(name) || strncmp(p, name, len)) continue;
        p += len;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != '=') fail("malformed charset entry");
        while (*p == ' ' || *p == '\t') p++;
        char *end = strrchr(p, ']');
        if (*p != '[' || !end) fail("malformed charset entry");
        *end = 0;
        result = copy(p + 1);
        break;
    }
    if (ferror(f)) io_fail();
    free(line); fclose(f);
    if (!result) fail("charset name not found");
    return result;
}

static size_t load_words(const char *path, wchar_t **words) {
    FILE *f = fopen(path, "r");
    if (!f) io_fail();
    char *line = NULL;
    size_t cap = 0, count = 0;
    while (getline(&line, &cap, f) >= 0) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        if (count == MAX_WORD) fail("too many permutation words");
        words[count++] = wide(line);
    }
    if (ferror(f)) io_fail();
    fclose(f);
    free(line);
    if (!count) fail("empty permutation file");
    return count;
}

static void lzma_write(struct output *o, const void *data, size_t n, bool finish) {
    o->lzma.next_in = data;
    o->lzma.avail_in = n;
    do {
        unsigned char buf[65536];
        o->lzma.next_out = buf;
        o->lzma.avail_out = sizeof(buf);
        lzma_ret ret = lzma_code(&o->lzma, finish ? LZMA_FINISH : LZMA_RUN);
        size_t used = sizeof(buf) - o->lzma.avail_out;
        if (fwrite(buf, 1, used, o->file) != used) io_fail();
        if (ret == LZMA_STREAM_END) break;
        if (ret != LZMA_OK) fail("lzma compression failed");
    } while (finish || o->lzma.avail_in);
}
static void open_output(struct output *o, int resume_fd) {
    if (!o->path) { o->file = stdout; return; }
    int fd = resume_fd;
    if (fd < 0) fd = open(o->path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                          (o->split ? O_EXCL : O_TRUNC), 0666);
    if (fd < 0) io_fail();
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) fail("output must be a regular file");
    if (o->codec && !strcmp(o->codec, "gzip")) {
        o->gzip = gzdopen(fd, "wb");
        if (!o->gzip) { close(fd); fail("cannot initialize gzip output"); }
    } else {
        o->file = fdopen(fd, "wb");
        if (!o->file) { close(fd); io_fail(); }
        if (o->codec && !strcmp(o->codec, "bzip2")) {
            int err;
            o->bzip = BZ2_bzWriteOpen(&err, o->file, 9, 0, 30);
            if (err != BZ_OK) fail("cannot initialize bzip2 output");
        } else if (o->codec) {
            lzma_options_lzma options;
            o->lzma = (lzma_stream)LZMA_STREAM_INIT;
            if (lzma_lzma_preset(&options, 6) || lzma_alone_encoder(&o->lzma, &options) != LZMA_OK)
                fail("cannot initialize lzma output");
        }
    }
}
static char *filename_word(const char *s) {
    size_t n = strlen(s);
    char *p = allocate(n + 1, 3), *w = p;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '/' || c == '%' || c < 32 || c == 127) {
            static const char hex[] = "0123456789ABCDEF";
            *w++ = '%'; *w++ = hex[c >> 4]; *w++ = hex[c & 15];
        } else *w++ = (char)c;
    }
    *w = 0;
    return p;
}
static void finish_output(struct output *o) {
    if (!o->file && !o->gzip) return;
    if (o->gzip) {
        if (gzclose(o->gzip) != Z_OK) fail("gzip output failed");
        o->gzip = NULL;
    } else {
        if (o->bzip) {
            int err;
            BZ2_bzWriteClose(&err, o->bzip, 0, NULL, NULL);
            if (err != BZ_OK) fail("bzip2 output failed");
            o->bzip = NULL;
        } else if (o->codec) { lzma_write(o, NULL, 0, true); lzma_end(&o->lzma); }
        if (o->file == stdout) { if (fflush(stdout)) io_fail(); }
        else if (fclose(o->file)) io_fail();
        o->file = NULL;
    }
    if (o->split && o->first) {
        char *a = filename_word(o->first), *b = filename_word(o->last), *name = NULL;
        const char *ext = !o->codec ? "" : !strcmp(o->codec, "gzip") ? ".gz" :
                          !strcmp(o->codec, "bzip2") ? ".bz2" : ".lzma";
        if (asprintf(&name, "%s-%s.txt%s", a, b, ext) < 0) io_fail();
        free(a); free(b);
        if (link(o->path, name) || unlink(o->path)) io_fail();
        free(name);
    }
    free(o->first); free(o->last);
    o->first = o->last = NULL;
    o->lines = o->bytes = 0;
}
static void emit(struct output *o, const wchar_t *word) {
    char *s = narrow(word);
    size_t n = strlen(s);
    if (o->lines && ((o->line_limit && o->lines >= o->line_limit) ||
        (o->byte_limit && (n + 1 > o->byte_limit || o->bytes > o->byte_limit - n - 1))))
        finish_output(o);
    if (!o->file && !o->gzip) open_output(o, -1);
    if (o->split) {
        if (!o->first) o->first = copy(s);
        free(o->last); o->last = copy(s);
    }
    s[n++] = '\n';
    if (o->gzip) {
        if (n > INT_MAX || gzwrite(o->gzip, s, (unsigned)n) != (int)n) fail("gzip output failed");
    } else if (o->bzip) {
        int err;
        if (n > INT_MAX) fail("word too large");
        BZ2_bzWrite(&err, o->bzip, s, (int)n);
        if (err != BZ_OK) fail("bzip2 output failed");
    } else if (o->codec) lzma_write(o, s, n, false);
    else if (fwrite(s, 1, n, o->file) != n) io_fail();
    if (o->bytes > UINT64_MAX - n || o->lines == UINT64_MAX) fail("output counter overflow");
    o->bytes += n; o->lines++;
    free(s);
}
static void slots(struct slot *s, size_t n, const struct set *sets,
                  const wchar_t *pattern, const wchar_t *literal) {
    for (size_t i = 0; i < n; i++) {
        int c = pattern ? category(pattern[i]) : 0;
        if (pattern && literal && literal[i] == pattern[i]) c = -1;
        s[i].category = c;
        s[i].index = 0;
        s[i].text = c < 0 ? pattern + i : sets[c].text;
        s[i].count = c < 0 ? 1 : sets[c].count;
    }
}
static void seed(struct slot *s, size_t n, const wchar_t *word) {
    if (wcslen(word) != n) fail("bound has the wrong length");
    for (size_t i = 0; i < n; i++) {
        size_t j = 0;
        while (j < s[i].count && s[i].text[j] != word[i]) j++;
        if (j == s[i].count) fail("bound does not match the pattern or character set");
        s[i].index = j;
    }
}
static bool advance(struct slot *s, size_t n, bool invert) {
    for (size_t j = 0; j < n; j++) {
        size_t i = invert ? j : n - j - 1;
        if (++s[i].index < s[i].count) return true;
        s[i].index = 0;
    }
    return false;
}
static bool allowed(const wchar_t *w, const struct slot *s, size_t n, const size_t *limits) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        run = i && w[i] == w[i - 1] ? run + 1 : 1;
        int c = s[i].category;
        if (c >= 0 && limits[c] && run > limits[c]) return false;
    }
    return true;
}
static int word_compare(const void *a, const void *b) {
    return wcscmp(*(wchar_t *const *)a, *(wchar_t *const *)b);
}
static bool permutation(wchar_t **words, size_t n) {
    if (n < 2) return false;
    size_t i = n - 1;
    while (i && wcscmp(words[i - 1], words[i]) >= 0) i--;
    if (!i) return false;
    size_t j = n - 1;
    while (wcscmp(words[j], words[i - 1]) <= 0) j--;
    wchar_t *tmp = words[i - 1]; words[i - 1] = words[j]; words[j] = tmp;
    for (j = n - 1; i < j; i++, j--) { tmp = words[i]; words[i] = words[j]; words[j] = tmp; }
    return true;
}
/* Read only a bounded tail. Discard an interrupted partial final line. */
static wchar_t *resume_word(struct output *o, int *fd, off_t *end, size_t max) {
    *fd = open(o->path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (*fd < 0) io_fail();
    struct stat st;
    if (fstat(*fd, &st) || !S_ISREG(st.st_mode)) fail("resume input must be a regular file");
    size_t cap = (max + 1) * MB_LEN_MAX * 2 + 2;
    size_t n = (uintmax_t)st.st_size < cap ? (size_t)st.st_size : cap;
    char *buf = allocate(n + 1, 1);
    off_t offset = st.st_size - (off_t)n;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(*fd, buf + got, n - got, offset + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) fail("cannot read resume tail");
        got += (size_t)r;
    }
    size_t finish = n;
    while (finish && buf[finish - 1] != '\n') finish--;
    if (!finish) {
        if (offset) fail("resume line exceeds configured word size");
        *end = 0; free(buf); return NULL;
    }
    *end = offset + (off_t)finish;
    size_t begin = finish - 1;
    while (begin && buf[begin - 1] != '\n') begin--;
    if (!begin && offset) fail("resume line exceeds configured word size");
    if (memchr(buf + begin, 0, finish - begin)) fail("NUL byte in resume line");
    buf[finish - 1] = 0;
    wchar_t *word = wide(buf + begin);
    free(buf);
    o->bytes = (uint64_t)*end;
    if (o->split) {
        char *first = allocate(cap, 1);
        size_t firstlen = 0;
        bool first_done = false;
        off_t at = 0;
        while (at < *end) {
            char block[65536];
            size_t want = (uintmax_t)(*end - at) < sizeof(block) ? (size_t)(*end - at) : sizeof(block);
            ssize_t r = pread(*fd, block, want, at);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) fail("cannot read split resume input");
            for (ssize_t i = 0; i < r; i++) {
                if (block[i] == '\n') { o->lines++; first_done = true; }
                else if (!first_done) {
                    if (firstlen + 1 >= cap) fail("resume line exceeds configured word size");
                    first[firstlen++] = block[i];
                }
            }
            at += r;
        }
        o->first = first;
        o->last = narrow(word);
    }
    return word;
}

int bx_crunch_main(int argc, char **argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        puts("usage: crunch min max [lower [upper [digits [symbols]]]] [options]\n"
             "  -t pattern -l literals -s start -e end -i -d count[@,%^]\n"
             "  -f charset-file name -o output -r -b size -c lines -u\n"
             "  -z gzip|bzip2|lzma -q word-file -p words... (last)");
        return 0;
    }
    if (argc < 3) fail("usage: crunch min max [charset] [options]");
    if (!setlocale(LC_CTYPE, "")) fail("cannot select character locale");
    uint64_t low = number(argv[1], NULL), high = number(argv[2], NULL);
    if (low > high || high > MAX_WORD) fail("length range must be within 0..4096");
    size_t min = (size_t)low, max = (size_t)high;
    struct set sets[4];
    const char *defaults[] = {"abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "0123456789", "!@#$%^&*()-_+=~`[]{}|\\:;\"'<>,.?/ "};
    int arg = 3;
    for (int i = 0; i < 4; i++) {
        const char *text = defaults[i];
        if (arg < argc && argv[arg][0] != '-') {
            if (strcmp(argv[arg], "+")) text = argv[arg];
            arg++;
        }
        sets[i] = charset(text);
    }
    wchar_t *pattern = NULL, *literal = NULL, *start = NULL, *end = NULL;
    wchar_t **words = allocate(MAX_WORD, sizeof(*words));
    size_t word_count = 0, limits[4] = {0};
    struct file_arg { const char *path, *charset_name; };
    struct file_arg *inputs = NULL;
    size_t input_count = 0;
    bool invert = false, resume = false, word_file = false;
    struct output out = {0};
    for (; arg < argc; arg++) {
        const char *opt = argv[arg];
        if (!strcmp(opt, "-i")) { invert = true; continue; }
        if (!strcmp(opt, "-u") || !strcmp(opt, "--stdout")) continue;
        if (!strcmp(opt, "-r")) { resume = true; continue; }
        if (strcmp(opt, "-p") && (strlen(opt) != 2 || !strchr("tlosefdbcqz", opt[1])))
            fail("unknown or unsupported option");
        if (++arg == argc) fail("option requires an argument");
        const char *value = argv[arg];
        switch (opt[1]) {
        case 't': free(pattern); pattern = wide(value); break;
        case 'l': free(literal); literal = wide(value); break;
        case 's': free(start); start = wide(value); break;
        case 'e': free(end); end = wide(value); break;
        case 'o': out.path = value; out.split = !strcmp(value, "START"); break;
        case 'z':
            if (strcmp(value, "gzip") && strcmp(value, "bzip2") && strcmp(value, "lzma"))
                fail("supported output compression: gzip, bzip2, lzma (no external programs)");
            out.codec = value; break;
        case 'f':
        case 'q': {
            const char *name = NULL;
            if (opt[1] == 'f') {
                if (++arg == argc) fail("-f requires a charset name");
                name = argv[arg];
            } else {
                if (word_file) fail("only one permutation input is allowed");
                word_file = true;
            }
            if (!inputs) inputs = allocate((size_t)argc / 2, sizeof(*inputs));
            inputs[input_count++] = (struct file_arg){value, name};
            break;
        }
        case 'd': {
            const char *tail;
            uint64_t count = number(value, &tail);
            if (!*tail || !count || count > MAX_WORD) fail("invalid repetition limit");
            for (; *tail; tail++) {
                int c = category((unsigned char)*tail);
                if (c < 0) fail("invalid repetition category");
                limits[c] = (size_t)count;
            }
            break;
        }
        case 'c': out.line_limit = number(value, NULL); if (!out.line_limit) fail("zero line limit"); break;
        case 'b': {
            const char *tail;
            uint64_t count = number(value, &tail), unit = 1;
            const char *suffix[] = {"kb", "mb", "gb", "kib", "mib", "gib"};
            if (*tail) {
                int i;
                for (i = 0; i < 6 && strcasecmp(tail, suffix[i]); i++);
                if (i == 6) fail("invalid size suffix");
                for (int j = 0; j <= i % 3; j++) unit *= i < 3 ? 1000 : 1024;
            }
            if (!count || count > UINT64_MAX / unit) fail("invalid byte limit");
            out.byte_limit = count * unit; break;
        }
        case 'p':
            if (word_file) fail("only one permutation input is allowed");
            if (arg == argc - 1) {
                struct set p = charset(value);
                for (size_t i = 0; i < p.count; i++) {
                    words[word_count] = allocate(2, sizeof(wchar_t));
                    words[word_count++][0] = p.text[i];
                }
                free(p.text);
            } else for (; arg < argc; arg++) {
                if (word_count == MAX_WORD) fail("too many permutation words");
                words[word_count++] = wide(argv[arg]);
            }
            arg = argc; break;
        default: fail("unsupported option");
        }
    }
    if (pattern && (wcslen(pattern) != min || min != max)) fail("pattern length must equal min and max");
    if (literal && (!pattern || wcslen(literal) != max)) fail("literal mask must match the pattern length");
    if ((out.line_limit || out.byte_limit) && !out.split) fail("splitting requires -o START");
    if (out.codec && !out.path) fail("compression requires -o");
    bool permutations = word_count != 0 || word_file;
    if (resume && (!out.path || out.codec || start || permutations))
        fail("resume requires plain -o output, without -s or permutations");
    if (permutations && (start || end)) fail("permutations cannot use start/end bounds");
    if (permutations && (limits[0] || limits[1] || limits[2] || limits[3]))
        fail("repetition limits cannot be combined with permutations");
    for (size_t i = 0; i < input_count; i++) {
        const struct file_arg *input = &inputs[i];
        if (input->charset_name) {
            char *text = load_set(input->path, input->charset_name);
            free(sets[0].text);
            sets[0] = charset(text);
            free(text);
        } else {
            word_count = load_words(input->path, words);
        }
    }
    free(inputs);
    char *compressed_path = NULL;
    if (out.codec && !out.split) {
        const char *ext = !strcmp(out.codec, "gzip") ? ".gz" : !strcmp(out.codec, "bzip2") ? ".bz2" : ".lzma";
        if (asprintf(&compressed_path, "%s%s", out.path, ext) < 0) io_fail();
        out.path = compressed_path;
    }
    struct slot *s = allocate(max + 1, sizeof(*s));
    wchar_t *word = allocate(max + 1, sizeof(*word));
    if (end) {
        size_t len = wcslen(end);
        if (len < min || len > max) fail("end length outside range");
        slots(s, len, sets, pattern, literal); seed(s, len, end);
    }
    int resume_fd = -1; off_t resume_end = 0;
    if (resume) start = resume_word(&out, &resume_fd, &resume_end, max);
    if (start) {
        size_t len = wcslen(start);
        if (len < min || len > max) fail("start length outside range");
        slots(s, len, sets, pattern, literal); seed(s, len, start);
        if (end) {
            size_t endlen = wcslen(end);
            if (len > endlen) fail("start follows end");
            if (len == endlen) {
                struct slot *last = allocate(len + 1, sizeof(*last));
                slots(last, len, sets, pattern, literal); seed(last, len, end);
                for (size_t j = 0; j < len; j++) {
                    size_t i = invert ? len - j - 1 : j;
                    if (s[i].index > last[i].index) fail("start follows end");
                    if (s[i].index < last[i].index) break;
                }
                free(last);
            }
        }
        min = len;
    }
    if (word_count) {
        qsort(words, word_count, sizeof(*words), word_compare);
        size_t total = max + 1, placeholders = 0;
        for (size_t i = 0; i < word_count; i++) total += wcslen(words[i]);
        if (pattern) {
            for (size_t i = 0; i < max; i++) if (category(pattern[i]) < 0) placeholders++;
            if (placeholders != word_count) fail("pattern needs one non-symbol placeholder per permutation word");
        }
        free(word); word = allocate(total, sizeof(*word));
    }
    if (resume_fd >= 0) {
        if (ftruncate(resume_fd, resume_end) || lseek(resume_fd, resume_end, SEEK_SET) < 0) io_fail();
        open_output(&out, resume_fd);
    } else if (!out.split) open_output(&out, -1);
    if (word_count) {
        do {
            slots(s, max, sets, pattern, literal);
            do {
                size_t pos = 0, item = 0;
                if (!pattern) for (size_t i = 0; i < word_count; i++) {
                    size_t n = wcslen(words[i]); wmemcpy(word + pos, words[i], n); pos += n;
                }
                else for (size_t i = 0; i < max; i++) {
                    if (category(pattern[i]) < 0) {
                        size_t n = wcslen(words[item]); wmemcpy(word + pos, words[item++], n); pos += n;
                    } else word[pos++] = s[i].text[s[i].index];
                }
                word[pos] = 0; emit(&out, word);
            } while (pattern && advance(s, max, invert));
        } while (permutation(words, word_count));
    } else {
        bool done = false;
        for (size_t len = min; len <= max && !done; len++) {
            slots(s, len, sets, pattern, literal);
            if (start && len == min) seed(s, len, start);
            do {
                for (size_t i = 0; i < len; i++) word[i] = s[i].text[s[i].index];
                word[len] = 0;
                if (!(resume && start) && allowed(word, s, len, limits)) emit(&out, word);
                resume = false;
                if (end && !wcscmp(word, end)) { done = true; break; }
            } while (advance(s, len, invert));
        }
    }
    finish_output(&out);
    for (int i = 0; i < 4; i++) free(sets[i].text);
    for (size_t i = 0; i < word_count; i++) free(words[i]);
    free(words); free(s); free(word); free(pattern); free(literal); free(start); free(end);
    free(compressed_path);
    return 0;
}
