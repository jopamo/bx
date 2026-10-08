#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/here_document.h"
#include "applets/shell/ash/parser_internal.h"
#include "applets/shell/ash/quote.h"
#include "lib/text_buffer.h"

static bool ash_parser_reserve_here_documents(
    struct ash_parser* parser
) {
    if (parser->pending_here_document_count <
        parser->pending_here_document_capacity) {
        return true;
    }
    size_t capacity = parser->pending_here_document_capacity;
    if (capacity == 0u) {
        capacity = 4u;
    }
    else {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    if (capacity >
        SIZE_MAX / sizeof(parser->pending_here_documents[0])) {
        return false;
    }
    struct ash_here_document** documents = realloc(
        parser->pending_here_documents,
        capacity * sizeof(documents[0])
    );
    if (documents == NULL) {
        return false;
    }
    parser->pending_here_documents = documents;
    parser->pending_here_document_capacity = capacity;
    return true;
}

static struct ash_here_document* ash_parser_create_here_document(
    const struct ash_redirection* redirection
) {
    const struct ash_word* target = &redirection->target.syntax;
    struct bx_text_buffer delimiter;
    bx_text_buffer_init(&delimiter);
    bool delimiter_quoted = false;
    for (size_t i = 0u; i < target->count; i++) {
        const struct ash_word_part* part = &target->parts[i];
        bool appended = part->kind == ASH_WORD_TEXT && part->quote == ASH_QUOTE_DOLLAR_SINGLE ? ash_quote_append_dollar_single(&delimiter, part->text, part->length)
                                                                                              : bx_text_buffer_append_span(&delimiter, part->text, part->length);
        if (!appended) {
            bx_text_buffer_destroy(&delimiter);
            return NULL;
        }
        delimiter_quoted |= ash_word_part_is_quoted(part);
    }

    struct ash_here_document* document = calloc(
        1u,
        sizeof(*document)
    );
    size_t delimiter_length = delimiter.length;
    char* spelling = bx_text_buffer_take(&delimiter);
    if (document == NULL || spelling == NULL) {
        free(document);
        free(spelling);
        bx_text_buffer_destroy(&delimiter);
        return NULL;
    }
    *document = (struct ash_here_document){
        .state = ASH_HERE_DOCUMENT_PENDING,
        .delimiter = spelling,
        .delimiter_length = delimiter_length,
        .operator_location = redirection->location,
        .delimiter_quoted = delimiter_quoted,
        .strip_tabs = redirection->operator == ASH_TOKEN_DLESS_DASH,
    };
    return document;
}

bool ash_parser_register_here_document(
    struct ash_parser* parser,
    struct ash_redirection* redirection
) {
    if (redirection->operator != ASH_TOKEN_DLESS &&
        redirection->operator != ASH_TOKEN_DLESS_DASH) {
        return true;
    }
    if (redirection->here_document != NULL ||
        !redirection->target.has_syntax) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            redirection->location,
            "invalid here-document registration"
        );
        return false;
    }

    struct ash_here_document* document =
        ash_parser_create_here_document(redirection);
    if (document == NULL ||
        !ash_parser_reserve_here_documents(parser)) {
        if (document != NULL) {
            free(document->delimiter);
            free(document);
        }
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            redirection->location,
            "out of memory"
        );
        return false;
    }
    redirection->here_document = document;
    parser->pending_here_documents[
        parser->pending_here_document_count++
    ] = document;
    return true;
}

