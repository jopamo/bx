#define _GNU_SOURCE
#include "engine_internal.h"
#include <limits.h>

static curl_off_t add_resume_offset(curl_off_t value, curl_off_t offset) {
    if (value < 0 || offset <= 0)
        return value;
    if (value > (curl_off_t)(LLONG_MAX - (long long)offset))
        return (curl_off_t)LLONG_MAX;
    return value + offset;
}

static int64_t progress_count(curl_off_t value) {
    if (value < 0)
        return -1;
    if ((uint64_t)value > (uint64_t)INT64_MAX)
        return INT64_MAX;
    return (int64_t)value;
}

void bx_fetch_progress_emit(BxFetchTransfer* transfer, curl_off_t download_total, curl_off_t downloaded) {
    if (!transfer || !transfer->engine || !transfer->engine->observer.on_progress) {
        return;
    }

    curl_off_t display_total = add_resume_offset(download_total, transfer->progress_resume_offset);
    curl_off_t display_downloaded = add_resume_offset(downloaded, transfer->progress_resume_offset);

    double now = bx_fetch_monotonic_seconds();
    if (!transfer->progress_started) {
        transfer->progress_started = true;
        transfer->progress_start_s = now;
    }
    if (now <= 0.0)
        now = transfer->progress_start_s;

    double elapsed = now - transfer->progress_start_s;
    if (elapsed < 0.0)
        elapsed = 0.0;

    double speed = transfer->progress_last_speed_bps;
#ifdef CURLINFO_SPEED_DOWNLOAD_T
    if (speed <= 0.0 && transfer->easy) {
        curl_off_t current_speed = 0;
        if (curl_easy_getinfo(transfer->easy, CURLINFO_SPEED_DOWNLOAD_T, &current_speed) == CURLE_OK && current_speed > 0) {
            speed = (double)current_speed;
        }
    }
#endif
    if (speed <= 0.0 && elapsed > 0.0 && downloaded > 0)
        speed = (double)downloaded / elapsed;

    bool total_known = display_total > 0;
    int percent = 0;
    if (total_known && display_downloaded >= 0) {
        double ratio = (double)display_downloaded / (double)display_total;
        percent = ratio >= 1.0 ? 100 : (int)(ratio * 100.0);
    }

    double average_speed = elapsed > 0.0 && downloaded > 0 ? (double)downloaded / elapsed : 0.0;
    double eta_speed = average_speed > 0.0 ? average_speed : speed;
    double eta = -1.0;
    if (download_total > 0 && downloaded >= 0 && downloaded < download_total && eta_speed > 0.0) {
        eta = (double)(download_total - downloaded) / eta_speed;
    }

    const BxFetchProgress progress = {
        .total_known = total_known,
        .percent = percent,
        .downloaded_bytes = progress_count(display_downloaded),
        .total_bytes = progress_count(display_total),
        .bytes_per_second = speed,
        .elapsed_seconds = elapsed,
        .eta_seconds = eta,
    };
    transfer->engine->observer.on_progress(transfer->engine->observer.userdata, transfer->req, &progress);
}

int bx_fetch_progress_callback(void* userdata, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    BxFetchTransfer* transfer = userdata;
    (void)upload_total;
    (void)uploaded;
    if (!transfer)
        return 0;

    double now = bx_fetch_monotonic_seconds();
    if (!transfer->progress_started) {
        transfer->progress_started = true;
        transfer->progress_start_s = now;
    }

    if (now > 0.0 && transfer->progress_last_sample_s > 0.0 && downloaded >= transfer->progress_last_bytes) {
        double interval = now - transfer->progress_last_sample_s;
        if (interval > 0.0) {
            double instant_speed = (double)(downloaded - transfer->progress_last_bytes) / interval;
            if (instant_speed >= 0.0) {
                transfer->progress_last_speed_bps = transfer->progress_last_speed_bps <= 0.0 ? instant_speed : (transfer->progress_last_speed_bps * 0.70) + (instant_speed * 0.30);
            }
        }
    }
    transfer->progress_last_sample_s = now;
    transfer->progress_last_bytes = downloaded;

    if (transfer->progress_last_update_s <= 0.0 || (now - transfer->progress_last_update_s) >= 0.125) {
        bx_fetch_progress_emit(transfer, download_total, downloaded);
        transfer->progress_last_update_s = now;
    }

    return transfer->engine && transfer->engine->cancelled ? 1 : 0;
}
