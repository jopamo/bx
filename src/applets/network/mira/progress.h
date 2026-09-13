#ifndef BX_MIRA_PROGRESS_H
#define BX_MIRA_PROGRESS_H

#include "lib/fetch/run.h"
#include <stdio.h>

typedef double (*MiraProgressClock)(void* userdata);
typedef struct MiraProgressTransfer MiraProgressTransfer;

typedef struct {
    FILE* diagnostics;
    bool enabled;
    bool terminal;
    bool line_active;
    double last_draw;
    MiraProgressClock clock;
    void* clock_userdata;
    MiraProgressTransfer* transfers;
} MiraProgressRenderer;

/* Stream and clock userdata are borrowed. NULL clock selects CLOCK_MONOTONIC. */
void bx_mira_progress_init(MiraProgressRenderer* renderer, FILE* diagnostics, bool enabled, MiraProgressClock clock, void* clock_userdata);
void bx_mira_progress_sample(MiraProgressRenderer* renderer, const BxFetchRunProgressObservation* observation);
/* Clear a redraw before any diagnostic, independently of progress throttling. */
void bx_mira_progress_interrupt(MiraProgressRenderer* renderer);
void bx_mira_progress_complete(MiraProgressRenderer* renderer, const BxFetchRunCompletion* completion, bool report);
void bx_mira_progress_destroy(MiraProgressRenderer* renderer);

#endif
