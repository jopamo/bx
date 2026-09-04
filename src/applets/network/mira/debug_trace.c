#include "debug_trace.h"
#include <inttypes.h>

void bx_mira_json_write_string(FILE* stream, const char* value) {
    fputc('"', stream);
    for (const unsigned char* cursor = (const unsigned char*)value; *cursor; cursor++) {
        switch (*cursor) {
            case '"':
                fputs("\\\"", stream);
                break;
            case '\\':
                fputs("\\\\", stream);
                break;
            case '\b':
                fputs("\\b", stream);
                break;
            case '\f':
                fputs("\\f", stream);
                break;
            case '\n':
                fputs("\\n", stream);
                break;
            case '\r':
                fputs("\\r", stream);
                break;
            case '\t':
                fputs("\\t", stream);
                break;
            default:
                if (*cursor < 0x20)
                    fprintf(stream, "\\u%04x", *cursor);
                else
                    fputc(*cursor, stream);
                break;
        }
    }
    fputc('"', stream);
}

void bx_mira_debug_trace_init(MiraDebugTrace* trace, FILE* stream, const struct bx_fetch_config* config) {
    if (!trace)
        return;
    trace->stream = stream;
    trace->sequence = 0;
    trace->enabled = config && config->logging.debug_trace;
}

static void mira_debug_trace_prefix(MiraDebugTrace* trace, const char* layer, const char* action) {
    fprintf(trace->stream,
            "{\"schema_version\":1,\"event\":\"debug-trace\","
            "\"sequence\":%" PRIu64 ",\"layer\":",
            trace->sequence++);
    bx_mira_json_write_string(trace->stream, layer);
    fputs(",\"action\":", trace->stream);
    bx_mira_json_write_string(trace->stream, action);
}

void bx_mira_debug_trace_parse_complete(MiraDebugTrace* trace, const struct bx_fetch_config* config) {
    if (!trace || !trace->enabled || !config)
        return;
    mira_debug_trace_prefix(trace, "parse", "complete");
    fprintf(trace->stream,
            ",\"url_count\":%d,\"has_input_file\":%s,"
            "\"recursive\":%s,\"dry_run\":%s,\"max_threads\":%d}\n",
            config->input.url_count, config->input.input_file ? "true" : "false", config->recursive.recursive ? "true" : "false", config->download.dry_run ? "true" : "false",
            config->download.max_threads);
}

void bx_mira_debug_trace_dry_run_enqueued(MiraDebugTrace* trace, const char* display_url, const char* output_path, int depth) {
    if (!trace || !trace->enabled)
        return;
    mira_debug_trace_prefix(trace, "scheduler", "enqueued");
    fputs(",\"url\":", trace->stream);
    bx_mira_json_write_string(trace->stream, display_url);
    fputs(",\"output_path\":", trace->stream);
    bx_mira_json_write_string(trace->stream, output_path);
    fprintf(trace->stream, ",\"depth\":%d}\n", depth);
}

void bx_mira_debug_trace_dry_run_decision(MiraDebugTrace* trace, const char* display_url, const char* output_path) {
    if (!trace || !trace->enabled)
        return;
    mira_debug_trace_prefix(trace, "scheduler", "decision");
    fputs(",\"url\":", trace->stream);
    bx_mira_json_write_string(trace->stream, display_url);
    fputs(",\"output_path\":", trace->stream);
    bx_mira_json_write_string(trace->stream, output_path);
    fputs(",\"decision\":\"fetch\",\"reason\":\"dry-run\"}\n", trace->stream);
}
