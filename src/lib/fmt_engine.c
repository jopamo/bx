#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bx/diag.h"
#include "lib/fmt_engine.h"
#include "lib/line_writer.h"

#define BX_FMT_ENGINE_LEEWAY 7
#define BX_FMT_ENGINE_DEF_INDENT 3
#define BX_FMT_ENGINE_MAX_WORDS 1000
#define BX_FMT_ENGINE_MAX_CHARS 5000
#define BX_FMT_ENGINE_TAB_WIDTH 8

typedef long bx_fmt_engine_cost;

#define BX_FMT_ENGINE_MAX_COST LONG_MAX
#define BX_FMT_ENGINE_SQR(value) ((value) * (value))
#define BX_FMT_ENGINE_EQUIV(value) BX_FMT_ENGINE_SQR((bx_fmt_engine_cost)(value))
#define BX_FMT_ENGINE_SHORT_COST(delta) BX_FMT_ENGINE_EQUIV((delta) * 10)
#define BX_FMT_ENGINE_RAGGED_COST(delta) (BX_FMT_ENGINE_SHORT_COST(delta) / 2)
#define BX_FMT_ENGINE_LINE_COST BX_FMT_ENGINE_EQUIV(70)
#define BX_FMT_ENGINE_WIDOW_COST(length) (BX_FMT_ENGINE_EQUIV(200) / ((length) + 2))
#define BX_FMT_ENGINE_ORPHAN_COST(length) (BX_FMT_ENGINE_EQUIV(150) / ((length) + 2))
#define BX_FMT_ENGINE_SENTENCE_BONUS BX_FMT_ENGINE_EQUIV(50)
#define BX_FMT_ENGINE_NOBREAK_COST BX_FMT_ENGINE_EQUIV(600)
#define BX_FMT_ENGINE_PAREN_BONUS BX_FMT_ENGINE_EQUIV(40)
#define BX_FMT_ENGINE_PUNCT_BONUS BX_FMT_ENGINE_EQUIV(40)
#define BX_FMT_ENGINE_LINE_CREDIT BX_FMT_ENGINE_EQUIV(3)

struct bx_fmt_engine_word {
    char const *text;
    int length;
    int space;
    unsigned int paren : 1;
    unsigned int period : 1;
    unsigned int punct : 1;
    unsigned int final : 1;
    int line_length;
    bx_fmt_engine_cost best_cost;
    struct bx_fmt_engine_word *next_break;
};

struct bx_fmt_engine {
    FILE *stream;
    const struct bx_fmt_engine_options *options;
    struct bx_line_writer *writer;
    struct bx_diag_ctx *diag;
    int in_column;
    int out_column;
    char parabuf[BX_FMT_ENGINE_MAX_CHARS];
    char *wptr;
    struct bx_fmt_engine_word words[BX_FMT_ENGINE_MAX_WORDS];
    struct bx_fmt_engine_word *word_limit;
    bool tabs;
    int prefix_indent;
    int first_indent;
    int other_indent;
    int next_char;
    int next_prefix_indent;
    int last_line_length;
    bool ok;
};

static bool bx_fmt_engine_writer_write(struct bx_fmt_engine *engine,
                                       const void *data,
                                       size_t length) {
    if (!bx_line_writer_write(engine->writer, data, length)) {
        bx_diag(engine->diag, "write error: %s", strerror(errno));
        engine->ok = false;
        return false;
    }
    return true;
}

static bool bx_fmt_engine_writer_putc(struct bx_fmt_engine *engine, char ch) {
    if (!bx_line_writer_putc(engine->writer, ch)) {
        bx_diag(engine->diag, "write error: %s", strerror(errno));
        engine->ok = false;
        return false;
    }
    return true;
}

static bool bx_fmt_engine_is_space(int ch) {
    switch (ch) {
        case ' ':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
        case '\v':
            return true;
        default:
            return false;
    }
}

static bool bx_fmt_engine_is_open_punct(int ch) {
    return strchr("([\'`\"", ch) != NULL;
}

static bool bx_fmt_engine_is_close_punct(int ch) {
    return strchr(")]'\"", ch) != NULL;
}

static bool bx_fmt_engine_is_period_punct(int ch) {
    return strchr(".?!", ch) != NULL;
}

