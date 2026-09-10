/*
 * Frame instrumentation. See gauge_perf.h and docs/performance.md.
 *
 * All callbacks run on the LVGL task, so the accumulators need no locking against each
 * other. gauge_perf_get() can be called from another task, hence the snapshot copy.
 */

#include "gauge_perf.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gauge_perf";

static struct {
    bool     attached;
    uint32_t panel_px;
    uint8_t  bytes_per_px;

    /* Accumulated for the frame currently being composed. */
    uint32_t pending_dirty_px;
    int64_t  render_start_us;

    /* Window accumulators. */
    int64_t  window_start_us;
    uint32_t frames;
    uint32_t skipped;
    uint64_t dirty_px_total;
    uint32_t dirty_px_max;
    uint64_t render_us_total;
    uint32_t render_us_max;

    char     label[32];
    char     report_tag[24];
} s;

/*
 * Measured at flush, not at invalidation. Invalidation events fire once per request and
 * overlap freely -- summing them reported over 100% of the screen per frame, which is not a
 * real quantity. The flushed areas are what actually crosses the QSPI bus.
 */
static void on_flush_start(lv_event_t *e)
{
    const lv_area_t *area = lv_event_get_param(e);
    if (area != NULL) {
        s.pending_dirty_px += lv_area_get_size(area);
    }
}

static void on_render_start(lv_event_t *e)
{
    (void)e;
    s.render_start_us = esp_timer_get_time();
}

static void on_refr_ready(lv_event_t *e)
{
    (void)e;

    /*
     * REFR_START/READY fire on every refresh cycle even when nothing changed, so a frame
     * only counts if something was actually invalidated. Counting idle cycles would report
     * a flattering FPS that says nothing about render cost.
     */
    if (s.pending_dirty_px == 0) {
        s.skipped++;
        return;
    }

    uint32_t render_us = (uint32_t)(esp_timer_get_time() - s.render_start_us);

    s.frames++;
    s.dirty_px_total += s.pending_dirty_px;
    if (s.pending_dirty_px > s.dirty_px_max) {
        s.dirty_px_max = s.pending_dirty_px;
    }
    s.render_us_total += render_us;
    if (render_us > s.render_us_max) {
        s.render_us_max = render_us;
    }

    s.pending_dirty_px = 0;
}

esp_err_t gauge_perf_attach(lv_display_t *disp)
{
    if (disp == NULL) {
        disp = lv_display_get_default();
    }
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_STATE, TAG, "no display");
    ESP_RETURN_ON_FALSE(!s.attached, ESP_ERR_INVALID_STATE, TAG, "already attached");

    lv_color_format_t cf = lv_display_get_color_format(disp);

    memset(&s, 0, sizeof(s));
    s.panel_px = (uint32_t)lv_display_get_horizontal_resolution(disp) *
                 (uint32_t)lv_display_get_vertical_resolution(disp);
    s.bytes_per_px = lv_color_format_get_size(cf);

    lv_display_add_event_cb(disp, on_flush_start, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, on_render_start, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, NULL);

    s.attached        = true;
    s.window_start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "attached: panel %" PRIu32 " px, %u bytes/px",
             s.panel_px, (unsigned)s.bytes_per_px);
    return ESP_OK;
}

void gauge_perf_reset(void)
{
    s.frames           = 0;
    s.skipped          = 0;
    s.dirty_px_total   = 0;
    s.dirty_px_max     = 0;
    s.render_us_total  = 0;
    s.render_us_max    = 0;
    s.pending_dirty_px = 0;
    s.window_start_us  = esp_timer_get_time();
}

void gauge_perf_get(gauge_perf_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s.window_start_us) / 1000);

    out->frames    = s.frames;
    out->skipped   = s.skipped;
    out->window_ms = elapsed_ms;

    if (elapsed_ms > 0) {
        out->fps = (float)s.frames * 1000.0f / (float)elapsed_ms;
    }

    if (s.frames > 0) {
        out->dirty_px_mean   = (uint32_t)(s.dirty_px_total / s.frames);
        out->dirty_px_max    = s.dirty_px_max;
        out->bytes_per_frame = out->dirty_px_mean * s.bytes_per_px;
        out->render_ms_mean  = (float)(s.render_us_total / s.frames) / 1000.0f;
        out->render_ms_max   = (float)s.render_us_max / 1000.0f;

        if (s.panel_px > 0) {
            out->dirty_pct_mean = (float)out->dirty_px_mean * 100.0f / (float)s.panel_px;
        }
    }
}

void gauge_perf_report(const char *tag, const char *label)
{
    gauge_perf_stats_t st;
    gauge_perf_get(&st);

    if (st.frames == 0) {
        ESP_LOGI(tag ? tag : TAG, "[%s] no frames drawn in %" PRIu32 " ms (%" PRIu32 " idle)",
                 label ? label : "", st.window_ms, st.skipped);
    } else {
        ESP_LOGI(tag ? tag : TAG,
                 "[%s] %.1f fps | dirty %.1f%% (%" PRIu32 " px, max %" PRIu32
                 ") | %" PRIu32 " B/frame | render %.2f ms (max %.2f)",
                 label ? label : "", (double)st.fps, (double)st.dirty_pct_mean,
                 st.dirty_px_mean, st.dirty_px_max, st.bytes_per_frame,
                 (double)st.render_ms_mean, (double)st.render_ms_max);
    }

    gauge_perf_reset();
}

static void report_timer_cb(lv_timer_t *t)
{
    (void)t;
    gauge_perf_report(s.report_tag, s.label);
}

esp_err_t gauge_perf_start_reporting(uint32_t period_ms, const char *label)
{
    ESP_RETURN_ON_FALSE(s.attached, ESP_ERR_INVALID_STATE, TAG, "not attached");

    snprintf(s.label, sizeof(s.label), "%s", label ? label : "");
    snprintf(s.report_tag, sizeof(s.report_tag), "%s", TAG);

    lv_timer_t *t = lv_timer_create(report_timer_cb, period_ms, NULL);
    ESP_RETURN_ON_FALSE(t != NULL, ESP_ERR_NO_MEM, TAG, "could not create report timer");

    gauge_perf_reset();
    return ESP_OK;
}
