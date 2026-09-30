/*
 * Remote control of the UI: screenshots and synthetic touch gestures, for driving and inspecting
 * the gauge over HTTP (docs/networking.md, "Remote control"). See app_ui.h.
 *
 * Gestures go through a second LVGL pointer device, so they take exactly the path a finger does:
 * scrolling, clicks, long presses and the tileview all behave as they would on the glass. The
 * physical touch controller is untouched and keeps working alongside.
 *
 * Screenshots are rendered with lv_snapshot_take() on the LVGL task. They show what LVGL draws,
 * not what reached the panel: a fault in the flush path does not appear in them.
 */

#include "app_ui.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "app_ui_remote";

#define GESTURE_MAX_MS      10000
#define SCREENSHOT_WAIT_MS  3000

static struct {
    portMUX_TYPE lock;

    /* Guarded by lock: handed from the requesting task to the LVGL task. */
    app_ui_gesture_t queued;
    bool             pending;
    bool             active;
    bool             done;

    /* LVGL task only. */
    lv_indev_t      *indev;
    app_ui_gesture_t playing;
    int64_t          start_us;
    lv_point_t       last;
} r = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

/* ------------------------------------------------------------------ gestures ------- */

/*
 * LVGL rotates pointer input by the display rotation (indev_pointer_proc()), because the touch
 * controller reports panel coordinates. Gestures arrive in screen coordinates, the same as a
 * screenshot's, so undo that here.
 */
static lv_point_t to_raw(lv_display_t *disp, lv_point_t p)
{
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    switch (lv_display_get_rotation(disp)) {
    case LV_DISPLAY_ROTATION_90:  return (lv_point_t){p.y, h - 1 - p.x};
    case LV_DISPLAY_ROTATION_180: return (lv_point_t){w - 1 - p.x, h - 1 - p.y};
    case LV_DISPLAY_ROTATION_270: return (lv_point_t){w - 1 - p.y, p.x};
    default:                      return p;
    }
}

static void remote_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    taskENTER_CRITICAL(&r.lock);
    if (r.pending) {
        r.playing = r.queued;
        r.pending = false;
        r.active  = true;
        r.start_us = 0;
    }
    bool active = r.active;
    taskEXIT_CRITICAL(&r.lock);

    lv_display_t *disp = lv_indev_get_display(indev);

    if (!active) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = r.last;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (r.start_us == 0) {
        r.start_us = now; /* the first read presses at the start point */
    }
    int64_t elapsed = (now - r.start_us) / 1000;

    const app_ui_gesture_t *g = &r.playing;
    lv_point_t              p;
    if (elapsed >= g->duration_ms) {
        p           = (lv_point_t){g->x2, g->y2};
        data->state = LV_INDEV_STATE_RELEASED;
        taskENTER_CRITICAL(&r.lock);
        r.active = false;
        r.done   = true;
        taskEXIT_CRITICAL(&r.lock);
    } else {
        int32_t t   = (int32_t)(elapsed * 1024 / g->duration_ms);
        p.x         = g->x1 + (g->x2 - g->x1) * t / 1024;
        p.y         = g->y1 + (g->y2 - g->y1) * t / 1024;
        data->state = LV_INDEV_STATE_PRESSED;
    }

    r.last      = to_raw(disp, p);
    data->point = r.last;
}

esp_err_t app_ui_remote_start(void)
{
    if (r.indev != NULL) {
        return ESP_OK;
    }
    r.indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(r.indev != NULL, ESP_ERR_NO_MEM, TAG, "indev");
    lv_indev_set_type(r.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(r.indev, remote_read_cb);
    ESP_LOGI(TAG, "remote input ready");
    return ESP_OK;
}

esp_err_t app_ui_remote_gesture(const app_ui_gesture_t *g)
{
    ESP_RETURN_ON_FALSE(g != NULL && g->duration_ms > 0 && g->duration_ms <= GESTURE_MAX_MS,
                        ESP_ERR_INVALID_ARG, TAG, "bad gesture");
    ESP_RETURN_ON_FALSE(r.indev != NULL, ESP_ERR_INVALID_STATE, TAG, "remote input not started");

    taskENTER_CRITICAL(&r.lock);
    bool busy = r.pending || r.active;
    if (!busy) {
        r.queued  = *g;
        r.pending = true;
        r.done    = false;
    }
    taskEXIT_CRITICAL(&r.lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The read timer runs every refresh period, so allow for a slow frame or two on top. */
    int64_t deadline = esp_timer_get_time() + ((int64_t)g->duration_ms + 1000) * 1000;
    for (;;) {
        taskENTER_CRITICAL(&r.lock);
        bool done = r.done;
        taskEXIT_CRITICAL(&r.lock);
        if (done) {
            return ESP_OK;
        }
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------------------------------------------------------- screenshots ------ */

typedef struct {
    SemaphoreHandle_t done;
    lv_draw_buf_t    *buf;
} snapshot_job_t;

static void snapshot_async_cb(void *arg)
{
    snapshot_job_t *job = arg;
    /* RGB565 is the display's own format, so this is the cheapest render and a direct BMP. */
    job->buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    xSemaphoreGive(job->done);
}

esp_err_t app_ui_screenshot(app_ui_image_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "no output");

    /*
     * Rendered on the LVGL task rather than here: drawing the object tree recurses deeply, and the
     * HTTP task's internal-RAM stack is sized for its own work, not LVGL's.
     */
    snapshot_job_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(job != NULL, ESP_ERR_NO_MEM, TAG, "job");
    job->done = xSemaphoreCreateBinary();
    if (job->done == NULL) {
        free(job);
        return ESP_ERR_NO_MEM;
    }

    if (bsp_display_lock(-1) != ESP_OK) {
        vSemaphoreDelete(job->done);
        free(job);
        return ESP_ERR_INVALID_STATE;
    }
    lv_result_t queued = lv_async_call(snapshot_async_cb, job);
    bsp_display_unlock();

    if (queued != LV_RESULT_OK) {
        vSemaphoreDelete(job->done);
        free(job);
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(SCREENSHOT_WAIT_MS)) != pdTRUE) {
        /* The callback still holds the job; leak it rather than free it under the LVGL task. */
        ESP_LOGE(TAG, "screenshot timed out");
        return ESP_ERR_TIMEOUT;
    }

    lv_draw_buf_t *buf = job->buf;
    vSemaphoreDelete(job->done);
    free(job);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "snapshot failed (out of PSRAM?)");

    *out = (app_ui_image_t){
        .width  = (uint16_t)buf->header.w,
        .height = (uint16_t)buf->header.h,
        .stride = buf->header.stride,
        .rgb565 = buf->data,
        .handle = buf,
    };
    return ESP_OK;
}

void app_ui_screenshot_free(app_ui_image_t *img)
{
    if (img == NULL || img->handle == NULL) {
        return;
    }
    if (bsp_display_lock(-1) == ESP_OK) {
        lv_draw_buf_destroy((lv_draw_buf_t *)img->handle);
        bsp_display_unlock();
    }
    img->handle = NULL;
    img->rgb565 = NULL;
}