size_t bx_fmt_engine_default_goal_width(size_t width) {
    size_t goal = (width * (size_t)(2 * (100 - BX_FMT_ENGINE_LEEWAY) + 1)) / 200u;

    if (goal > width) {
        goal = width;
    }
    return goal;
}

static bool bx_fmt_engine_put_space(struct bx_fmt_engine *engine, int space) {
    int space_target = engine->out_column + space;

    if (engine->tabs) {
        int tab_target = (space_target / BX_FMT_ENGINE_TAB_WIDTH) * BX_FMT_ENGINE_TAB_WIDTH;
        if (engine->out_column + 1 < tab_target) {
            while (engine->out_column < tab_target) {
                if (!bx_fmt_engine_writer_putc(engine, '\t')) {
                    return false;
                }
                engine->out_column =
                    (engine->out_column / BX_FMT_ENGINE_TAB_WIDTH + 1) *
                    BX_FMT_ENGINE_TAB_WIDTH;
            }
        }
    }

    while (engine->out_column < space_target) {
        if (!bx_fmt_engine_writer_putc(engine, ' ')) {
            return false;
        }
        engine->out_column++;
    }
    return true;
}

static void bx_fmt_engine_set_other_indent(struct bx_fmt_engine *engine,
                                            bool same_paragraph) {
    if (engine->options->split_only) {
        engine->other_indent = engine->first_indent;
    } else if (engine->options->crown_margin) {
        engine->other_indent = same_paragraph ? engine->in_column
                                              : engine->first_indent;
    } else if (engine->options->tagged_paragraph) {
        if (same_paragraph && engine->in_column != engine->first_indent) {
            engine->other_indent = engine->in_column;
        } else if (engine->other_indent == engine->first_indent) {
            engine->other_indent =
                engine->first_indent == 0 ? BX_FMT_ENGINE_DEF_INDENT : 0;
        }
    } else {
        engine->other_indent = engine->first_indent;
    }
}

static int bx_fmt_engine_get_space(struct bx_fmt_engine *engine, int ch) {
    while (true) {
        if (ch == ' ') {
            engine->in_column++;
        } else if (ch == '\t') {
            engine->tabs = true;
            engine->in_column =
                (engine->in_column / BX_FMT_ENGINE_TAB_WIDTH + 1) *
                BX_FMT_ENGINE_TAB_WIDTH;
        } else {
            return ch;
        }
        ch = getc(engine->stream);
    }
}

static int bx_fmt_engine_get_prefix(struct bx_fmt_engine *engine) {
    int ch;

    engine->in_column = 0;
    ch = bx_fmt_engine_get_space(engine, getc(engine->stream));
    if (engine->options->prefix_length == 0u) {
        size_t indent = engine->options->prefix_lead_space < (size_t)engine->in_column
                            ? engine->options->prefix_lead_space
                            : (size_t)engine->in_column;
        engine->next_prefix_indent = (int)indent;
    } else {
        char const *prefix = engine->options->prefix;

        engine->next_prefix_indent = engine->in_column;
        for (; *prefix != '\0'; prefix++) {
            unsigned char prefix_ch = (unsigned char)*prefix;
            if (ch != (int)prefix_ch) {
                return ch;
            }
            engine->in_column++;
            ch = getc(engine->stream);
        }
        ch = bx_fmt_engine_get_space(engine, ch);
    }
    return ch;
}

static bool bx_fmt_engine_same_paragraph(const struct bx_fmt_engine *engine,
                                         int ch) {
    return engine->next_prefix_indent == engine->prefix_indent &&
           engine->in_column >=
               engine->next_prefix_indent + (int)engine->options->prefix_full_length &&
           ch != '\n' && ch != EOF;
}

static int bx_fmt_engine_copy_rest(struct bx_fmt_engine *engine, int ch) {
    engine->out_column = 0;
    if (engine->in_column > engine->next_prefix_indent ||
        (ch != '\n' && ch != EOF)) {
        char const *prefix = engine->options->prefix;

        if (!bx_fmt_engine_put_space(engine, engine->next_prefix_indent)) {
            return EOF;
        }
        while (engine->out_column != engine->in_column && *prefix != '\0') {
            if (!bx_fmt_engine_writer_putc(engine, *prefix++)) {
                return EOF;
            }
            engine->out_column++;
        }
        if (ch != EOF && ch != '\n' &&
            !bx_fmt_engine_put_space(engine, engine->in_column - engine->out_column)) {
            return EOF;
        }
        if (ch == EOF &&
            engine->in_column >=
                engine->next_prefix_indent + (int)engine->options->prefix_length) {
            if (!bx_fmt_engine_writer_putc(engine, '\n')) {
                return EOF;
            }
        }
    }

    while (ch != '\n' && ch != EOF) {
        if (!bx_fmt_engine_writer_putc(engine, (char)ch)) {
            return EOF;
        }
        ch = getc(engine->stream);
    }
    return ch;
}

