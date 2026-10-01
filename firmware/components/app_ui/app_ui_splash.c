/*
 * Boot splash: the obd-display Pi splash, recut for the round panel (docs/boot-splash.md).
 *
 * The scene is one RGB565 image in flash, logo and tag line baked in, so it is blitted once and
 * never decoded. The only animation is an underline revealed left to right under the tag line, led
 * by a spark: the bar image sits in a container whose width grows, so each frame dirties only the
 * strip the bar and spark move through.
 *
 * The hand-over to the dial is a dip through black on the panel's own brightness, not an opacity
 * fade. Blending the full-screen scene over the dial measured ~75 ms a frame (~13 fps, visibly
 * stepped); the brightness command costs no rendering. At black the splash is deleted, the dial
 * draws unseen, and brightness comes back up to the user's setting.
 *
 * The hand-over waits for app_ui_splash_release() as well as the animation: app_main releases the
 * splash once the sensors are reading and WiFi has initialised, so the gauge opens on live values
 * and the hand-over's full-screen redraw never overlaps WiFi claiming its buffers.
 *
 * Everything lives on lv_layer_top(), above whatever app_ui_create() builds. A tap skips the
 * animation, but not the wait for release.
 */
#include "app_ui.h"

#include <stdatomic.h>

#include "esp_check.h"
#include "esp_log.h"

#include "bsp/display.h"

#include "app_settings.h"
#include "gauge_perf.h"
#include "splash/splash_assets.h"

static const char *TAG = "app_ui";

/* Timing, from the Pi splash but quicker: here the driver is waiting on the gauge, not a boot. */
#define SPLASH_DELAY_MS 300 /* let the scene register before anything moves */
#define SPLASH_SWEEP_MS 1400
#define SPARK_FADE_MS   300
#define SPLASH_HOLD_MS  500 /* complete underline, before handing over */
#define RELEASE_MAX_MS  5000 /* hand over regardless, if app_main never releases the splash */
#define WAIT_POLL_MS    20
#define DIM_MS          250 /* brightness down to black */
#define DARK_MS         60  /* at black: long enough for the dial's first full frame */
#define UNDIM_MS        300 /* brightness back up, onto the dial */

/* EMBED_FILES in CMakeLists.txt. */
extern const uint8_t scene_bin_start[] asm("_binary_scene_bin_start");
extern const uint8_t bar_bin_start[] asm("_binary_bar_bin_start");
extern const uint8_t spark_bin_start[] asm("_binary_spark_bin_start");

#define IMAGE_DSC(name, cf_, w_, h_, bpp)                                                  \
    static const lv_image_dsc_t name = {                                                   \
        .header    = {.magic  = LV_IMAGE_HEADER_MAGIC,                                     \
                      .cf     = (cf_),                                                     \
                      .w      = (w_),                                                      \
                      .h      = (h_),                                                      \
                      .stride = (w_) * (bpp)},                                             \
        .data_size = (w_) * (h_) * (bpp),                                                  \
        .data      = name##_bin_start,                                                     \
    }

IMAGE_DSC(scene, LV_COLOR_FORMAT_RGB565, SPLASH_SCENE_SIZE, SPLASH_SCENE_SIZE, 2);
IMAGE_DSC(bar, LV_COLOR_FORMAT_ARGB8888, SPLASH_BAR_W, SPLASH_BAR_H, 4);
IMAGE_DSC(spark, LV_COLOR_FORMAT_ARGB8888, SPLASH_SPARK_SIZE, SPLASH_SPARK_SIZE, 4);

static struct {
    lv_obj_t   *root;
    lv_obj_t   *bar_clip;
    lv_obj_t   *spark;
    lv_timer_t *waiting;   /* underline complete, waiting for release */
    uint32_t    line_done; /* lv_tick_get() when the underline completed */
    bool        leaving;
    atomic_bool released;
} s;

static void set_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void sweep_exec(void *obj, int32_t w)
{
    lv_obj_set_width(obj, w);
    lv_obj_set_x(s.spark, SPLASH_BAR_X + w - SPLASH_SPARK_SIZE / 2);
}

static void sweep_start(lv_anim_t *a)
{
    (void)a;
    set_opa(s.spark, LV_OPA_COVER);
}

/* v is 0..1000 of the user's brightness; squared, so the dip looks even to the eye. */
static void brightness_exec(void *var, int32_t v)
{
    (void)var;
    bsp_display_brightness_set((int)(app_settings_get()->brightness * v * v / 1000000));
}

