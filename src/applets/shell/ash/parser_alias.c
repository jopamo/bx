#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/aliases.h"
#include "applets/shell/ash/parser_internal.h"

static struct ash_lexer* ash_parser_active_lexer(
    struct ash_parser* parser
) {
    if (parser->alias_frame_count == 0u) {
        return &parser->lexer;
    }
    return &parser->alias_frames[
        parser->alias_frame_count - 1u
    ].lexer;
}

static void ash_parser_pop_alias(
    struct ash_parser* parser,
    bool preserve_continuation
) {
    struct ash_parser_alias_frame* frame =
        &parser->alias_frames[parser->alias_frame_count - 1u];
    if (preserve_continuation) {
        parser->continue_alias |= frame->continue_alias;
    }
    free(frame->owned_input);
    free(frame->releases);
    *frame = (struct ash_parser_alias_frame){0};
    parser->alias_frame_count--;
}

static enum ash_parser_result ash_parser_fill(struct ash_parser* parser) {
    if (parser->result != ASH_PARSER_COMPLETE) {
        return parser->result;
    }
    if (parser->has_lookahead) {
        return ASH_PARSER_COMPLETE;
    }

    while (true) {
        struct ash_lexer* lexer = ash_parser_active_lexer(parser);
        if (parser->alias_frame_count != 0u) {
            struct ash_parser_alias_frame* frame =
                &parser->alias_frames[
                    parser->alias_frame_count - 1u
                ];
            if (frame->owned_input != NULL &&
                frame->alias_active &&
                frame->lexer.offset >= frame->alias_length) {
                frame->alias_active = false;
            }
            for (size_t i = 0u; i < frame->release_count; i++) {
                if (frame->releases[i].offset >
                    frame->lexer.offset) {
                    continue;
                }
                for (size_t j = 0u;
                     j + 1u < parser->alias_frame_count;
                     j++) {
                    if (parser->alias_frames[j].alias ==
                        frame->releases[i].alias) {
                        parser->alias_frames[j].alias_active = false;
                    }
                }
            }
        }
        enum ash_lexer_result result = ash_lexer_next(
            lexer,
            &parser->lookahead
        );
        if (parser->alias_frame_count != 0u &&
            ash_lexer_discarded_comment(lexer)) {
            parser->alias_comment_elided = true;
        }
        if (result == ASH_LEXER_INCOMPLETE) {
            ash_token_destroy(&parser->lookahead);
            return ash_parser_fail(
                parser,
                ASH_PARSER_INCOMPLETE,
                lexer->error_location,
                lexer->error
            );
        }
        if (result == ASH_LEXER_ERROR) {
            ash_token_destroy(&parser->lookahead);
            return ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                lexer->error_location,
                lexer->error
            );
        }
        if (result != ASH_LEXER_END ||
            parser->alias_frame_count == 0u) {
            break;
        }

        ash_token_destroy(&parser->lookahead);
        bool comment_continues =
            ash_lexer_ended_in_comment(lexer);
        ash_parser_pop_alias(parser, !comment_continues);
        while (comment_continues) {
            lexer = ash_parser_active_lexer(parser);
            if (!ash_lexer_discard_comment_tail(lexer)) {
                comment_continues = false;
            }
            else if (parser->alias_frame_count != 0u) {
                ash_parser_pop_alias(parser, false);
            }
            else {
                comment_continues = false;
            }
        }
    }
    parser->has_lookahead = true;
    parser->lookahead_alias_checked = false;
    return ASH_PARSER_COMPLETE;
}

struct ash_token* ash_parser_peek(struct ash_parser* parser) {
    if (ash_parser_fill(parser) != ASH_PARSER_COMPLETE) {
        return NULL;
    }
    return &parser->lookahead;
}

bool ash_parser_take(struct ash_parser* parser, struct ash_token* token) {
    if (ash_parser_fill(parser) != ASH_PARSER_COMPLETE) {
        return false;
    }
    *token = parser->lookahead;
    parser->lookahead = (struct ash_token){0};
    parser->has_lookahead = false;
    parser->lookahead_alias_checked = false;
    if (token->kind == ASH_TOKEN_NEWLINE) {
        parser->alias_comment_elided = false;
    }
    return true;
}

static bool ash_parser_alias_is_active(
    const struct ash_parser* parser,
    const struct ash_alias* alias
) {
    for (size_t i = 0u; i < parser->alias_frame_count; i++) {
        if (parser->alias_frames[i].alias_active &&
            parser->alias_frames[i].alias == alias) {
            return true;
        }
    }
    return false;
}

