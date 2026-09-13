#define _GNU_SOURCE
#include "progress.h"
#include "lib/path_ops.h"
#include "lib/path_quote.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct MiraProgressTransfer {
    uint64_t id;
    BxFetchProgressSample sample;
    uint64_t initial_received;
    double started;
    double last_record;
    char* label;
    struct MiraProgressTransfer* next;
};

static double monotonic_now(void* userdata) {
    (void)userdata;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

void bx_mira_progress_init(MiraProgressRenderer* renderer, FILE* diagnostics, bool enabled, MiraProgressClock clock, void* clock_userdata) {
    *renderer = (MiraProgressRenderer){
        .diagnostics = diagnostics,
        .enabled = enabled,
        .terminal = isatty(fileno(diagnostics)) == 1,
        .clock = clock ? clock : monotonic_now,
        .clock_userdata = clock_userdata,
    };
}

void bx_mira_progress_interrupt(MiraProgressRenderer* renderer) {
    if (renderer->line_active) {
        fputs("\r\033[K", renderer->diagnostics);
        fflush(renderer->diagnostics);
        renderer->line_active = false;
    }
}

static void format_bytes(char* buffer, size_t size, double bytes) {
    static const char* const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    size_t unit = 0;
    while (bytes >= 1024 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        bytes /= 1024;
        unit++;
    }
    snprintf(buffer, size, unit == 0 ? "%.0f %s" : "%.1f %s", bytes, units[unit]);
}

void bx_mira_progress_sample(MiraProgressRenderer* renderer, const BxFetchRunProgressObservation* observation) {
    if (!renderer->enabled)
        return;
    MiraProgressTransfer* transfer = renderer->transfers;
    while (transfer && transfer->id != observation->transfer_id)
        transfer = transfer->next;
    bool fresh = transfer == NULL;
    if (fresh) {
        transfer = calloc(1, sizeof(*transfer));
        if (!transfer)
            return; /* Progress is optional; completion still reports the outcome. */
        const char* path = bx_fetch_prepared_url_path(bx_fetch_request_target(observation->request));
        const char* name = path ? bx_path_basename_ptr(path) : NULL;
        transfer->label = bx_path_quote_dup(name && *name ? name : "(download)", BX_PATH_QUOTE_ESCAPE);
        if (!transfer->label) {
            free(transfer);
            return;
        }
        transfer->id = observation->transfer_id;
        transfer->next = renderer->transfers;
        renderer->transfers = transfer;
    }
    const BxFetchProgressSample* sample = &observation->sample;
    double now = renderer->clock(renderer->clock_userdata);
    bool reset = fresh || sample->generation != transfer->sample.generation;
    bool changed =
        reset || sample->total_known != transfer->sample.total_known || sample->total_bytes != transfer->sample.total_bytes || sample->accepted_prefix_bytes != transfer->sample.accepted_prefix_bytes;
    if (reset) {
        transfer->started = now;
        transfer->initial_received = sample->received_bytes;
    }
    transfer->sample = *sample;
    if (fresh && !renderer->terminal) {
        fprintf(renderer->diagnostics, "mira: receiving %s attempt %d/%d\n", transfer->label, observation->attempt, observation->max_attempts);
    }
    double last = renderer->terminal ? renderer->last_draw : transfer->last_record;
    double interval = renderer->terminal ? 0.2 : 5.0;
    if (!changed && now - last < interval)
        return;

    double elapsed = now > transfer->started ? now - transfer->started : 0;
    double rate = elapsed > 0 && sample->received_bytes >= transfer->initial_received ? (double)(sample->received_bytes - transfer->initial_received) / elapsed : 0;
    /* Sum in floating point for presentation; never overflow a byte counter. */
    double completed = (double)sample->accepted_prefix_bytes + (double)sample->received_bytes;
    char bytes[48], total[48], speed[48], size_text[128], timing[128], resume[80] = "";
    format_bytes(bytes, sizeof(bytes), completed);
    format_bytes(total, sizeof(total), (double)sample->total_bytes);
    format_bytes(speed, sizeof(speed), rate);
    if (sample->total_known && sample->total_bytes > 0) {
        double percent = completed / (double)sample->total_bytes * 100;
        bool incomplete = sample->accepted_prefix_bytes < sample->total_bytes && sample->received_bytes < sample->total_bytes - sample->accepted_prefix_bytes;
        if (incomplete && percent >= 100)
            percent = 99; /* Floating-point rounding must not finish an object. */
        snprintf(size_text, sizeof(size_text), "%s / %s | %.0f%%", bytes, total, floor(percent));
    }
    else {
        snprintf(size_text, sizeof(size_text), sample->total_known ? "%s / %s" : "%s", bytes, total);
    }
    if (sample->accepted_prefix_bytes) {
        char prefix[48];
        format_bytes(prefix, sizeof(prefix), (double)sample->accepted_prefix_bytes);
        snprintf(resume, sizeof(resume), " | resumed at %s", prefix);
    }
    if (sample->total_known && (double)sample->total_bytes > completed && rate > 0) {
        snprintf(timing, sizeof(timing), " | %s/s | ETA %.0fs", speed, ((double)sample->total_bytes - completed) / rate);
    }
    else if (rate > 0) {
        snprintf(timing, sizeof(timing), " | %s/s | %.0fs", speed, elapsed);
    }
    else {
        snprintf(timing, sizeof(timing), " | %.0fs", elapsed);
    }
    char line[1024];
    snprintf(line, sizeof(line), "mira: %s %s%s%s | %s", renderer->terminal ? "receiving" : "progress", size_text, resume, timing, transfer->label);
    if (renderer->terminal) {
        struct winsize window = {0};
        unsigned columns = 80;
        if (ioctl(fileno(renderer->diagnostics), TIOCGWINSZ, &window) == 0 && window.ws_col > 1)
            columns = window.ws_col;
        /* Leave the final column unused so a redraw never wraps. Quoted names
         * contain no terminal controls; truncation cannot expose a raw escape. */
        fprintf(renderer->diagnostics, "\r\033[K%.*s", (int)columns - 1, line);
        renderer->line_active = true;
        renderer->last_draw = now;
    }
    else {
        fprintf(renderer->diagnostics, "%s\n", line);
    }
    fflush(renderer->diagnostics);
    transfer->last_record = now;
}

void bx_mira_progress_complete(MiraProgressRenderer* renderer, const BxFetchRunCompletion* completion, bool report) {
    /* A different attempt may own the visible line. Clear it too, but retain
     * its estimator; its next observation can redraw without mixing records. */
    bx_mira_progress_interrupt(renderer);
    MiraProgressTransfer** link = &renderer->transfers;
    while (*link && (*link)->id != completion->transfer_id)
        link = &(*link)->next;
    if (*link) {
        MiraProgressTransfer* transfer = *link;
        *link = transfer->next;
        free(transfer->label);
        free(transfer);
    }
    if (!report || completion->retry_scheduled)
        return;
    const BxFetchTransferCompletion* transfer = completion->transfer;
    BxFetchOutputState output = transfer->response ? transfer->response->output_state : BX_FETCH_OUTPUT_STATE_NONE;
    const char* outcome;
    switch (output) {
        case BX_FETCH_OUTPUT_STATE_COMMITTED:
            outcome = "saved";
            break;
        case BX_FETCH_OUTPUT_STATE_METADATA_COMMITTED:
            outcome = "metadata refreshed (payload unchanged)";
            break;
        case BX_FETCH_OUTPUT_STATE_UNCHANGED:
            outcome = "unchanged";
            break;
        case BX_FETCH_OUTPUT_STATE_COMMIT_FAILED:
            outcome = "save/commit failed";
            break;
        default:
            outcome = transfer->result == BX_FETCH_ERROR_CANCELLED ? "cancelled" : transfer->result == BX_FETCH_OK ? "finished (no output committed)" : "failed";
            break;
    }
    char* path = bx_path_quote_dup(transfer->output_path ? transfer->output_path : "(output unavailable)", BX_PATH_QUOTE_ESCAPE);
    fprintf(renderer->diagnostics, "mira: %s %s\n", outcome, path ? path : "(path unavailable)");
    free(path);
}

void bx_mira_progress_destroy(MiraProgressRenderer* renderer) {
    bx_mira_progress_interrupt(renderer);
    while (renderer->transfers) {
        MiraProgressTransfer* transfer = renderer->transfers;
        renderer->transfers = transfer->next;
        free(transfer->label);
        free(transfer);
    }
}