static void brightness_anim(int32_t from, int32_t to, uint32_t delay_ms, uint32_t ms, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s); /* not an object: these outlive the splash */
    lv_anim_set_exec_cb(&a, brightness_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void undimmed(lv_anim_t *a)
{
    (void)a;
    gauge_perf_report(TAG, "boot splash");
}

static void dimmed(lv_anim_t *a)
{
    (void)a;
    lv_obj_delete(s.root);
    s.root = s.bar_clip = s.spark = NULL;
    brightness_anim(0, 1000, DARK_MS, UNDIM_MS, undimmed);
}

static void wait_cb(lv_timer_t *t)
{
    uint32_t held = lv_tick_elaps(s.line_done);
    if ((atomic_load(&s.released) && held >= SPLASH_HOLD_MS) || held >= RELEASE_MAX_MS) {
        if (!atomic_load(&s.released)) {
            ESP_LOGW(TAG, "boot splash never released; handing over anyway");
        }
        lv_timer_delete(t);
        s.waiting = NULL;
        s.leaving = true;
        brightness_anim(1000, 0, 0, DIM_MS, dimmed);
    }
}

/* The underline is complete: hold it until released, for at least SPLASH_HOLD_MS. */
static void line_complete(void)
{
    s.line_done = lv_tick_get();
    if (s.waiting == NULL) {
        s.waiting = lv_timer_create(wait_cb, WAIT_POLL_MS, NULL);
    }
}

static void spark_faded(lv_anim_t *a)
{
    (void)a;
    line_complete();
}

static void swept(lv_anim_t *a)
{
    (void)a;
    lv_anim_t f;
    lv_anim_init(&f);
    lv_anim_set_var(&f, s.spark);
    lv_anim_set_exec_cb(&f, set_opa);
    lv_anim_set_values(&f, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&f, SPARK_FADE_MS);
    lv_anim_set_completed_cb(&f, spark_faded);
    lv_anim_start(&f);
}

/* Skip to the complete underline with no hold; the hand-over still waits for release. */
static void pressed_cb(lv_event_t *e)
{
    (void)e;
    if (s.leaving) {
        return;
    }
    lv_anim_delete(s.bar_clip, NULL);
    lv_anim_delete(s.spark, NULL);
    lv_obj_set_width(s.bar_clip, SPLASH_BAR_W);
    set_opa(s.spark, LV_OPA_TRANSP);
    line_complete();
    s.line_done -= SPLASH_HOLD_MS;
}

/* A plain container: no theme padding, border or scrolling, never takes input. */
static lv_obj_t *bare_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    if (o != NULL) {
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    return o;
}

esp_err_t app_ui_splash_show(void)
{
    esp_err_t ret = ESP_OK;

    /*
     * EMBED_FILES does not align its data; these land word-aligned only by how the linker packs
     * them. LVGL reads ARGB8888 a word at a time, which faults on Xtensa if they ever do not.
     */
    ESP_GOTO_ON_FALSE(((uintptr_t)bar_bin_start | (uintptr_t)spark_bin_start | (uintptr_t)scene_bin_start) % 4 == 0,
                      ESP_ERR_INVALID_STATE, fail, TAG, "splash images not word-aligned");

    s.root = bare_obj(lv_layer_top());
    ESP_GOTO_ON_FALSE(s.root != NULL, ESP_ERR_NO_MEM, fail, TAG, "splash root failed");
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, LV_PART_MAIN);
    /* Takes the touch, so nothing reaches the dial underneath while the splash is up. */
    lv_obj_add_flag(s.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s.root, pressed_cb, LV_EVENT_PRESSED, NULL);

    /* The bar and spark are children of the scene, so their positions are in scene pixels. */
    lv_obj_t *img = lv_image_create(s.root);
    ESP_GOTO_ON_FALSE(img != NULL, ESP_ERR_NO_MEM, fail, TAG, "splash image failed");
    lv_image_set_src(img, &scene);
    lv_obj_center(img);

    s.bar_clip = bare_obj(img);
    ESP_GOTO_ON_FALSE(s.bar_clip != NULL, ESP_ERR_NO_MEM, fail, TAG, "splash bar failed");
    lv_obj_set_pos(s.bar_clip, SPLASH_BAR_X, SPLASH_BAR_Y);
    lv_obj_set_size(s.bar_clip, 0, SPLASH_BAR_H);
    lv_obj_t *bar_img = lv_image_create(s.bar_clip);
    ESP_GOTO_ON_FALSE(bar_img != NULL, ESP_ERR_NO_MEM, fail, TAG, "splash bar failed");
    lv_image_set_src(bar_img, &bar);

    s.spark = lv_image_create(img);
    ESP_GOTO_ON_FALSE(s.spark != NULL, ESP_ERR_NO_MEM, fail, TAG, "splash spark failed");
    lv_image_set_src(s.spark, &spark);
    lv_obj_set_pos(s.spark, SPLASH_BAR_X - SPLASH_SPARK_SIZE / 2,
                   SPLASH_BAR_Y + SPLASH_BAR_H / 2 - SPLASH_SPARK_SIZE / 2);
    set_opa(s.spark, LV_OPA_TRANSP);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s.bar_clip);
    lv_anim_set_exec_cb(&a, sweep_exec);
    lv_anim_set_values(&a, 0, SPLASH_BAR_W);
    lv_anim_set_delay(&a, SPLASH_DELAY_MS);
    lv_anim_set_duration(&a, SPLASH_SWEEP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_start_cb(&a, sweep_start);
    lv_anim_set_completed_cb(&a, swept);
    lv_anim_start(&a);

    return ESP_OK;

fail:
    if (s.root != NULL) {
        lv_obj_delete(s.root);
        s.root = NULL;
    }
    return ret;
}

void app_ui_splash_release(void)
{
    atomic_store(&s.released, true);
}