static bool ash_parser_reserve_alias_frame(
    struct ash_parser* parser
) {
    if (parser->alias_frame_count < parser->alias_frame_capacity) {
        return true;
    }
    if (parser->alias_frame_capacity > SIZE_MAX / 2u ||
        parser->alias_frame_capacity * 2u >
            SIZE_MAX / sizeof(parser->alias_frames[0])) {
        return false;
    }

    size_t capacity = parser->alias_frame_capacity * 2u;
    struct ash_parser_alias_frame* frames;
    if (parser->alias_frames == parser->inline_alias_frames) {
        frames = malloc(capacity * sizeof(frames[0]));
        if (frames != NULL) {
            memcpy(
                frames,
                parser->inline_alias_frames,
                parser->alias_frame_count * sizeof(frames[0])
            );
        }
    }
    else {
        frames = realloc(
            parser->alias_frames,
            capacity * sizeof(frames[0])
        );
    }
    if (frames == NULL) {
        return false;
    }
    parser->alias_frames = frames;
    parser->alias_frame_capacity = capacity;
    return true;
}

static bool ash_parser_push_alias(
    struct ash_parser* parser,
    const struct ash_alias* alias,
    struct ash_source_location location
) {
    size_t alias_length = ash_alias_value_length(alias);
    size_t tail_length = 0u;
    bool bridge_tail = ash_alias_requires_tail(
        alias,
        &parser->lexer.options
    );
    if (bridge_tail) {
        for (size_t i = parser->alias_frame_count; i != 0u; i--) {
            const struct ash_lexer* lexer =
                &parser->alias_frames[i - 1u].lexer;
            size_t remaining = lexer->length - lexer->offset;
            if (remaining > SIZE_MAX - tail_length) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    location,
                    "out of memory"
                );
                return false;
            }
            tail_length += remaining;
        }
        size_t remaining =
            parser->lexer.length - parser->lexer.offset;
        if (remaining > SIZE_MAX - tail_length) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                location,
                "out of memory"
            );
            return false;
        }
        tail_length += remaining;
    }
    if (alias_length > SIZE_MAX - tail_length) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            location,
            "out of memory"
        );
        return false;
    }
    size_t length = alias_length + tail_length;
    if (location.offset > SIZE_MAX - length) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            location,
            "alias expansion location overflow"
        );
        return false;
    }

    char* owned_input = NULL;
    struct ash_parser_alias_release* releases = NULL;
    size_t release_count = 0u;
    const char* input = ash_alias_value(alias);
    if (bridge_tail) {
        if (length == SIZE_MAX) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                location,
                "out of memory"
            );
            return false;
        }
        if (parser->alias_frame_count >
            SIZE_MAX / sizeof(releases[0])) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                location,
                "out of memory"
            );
            return false;
        }
        owned_input = malloc(length + 1u);
        releases = parser->alias_frame_count != 0u ?
            malloc(
                parser->alias_frame_count *
                    sizeof(releases[0])
            ) :
            NULL;
        if (owned_input == NULL ||
            (parser->alias_frame_count != 0u &&
             releases == NULL)) {
            free(owned_input);
            free(releases);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                location,
                "out of memory"
            );
            return false;
        }
        memcpy(owned_input, input, alias_length);
        size_t offset = alias_length;
        /*
         * An incomplete replacement and every suspended token source form
         * one lexical stream. Flatten only this rare bridge path; ordinary
         * aliases continue to borrow their immutable table value.
         *
         * The older frames remain on the stack with exhausted lexers. Each
         * release marker records where its replacement ends in the bridge,
         * so recursion suppression lasts exactly as long as the original
         * source would have remained active.
         */
        for (size_t i = parser->alias_frame_count; i != 0u; i--) {
            const struct ash_lexer* lexer =
                &parser->alias_frames[i - 1u].lexer;
            const struct ash_parser_alias_frame* source =
                &parser->alias_frames[i - 1u];
            size_t remaining = lexer->length - lexer->offset;
            size_t segment_start = offset;
            if (source->alias_active) {
                bool duplicate = false;
                for (size_t j = 0u; j < release_count; j++) {
                    if (releases[j].alias == source->alias) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    size_t release_offset =
                        source->owned_input != NULL ?
                            segment_start +
                                (source->alias_length >
                                        lexer->offset ?
                                    source->alias_length -
                                        lexer->offset :
                                    0u) :
                            segment_start + remaining;
                    releases[release_count++] =
                        (struct ash_parser_alias_release){
                            .alias = source->alias,
                            .offset = release_offset,
                        };
                }
            }
            if (source->owned_input != NULL) {
                for (size_t j = 0u;
                     j < source->release_count;
                     j++) {
                    const struct ash_parser_alias_release* inherited =
                        &source->releases[j];
                    bool duplicate = false;
                    for (size_t k = 0u;
                         k < release_count;
                         k++) {
                        if (releases[k].alias == inherited->alias) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        releases[release_count++] =
                            (struct ash_parser_alias_release){
                                .alias = inherited->alias,
                                .offset = segment_start +
                                    (inherited->offset >
                                            lexer->offset ?
                                        inherited->offset -
                                            lexer->offset :
                                        0u),
                            };
                    }
                }
            }
            memcpy(
                owned_input + offset,
                lexer->input + lexer->offset,
                remaining
            );
            offset += remaining;
        }
        size_t remaining =
            parser->lexer.length - parser->lexer.offset;
        memcpy(
            owned_input + offset,
            parser->lexer.input + parser->lexer.offset,
            remaining
        );
        offset += remaining;
        owned_input[offset] = '\0';
        input = owned_input;
    }
    if (!ash_parser_reserve_alias_frame(parser)) {
        free(owned_input);
        free(releases);
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            location,
            "out of memory"
        );
        return false;
    }

    struct ash_parser_alias_frame* frame =
        &parser->alias_frames[parser->alias_frame_count++];
    *frame = (struct ash_parser_alias_frame){
        .alias = alias,
        .continue_alias = ash_alias_value_ends_blank(alias),
        .alias_active = true,
        .alias_length = alias_length,
        .owned_input = owned_input,
        .releases = releases,
        .release_count = release_count,
    };
    if (bridge_tail) {
        for (size_t i = 0u; i + 1u < parser->alias_frame_count; i++) {
            ash_lexer_discard_remaining(
                &parser->alias_frames[i].lexer
            );
        }
        ash_lexer_discard_remaining(&parser->lexer);
    }
    ash_lexer_init_at_with_options(
        &frame->lexer,
        location,
        input,
        length,
        &parser->lexer.options
    );
    return true;
}