static void bx_fmt_engine_check_punctuation(struct bx_fmt_engine_word *word) {
    char const *start = word->text;
    char const *finish = start + (word->length - 1);
    unsigned char fin = (unsigned char)*finish;

    word->paren = bx_fmt_engine_is_open_punct((unsigned char)*start) ? 1u : 0u;
    word->punct = ispunct(fin) ? 1u : 0u;
    while (start < finish && bx_fmt_engine_is_close_punct((unsigned char)*finish)) {
        finish--;
    }
    word->period = bx_fmt_engine_is_period_punct((unsigned char)*finish) ? 1u : 0u;
}

static void bx_fmt_engine_put_word(struct bx_fmt_engine *engine,
                                   const struct bx_fmt_engine_word *word) {
    (void)bx_fmt_engine_writer_write(engine, word->text, (size_t)word->length);
    engine->out_column += word->length;
}

static bx_fmt_engine_cost bx_fmt_engine_base_cost(
    const struct bx_fmt_engine *engine,
    const struct bx_fmt_engine_word *word
) {
    bx_fmt_engine_cost cost = BX_FMT_ENGINE_LINE_COST;

    if (word > engine->words) {
        if ((word - 1)->period) {
            if ((word - 1)->final) {
                cost -= BX_FMT_ENGINE_SENTENCE_BONUS;
            } else {
                cost += BX_FMT_ENGINE_NOBREAK_COST;
            }
        } else if ((word - 1)->punct) {
            cost -= BX_FMT_ENGINE_PUNCT_BONUS;
        } else if (word > engine->words + 1 && (word - 2)->final) {
            cost += BX_FMT_ENGINE_WIDOW_COST((word - 1)->length);
        }
    }

    if (word->paren) {
        cost -= BX_FMT_ENGINE_PAREN_BONUS;
    } else if (word->final) {
        cost += BX_FMT_ENGINE_ORPHAN_COST(word->length);
    }

    return cost;
}

static bx_fmt_engine_cost bx_fmt_engine_line_cost(
    const struct bx_fmt_engine *engine,
    const struct bx_fmt_engine_word *next,
    int len
) {
    int delta;
    bx_fmt_engine_cost cost;

    if (next == engine->word_limit) {
        return 0;
    }

    delta = (int)engine->options->goal - len;
    cost = BX_FMT_ENGINE_SHORT_COST(delta);
    if (next->next_break != engine->word_limit) {
        delta = len - next->line_length;
        cost += BX_FMT_ENGINE_RAGGED_COST(delta);
    }
    return cost;
}

static void bx_fmt_engine_fmt_paragraph(struct bx_fmt_engine *engine) {
    int saved_length = engine->word_limit->length;

    engine->word_limit->best_cost = 0;
    engine->word_limit->length = (int)engine->options->width;

    for (struct bx_fmt_engine_word *start = engine->word_limit - 1;
         start >= engine->words;
         start--) {
        struct bx_fmt_engine_word *word = start;
        bx_fmt_engine_cost best = BX_FMT_ENGINE_MAX_COST;
        int len = start == engine->words ? engine->first_indent : engine->other_indent;

        len += word->length;
        do {
            bx_fmt_engine_cost word_cost;

            word++;
            word_cost = bx_fmt_engine_line_cost(engine, word, len) + word->best_cost;
            if (start == engine->words && engine->last_line_length > 0) {
                word_cost +=
                    BX_FMT_ENGINE_RAGGED_COST(len - engine->last_line_length);
            }
            if (word_cost < best) {
                best = word_cost;
                start->next_break = word;
                start->line_length = len;
            }
            if (word == engine->word_limit) {
                break;
            }
            len += (word - 1)->space + word->length;
        } while (len <= (int)engine->options->width);

        start->best_cost = best + bx_fmt_engine_base_cost(engine, start);
    }

    engine->word_limit->length = saved_length;
}