static bool ash_parser_complete_here_document(
    struct ash_parser* parser,
    struct ash_here_document* document
) {
    struct bx_text_buffer body;
    struct bx_text_buffer logical_line;
    struct bx_text_buffer physical_line;
    bx_text_buffer_init(&body);
    bx_text_buffer_init(&logical_line);
    bx_text_buffer_init(&physical_line);

    struct ash_source_location candidate_location = {0};
    struct ash_source_location body_location = {0};
    size_t candidate_start = 0u;
    bool candidate_active = false;
    bool body_started = false;
    while (true) {
        struct ash_source_location line_location;
        enum ash_parser_raw_line_result result =
            ash_parser_take_raw_line(
                parser,
                &physical_line,
                &line_location
            );
        if (result != ASH_PARSER_RAW_LINE) {
            if (result == ASH_PARSER_RAW_END) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_INCOMPLETE,
                    candidate_active ?
                        candidate_location :
                        (struct ash_source_location){
                            .source = parser->lexer.source_name,
                            .identity = parser->lexer.source_identity,
                            .line = parser->lexer.line,
                            .column = parser->lexer.column,
                            .offset = parser->lexer.source_offset +
                                parser->lexer.offset,
                        },
                    "here-document delimited by end-of-file"
                );
            }
            bx_text_buffer_destroy(&physical_line);
            bx_text_buffer_destroy(&logical_line);
            bx_text_buffer_destroy(&body);
            return false;
        }

        if (!candidate_active) {
            candidate_location = line_location;
            candidate_start = body.length;
            candidate_active = true;
        }
        /*
         * Preserve exact source bytes in the body candidate. If its logical
         * form matches the delimiter, truncating to candidate_start commits
         * all preceding body bytes without copying them again.
         */
        if (!bx_text_buffer_append_span(
                &body,
                physical_line.data,
                physical_line.length
            )) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                line_location,
                "out of memory"
            );
            bx_text_buffer_destroy(&physical_line);
            bx_text_buffer_destroy(&logical_line);
            bx_text_buffer_destroy(&body);
            return false;
        }

        enum ash_here_document_line_result match =
            ash_here_document_match_line(&logical_line, physical_line.data, physical_line.length, document->delimiter, document->delimiter_length, document->delimiter_quoted, document->strip_tabs);
        if (match == ASH_HERE_DOCUMENT_LINE_ERROR) {
            ash_parser_fail(parser, ASH_PARSER_ERROR, line_location, "out of memory");
            bx_text_buffer_destroy(&physical_line);
            bx_text_buffer_destroy(&logical_line);
            bx_text_buffer_destroy(&body);
            return false;
        }
        if (match == ASH_HERE_DOCUMENT_LINE_CONTINUED) {
            continue;
        }
        bool delimiter = match == ASH_HERE_DOCUMENT_LINE_DELIMITER;
        if (delimiter) {
            body.length = candidate_start;
            body.data[body.length] = '\0';
            size_t body_length = body.length;
            document->body = bx_text_buffer_take(&body);
            if (document->body == NULL) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    candidate_location,
                    "out of memory"
                );
                bx_text_buffer_destroy(&physical_line);
                bx_text_buffer_destroy(&logical_line);
                bx_text_buffer_destroy(&body);
                return false;
            }
            document->body_length = body_length;
            document->body_location = body_started ?
                body_location : candidate_location;
            document->end_location = line_location;
            document->state = ASH_HERE_DOCUMENT_COMPLETE;
            bx_text_buffer_destroy(&physical_line);
            bx_text_buffer_destroy(&logical_line);
            return true;
        }

        if (!body_started) {
            body_location = candidate_location;
            body_started = true;
        }
        candidate_active = false;
    }
}

bool ash_parser_consume_here_documents(struct ash_parser* parser) {
    size_t count = parser->pending_here_document_count;
    for (size_t i = 0u; i < count; i++) {
        if (!ash_parser_complete_here_document(
                parser,
                parser->pending_here_documents[i]
            )) {
            return false;
        }
    }
    parser->pending_here_document_count = 0u;
    return true;
}

bool ash_parser_has_pending_here_documents(
    const struct ash_parser* parser
) {
    return parser->pending_here_document_count != 0u;
}

void ash_parser_discard_pending_here_documents(
    struct ash_parser* parser
) {
    parser->pending_here_document_count = 0u;
}

void ash_parser_here_document_state_destroy(
    struct ash_parser* parser
) {
    free(parser->pending_here_documents);
    parser->pending_here_documents = NULL;
    parser->pending_here_document_count = 0u;
    parser->pending_here_document_capacity = 0u;
}