bool ash_parser_prepare_alias(
    struct ash_parser* parser,
    bool command_position,
    bool reserved_word_precedes_alias,
    ash_parser_reserved_word_fn is_reserved_word
) {
    if (parser->aliases == NULL) {
        return true;
    }

    while (true) {
        struct ash_token* token = ash_parser_peek(parser);
        if (token == NULL) {
            return false;
        }
        bool eligible = command_position || parser->continue_alias;
        if (!eligible || token->kind != ASH_TOKEN_WORD ||
            parser->lookahead_alias_checked) {
            return true;
        }

        parser->continue_alias = false;
        if (reserved_word_precedes_alias &&
            is_reserved_word != NULL &&
            is_reserved_word(token)) {
            parser->lookahead_alias_checked = true;
            return true;
        }

        const struct ash_alias* alias = ash_alias_find_word(
            parser->aliases,
            &token->word
        );
        if (alias == NULL ||
            ash_parser_alias_is_active(parser, alias)) {
            parser->lookahead_alias_checked = true;
            return true;
        }

        struct ash_source_location location = token->location;
        ash_token_destroy(&parser->lookahead);
        parser->has_lookahead = false;
        parser->lookahead_alias_checked = false;
        if (!ash_parser_push_alias(parser, alias, location)) {
            return false;
        }
    }
}

bool ash_parser_prepare_command_alias(
    struct ash_parser* parser,
    ash_parser_reserved_word_fn is_reserved_word
) {
    while (true) {
        if (!ash_parser_prepare_alias(
                parser,
                true,
                true,
                is_reserved_word
            )) {
            return false;
        }
        if (!parser->alias_comment_elided) {
            return true;
        }

        parser->alias_comment_elided = false;
        struct ash_token* token = ash_parser_peek(parser);
        while (token != NULL &&
               token->kind == ASH_TOKEN_NEWLINE) {
            struct ash_token newline;
            (void)ash_parser_take(parser, &newline);
            ash_token_destroy(&newline);
            token = ash_parser_peek(parser);
        }
        if (token == NULL) {
            return false;
        }
    }
}

void ash_parser_alias_state_init(
    struct ash_parser* parser,
    const struct ash_alias_table* aliases
) {
    parser->aliases = aliases;
    parser->alias_frames = parser->inline_alias_frames;
    parser->alias_frame_capacity = ASH_PARSER_INLINE_ALIAS_FRAMES;
}

void ash_parser_alias_state_destroy(struct ash_parser* parser) {
    if (parser->has_lookahead) {
        ash_token_destroy(&parser->lookahead);
    }
    parser->has_lookahead = false;
    parser->lookahead_alias_checked = false;
    for (size_t i = 0u; i < parser->alias_frame_count; i++) {
        free(parser->alias_frames[i].owned_input);
        free(parser->alias_frames[i].releases);
    }
    if (parser->alias_frames != parser->inline_alias_frames) {
        free(parser->alias_frames);
    }
    parser->aliases = NULL;
    parser->alias_frames = NULL;
    parser->alias_frame_count = 0u;
    parser->alias_frame_capacity = 0u;
    parser->continue_alias = false;
    parser->alias_comment_elided = false;
}