static void bx_fmt_engine_put_line(struct bx_fmt_engine *engine,
                                   struct bx_fmt_engine_word *word,
                                   int indent) {
    struct bx_fmt_engine_word *endline;

    engine->out_column = 0;
    if (!bx_fmt_engine_put_space(engine, engine->prefix_indent)) {
        return;
    }
    if (!bx_fmt_engine_writer_write(engine, engine->options->prefix,
                                    engine->options->prefix_length)) {
        return;
    }
    engine->out_column += (int)engine->options->prefix_length;
    if (!bx_fmt_engine_put_space(engine, indent - engine->out_column)) {
        return;
    }

    endline = word->next_break - 1;
    for (; word != endline; word++) {
        bx_fmt_engine_put_word(engine, word);
        if (!engine->ok) {
            return;
        }
        if (!bx_fmt_engine_put_space(engine, word->space)) {
            return;
        }
    }
    bx_fmt_engine_put_word(engine, word);
    if (!engine->ok) {
        return;
    }

    engine->last_line_length = engine->out_column;
    (void)bx_fmt_engine_writer_putc(engine, '\n');
}

static void bx_fmt_engine_put_paragraph(struct bx_fmt_engine *engine,
                                        struct bx_fmt_engine_word *finish) {
    bx_fmt_engine_put_line(engine, engine->words, engine->first_indent);
    for (struct bx_fmt_engine_word *word = engine->words->next_break;
         engine->ok && word != finish;
         word = word->next_break) {
        bx_fmt_engine_put_line(engine, word, engine->other_indent);
    }
}

static void bx_fmt_engine_flush_paragraph(struct bx_fmt_engine *engine) {
    struct bx_fmt_engine_word *split_point;
    struct bx_fmt_engine_word *word;
    int shift;
    bx_fmt_engine_cost best_break;

    if (engine->word_limit == engine->words) {
        size_t to_write = (size_t)(engine->wptr - engine->parabuf);

        (void)bx_fmt_engine_writer_write(engine, engine->parabuf, to_write);
        engine->wptr = engine->parabuf;
        return;
    }

    bx_fmt_engine_fmt_paragraph(engine);
    if (!engine->ok) {
        return;
    }

    split_point = engine->word_limit;
    best_break = BX_FMT_ENGINE_MAX_COST;
    for (word = engine->words->next_break; word != engine->word_limit;
         word = word->next_break) {
        bx_fmt_engine_cost delta = word->best_cost - word->next_break->best_cost;

        if (delta < best_break) {
            split_point = word;
            best_break = delta;
        }
        if (best_break <= BX_FMT_ENGINE_MAX_COST - BX_FMT_ENGINE_LINE_CREDIT) {
            best_break += BX_FMT_ENGINE_LINE_CREDIT;
        }
    }
    bx_fmt_engine_put_paragraph(engine, split_point);
    if (!engine->ok) {
        return;
    }

    memmove(engine->parabuf, split_point->text,
            (size_t)(engine->wptr - split_point->text));
    shift = (int)(split_point->text - engine->parabuf);
    engine->wptr -= shift;

    for (word = split_point; word <= engine->word_limit; word++) {
        word->text -= shift;
    }

    memmove(engine->words, split_point,
            (size_t)(engine->word_limit - split_point + 1) * sizeof(*word));
    engine->word_limit -= split_point - engine->words;
}

static int bx_fmt_engine_get_line(struct bx_fmt_engine *engine, int ch) {
    char *end_of_parabuf = &engine->parabuf[BX_FMT_ENGINE_MAX_CHARS];
    struct bx_fmt_engine_word *end_of_word = &engine->words[BX_FMT_ENGINE_MAX_WORDS - 2];

    do {
        int start;

        engine->word_limit->text = engine->wptr;
        do {
            if (engine->wptr == end_of_parabuf) {
                bx_fmt_engine_set_other_indent(engine, true);
                bx_fmt_engine_flush_paragraph(engine);
                if (!engine->ok) {
                    return EOF;
                }
            }
            *engine->wptr++ = (char)ch;
            ch = getc(engine->stream);
        } while (ch != EOF && !bx_fmt_engine_is_space(ch));

        engine->in_column += engine->word_limit->length =
            (int)(engine->wptr - engine->word_limit->text);
        bx_fmt_engine_check_punctuation(engine->word_limit);

        start = engine->in_column;
        ch = bx_fmt_engine_get_space(engine, ch);
        engine->word_limit->space = engine->in_column - start;
        engine->word_limit->final =
            ch == EOF ||
            (engine->word_limit->period &&
             (ch == '\n' || engine->word_limit->space > 1));
        if (ch == '\n' || ch == EOF || engine->options->uniform_spacing) {
            engine->word_limit->space = engine->word_limit->final ? 2 : 1;
        }
        if (engine->word_limit == end_of_word) {
            bx_fmt_engine_set_other_indent(engine, true);
            bx_fmt_engine_flush_paragraph(engine);
            if (!engine->ok) {
                return EOF;
            }
        }
        engine->word_limit++;
    } while (ch != '\n' && ch != EOF);

    return bx_fmt_engine_get_prefix(engine);
}

