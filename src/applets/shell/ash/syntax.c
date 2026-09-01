#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/syntax.h"

bool ash_source_location_valid(const struct ash_source_location* location) {
    return location != NULL &&
        location->source != NULL &&
        location->line != 0u &&
        location->column != 0u;
}

bool ash_source_location_is_none(const struct ash_source_location* location) {
    return location != NULL &&
        location->source == NULL &&
        location->identity == NULL &&
        location->line == 0u &&
        location->column == 0u &&
        location->offset == 0u;
}

static int ash_syntax_grow_array(
    void** items,
    size_t* capacity,
    size_t needed,
    size_t item_size
) {
    if (*capacity >= needed) {
        return 0;
    }

    size_t grown = (*capacity == 0u) ? 4u : *capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            grown = needed;
            break;
        }
        grown *= 2u;
    }
    if (item_size != 0u && grown > SIZE_MAX / item_size) {
        errno = ENOMEM;
        return -1;
    }

    void* replacement = realloc(*items, grown * item_size);
    if (replacement == NULL) {
        return -1;
    }
    *items = replacement;
    *capacity = grown;
    return 0;
}

void ash_word_init(struct ash_word* word, struct ash_source_location location) {
    *word = (struct ash_word){
        .location = location,
    };
}

void ash_word_destroy(struct ash_word* word) {
    if (word == NULL) {
        return;
    }
    for (size_t i = 0u; i < word->count; i++) {
        free(word->parts[i].text);
    }
    free(word->parts);
    *word = (struct ash_word){0};
}

int ash_word_clone(struct ash_word* destination, const struct ash_word* source) {
    ash_word_init(destination, source->location);
    for (size_t i = 0u; i < source->count; i++) {
        const struct ash_word_part* part = &source->parts[i];
        if (ash_word_add_part(
                destination,
                part->kind,
                part->quote,
                part->location,
                part->text,
                part->length
            ) != 0) {
            ash_word_destroy(destination);
            return -1;
        }
    }
    return 0;
}

static int ash_word_part_reserve(struct ash_word_part* part, size_t needed) {
    if (part->capacity >= needed) {
        return 0;
    }

    size_t grown = (part->capacity == 0u) ? 16u : part->capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            grown = needed;
            break;
        }
        grown *= 2u;
    }

    char* replacement = realloc(part->text, grown);
    if (replacement == NULL) {
        return -1;
    }
    part->text = replacement;
    part->capacity = grown;
    return 0;
}

bool ash_word_part_is_quoted(const struct ash_word_part* part) {
    return part != NULL && part->quote != ASH_QUOTE_NONE;
}

bool ash_word_part_is_expansion(const struct ash_word_part* part) {
    return part != NULL && part->kind != ASH_WORD_TEXT;
}

static bool ash_word_part_kind_valid(enum ash_word_part_kind kind) {
    return kind >= ASH_WORD_TEXT &&
        kind <= ASH_WORD_PROCESS_SUBSTITUTION;
}

static bool ash_quote_kind_valid(enum ash_quote_kind quote) {
    return quote >= ASH_QUOTE_NONE && quote < ASH_QUOTE_COUNT;
}

static bool ash_word_part_spec_valid(
    enum ash_word_part_kind kind,
    enum ash_quote_kind quote
) {
    return ash_word_part_kind_valid(kind) &&
        ash_quote_kind_valid(quote) &&
        (kind == ASH_WORD_TEXT ||
         (quote != ASH_QUOTE_BACKSLASH &&
          quote != ASH_QUOTE_SINGLE &&
          quote != ASH_QUOTE_DOLLAR_SINGLE));
}

static int ash_word_part_append(
    struct ash_word_part* part,
    const char* text,
    size_t length
) {
    if (length > SIZE_MAX - part->length - 1u ||
        ash_word_part_reserve(part, part->length + length + 1u) != 0) {
        errno = ENOMEM;
        return -1;
    }
    if (length != 0u) {
        memcpy(part->text + part->length, text, length);
    }
    part->length += length;
    part->text[part->length] = '\0';
    return 0;
}

int ash_word_add_part(
    struct ash_word* word,
    enum ash_word_part_kind kind,
    enum ash_quote_kind quote,
    struct ash_source_location location,
    const char* text,
    size_t length
) {
    if (word == NULL || !ash_word_part_spec_valid(kind, quote) ||
        !ash_source_location_valid(&location) ||
        (text == NULL && length != 0u)) {
        errno = EINVAL;
        return -1;
    }
    if (word->count == SIZE_MAX) {
        errno = ENOMEM;
        return -1;
    }

    if (ash_syntax_grow_array(
            (void**)&word->parts,
            &word->capacity,
            word->count + 1u,
            sizeof(*word->parts)
        ) != 0) {
        return -1;
    }
    struct ash_word_part* part = &word->parts[word->count];
    *part = (struct ash_word_part){
        .kind = kind,
        .quote = quote,
        .location = location,
    };
    if (ash_word_part_append(part, text, length) != 0) {
        *part = (struct ash_word_part){0};
        return -1;
    }
    word->count++;
    return 0;
}

int ash_word_extend_last_part(
    struct ash_word* word,
    enum ash_word_part_kind expected_kind,
    enum ash_quote_kind expected_quote,
    const char* text,
    size_t length
) {
    if (word == NULL || word->count == 0u ||
        !ash_word_part_spec_valid(expected_kind, expected_quote) ||
        (text == NULL && length != 0u)) {
        errno = EINVAL;
        return -1;
    }
    struct ash_word_part* part = &word->parts[word->count - 1u];
    if (part->kind != expected_kind || part->quote != expected_quote) {
        errno = EINVAL;
        return -1;
    }
    return ash_word_part_append(part, text, length);
}