static bool bx_fmt_engine_get_paragraph(struct bx_fmt_engine *engine) {
    int ch = engine->next_char;

    engine->last_line_length = 0;
    while (ch == '\n' || ch == EOF ||
           engine->next_prefix_indent < (int)engine->options->prefix_lead_space ||
           engine->in_column <
               engine->next_prefix_indent + (int)engine->options->prefix_full_length) {
        ch = bx_fmt_engine_copy_rest(engine, ch);
        if (!engine->ok) {
            engine->next_char = EOF;
            return false;
        }
        if (ch == EOF) {
            engine->next_char = EOF;
            return false;
        }
        if (!bx_fmt_engine_writer_putc(engine, '\n')) {
            engine->next_char = EOF;
            return false;
        }
        ch = bx_fmt_engine_get_prefix(engine);
    }

    engine->prefix_indent = engine->next_prefix_indent;
    engine->first_indent = engine->in_column;
    engine->wptr = engine->parabuf;
    engine->word_limit = engine->words;
    ch = bx_fmt_engine_get_line(engine, ch);
    if (!engine->ok) {
        engine->next_char = EOF;
        return false;
    }
    bx_fmt_engine_set_other_indent(engine, bx_fmt_engine_same_paragraph(engine, ch));

    if (engine->options->split_only) {
        /* Nothing more to read for this paragraph. */
    } else if (engine->options->crown_margin) {
        if (bx_fmt_engine_same_paragraph(engine, ch)) {
            do {
                ch = bx_fmt_engine_get_line(engine, ch);
                if (!engine->ok) {
                    engine->next_char = EOF;
                    return false;
                }
            } while (bx_fmt_engine_same_paragraph(engine, ch) &&
                     engine->in_column == engine->other_indent);
        }
    } else if (engine->options->tagged_paragraph) {
        if (bx_fmt_engine_same_paragraph(engine, ch) &&
            engine->in_column != engine->first_indent) {
            do {
                ch = bx_fmt_engine_get_line(engine, ch);
                if (!engine->ok) {
                    engine->next_char = EOF;
                    return false;
                }
            } while (bx_fmt_engine_same_paragraph(engine, ch) &&
                     engine->in_column == engine->other_indent);
        }
    } else {
        while (bx_fmt_engine_same_paragraph(engine, ch) &&
               engine->in_column == engine->other_indent) {
            ch = bx_fmt_engine_get_line(engine, ch);
            if (!engine->ok) {
                engine->next_char = EOF;
                return false;
            }
        }
    }

    (engine->word_limit - 1)->period = 1u;
    (engine->word_limit - 1)->final = 1u;
    engine->next_char = ch;
    return true;
}

bool bx_fmt_engine_process_stream(FILE *stream,
                                  const struct bx_fmt_engine_options *options,
                                  struct bx_line_writer *writer,
                                  struct bx_diag_ctx *diag) {
    struct bx_fmt_engine engine = {
        .stream = stream,
        .options = options,
        .writer = writer,
        .diag = diag,
        .ok = true,
    };

    engine.other_indent = 0;
    engine.tabs = false;
    engine.next_char = bx_fmt_engine_get_prefix(&engine);
    while (engine.ok && bx_fmt_engine_get_paragraph(&engine)) {
        bx_fmt_engine_fmt_paragraph(&engine);
        if (!engine.ok) {
            break;
        }
        bx_fmt_engine_put_paragraph(&engine, engine.word_limit);
    }

    return engine.ok;
}
