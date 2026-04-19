#include <pebble.h>

// ── Layout constants ────────────────────────────────────────────────────────
// All positions are relative to the top-left of the screen.
// Adjust these values to match your top_cover.png pixel positions exactly.

#define COVER_H   32    // height of the HUD bar (top_cover.png)

// Hours text — drawn inside the B[  ] bracket area
#define HOURS_X   14
#define HOURS_Y    4
#define HOURS_W   46
#define HOURS_H   22

// Minutes text — drawn inside the A[  ] bracket area
#define MINS_X    76
#define MINS_Y     4
#define MINS_W    46
#define MINS_H    22

// Steps counter — drawn below the rupee icon
#define STEPS_X  133
#define STEPS_Y   17
#define STEPS_W   36
#define STEPS_H   14

// Heart-rate number — drawn next to the heart icon
#define HR_X     171
#define HR_Y      17
#define HR_W      27
#define HR_H      14

// ── Animation ───────────────────────────────────────────────────────────────
#define ANIM_FALLBACK_MS 100   // per-frame delay when APNG embeds no timing

// ── Persistent settings ─────────────────────────────────────────────────────
#define SETTINGS_KEY 1

typedef struct {
  bool use_24hr;
} Settings;

// ── Globals ──────────────────────────────────────────────────────────────────
static Window        *s_window;

static BitmapLayer   *s_bg_layer;
static BitmapLayer   *s_cover_layer;
static TextLayer     *s_hours_layer;
static TextLayer     *s_minutes_layer;
static TextLayer     *s_steps_layer;
static TextLayer     *s_hr_layer;

static GBitmap       *s_still_bitmap;
static GBitmap       *s_cover_bitmap;
static GBitmap       *s_anim_bitmap;
static GBitmapSequence *s_sequence;
static AppTimer      *s_anim_timer;

static uint32_t       s_anim_total_frames;
static uint32_t       s_anim_frame_idx;
static bool           s_animating;

static Settings       s_settings;

// ── Settings ─────────────────────────────────────────────────────────────────
static void load_settings(void) {
  s_settings.use_24hr = false;
  persist_read_data(SETTINGS_KEY, &s_settings, sizeof(Settings));
}

static void save_settings(void) {
  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(Settings));
}

// ── Health ───────────────────────────────────────────────────────────────────
static void update_health(void) {
  static char steps_buf[8];
  static char hr_buf[5];

  time_t start = time_start_of_today();
  time_t end   = time(NULL);

  if (health_service_metric_accessible(HealthMetricStepCount, start, end)
      & HealthServiceAccessibilityMaskPeekAvailable) {
    HealthValue steps = health_service_sum_today(HealthMetricStepCount);
    snprintf(steps_buf, sizeof(steps_buf), "%d", (int)steps);
  } else {
    snprintf(steps_buf, sizeof(steps_buf), "--");
  }
  text_layer_set_text(s_steps_layer, steps_buf);

  if (health_service_metric_accessible(HealthMetricHeartRateBPM, start, end)
      & HealthServiceAccessibilityMaskPeekAvailable) {
    HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
    snprintf(hr_buf, sizeof(hr_buf), "%d", (int)hr);
  } else {
    snprintf(hr_buf, sizeof(hr_buf), "--");
  }
  text_layer_set_text(s_hr_layer, hr_buf);
}

// ── Time ─────────────────────────────────────────────────────────────────────
static void update_time(struct tm *tick_time) {
  static char hours_buf[4];
  static char mins_buf[4];

  strftime(mins_buf, sizeof(mins_buf), "%M", tick_time);

  if (s_settings.use_24hr) {
    strftime(hours_buf, sizeof(hours_buf), "%H", tick_time);
  } else {
    strftime(hours_buf, sizeof(hours_buf), "%I", tick_time);
    // Remove the leading zero in 12-hour mode (e.g. "09" → "9")
    if (hours_buf[0] == '0') {
      memmove(hours_buf, hours_buf + 1, sizeof(hours_buf) - 1);
    }
  }

  text_layer_set_text(s_hours_layer, hours_buf);
  text_layer_set_text(s_minutes_layer, mins_buf);
}

// ── Animation ────────────────────────────────────────────────────────────────
static void anim_timer_callback(void *context);   // forward declaration

static void stop_animation(void) {
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
    s_anim_timer = NULL;
  }
  if (s_sequence) {
    gbitmap_sequence_destroy(s_sequence);
    s_sequence = NULL;
  }
  if (s_anim_bitmap) {
    gbitmap_destroy(s_anim_bitmap);
    s_anim_bitmap = NULL;
  }
  // Reload the still background now that animation memory is freed
  s_still_bitmap = gbitmap_create_with_resource(RESOURCE_ID_STILL_BG);
  bitmap_layer_set_bitmap(s_bg_layer, s_still_bitmap);
  s_animating = false;
}

static void advance_anim_frame(void) {
  if (!s_sequence || !s_anim_bitmap) {
    stop_animation();
    return;
  }

  uint32_t delay_ms = ANIM_FALLBACK_MS;
  bool has_next = gbitmap_sequence_update_bitmap_next_frame(
                    s_sequence, s_anim_bitmap, &delay_ms);
  layer_mark_dirty(bitmap_layer_get_layer(s_bg_layer));

  s_anim_frame_idx++;

  // Stop when sequence ends OR we have looped through all frames once
  if (!has_next || s_anim_frame_idx >= s_anim_total_frames) {
    stop_animation();
    return;
  }

  s_anim_timer = app_timer_register(
    delay_ms > 0 ? delay_ms : ANIM_FALLBACK_MS,
    anim_timer_callback, NULL);
}

static void anim_timer_callback(void *context) {
  s_anim_timer = NULL;
  advance_anim_frame();
}

static void start_animation(void) {
  if (s_animating) return;

  s_sequence = gbitmap_sequence_create_with_resource(RESOURCE_ID_ANIMATION);
  if (!s_sequence) return;   // placeholder PNG or corrupt file — skip gracefully

  s_anim_total_frames = gbitmap_sequence_get_total_num_frames(s_sequence);
  s_anim_frame_idx    = 0;

  // Free the still bitmap to reclaim RAM before allocating the animation frame
  if (s_still_bitmap) {
    gbitmap_destroy(s_still_bitmap);
    s_still_bitmap = NULL;
  }

  GSize size = gbitmap_sequence_get_bitmap_size(s_sequence);
  s_anim_bitmap = gbitmap_create_blank(size, GBitmapFormat8Bit);
  if (!s_anim_bitmap) {
    gbitmap_sequence_destroy(s_sequence);
    s_sequence = NULL;
    // Restore still image so the face isn't blank
    s_still_bitmap = gbitmap_create_with_resource(RESOURCE_ID_STILL_BG);
    bitmap_layer_set_bitmap(s_bg_layer, s_still_bitmap);
    return;
  }

  s_animating = true;
  bitmap_layer_set_bitmap(s_bg_layer, s_anim_bitmap);
  advance_anim_frame();
}

// ── Event handlers ───────────────────────────────────────────────────────────
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_time(tick_time);
  start_animation();
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  start_animation();
}

static void health_handler(HealthEventType event, void *context) {
  update_health();
}

static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t = dict_find(iter, MESSAGE_KEY_Use24Hour);
  if (t) {
    s_settings.use_24hr = (bool)t->value->int32;
    save_settings();
    time_t now = time(NULL);
    struct tm *tick_time = localtime(&now);
    update_time(tick_time);
  }
}

// ── Window lifecycle ─────────────────────────────────────────────────────────
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  // Background / animation layer (full screen)
  s_still_bitmap = gbitmap_create_with_resource(RESOURCE_ID_STILL_BG);
  s_bg_layer = bitmap_layer_create(GRect(0, 0, 200, 228));
  bitmap_layer_set_bitmap(s_bg_layer, s_still_bitmap);
  bitmap_layer_set_compositing_mode(s_bg_layer, GCompOpAssign);
  layer_add_child(root, bitmap_layer_get_layer(s_bg_layer));

  // HUD overlay — sits on top of the background across the full width
  // Change GCompOpAssign to GCompOpSet if your top_cover.png has transparency
  s_cover_bitmap = gbitmap_create_with_resource(RESOURCE_ID_TOP_COVER);
  s_cover_layer = bitmap_layer_create(GRect(0, 0, 200, COVER_H));
  bitmap_layer_set_bitmap(s_cover_layer, s_cover_bitmap);
  bitmap_layer_set_compositing_mode(s_cover_layer, GCompOpAssign);
  layer_add_child(root, bitmap_layer_get_layer(s_cover_layer));

  // Hours text — inside the B[  ] section of the HUD
  // To use a custom pixel font, replace fonts_get_system_font(...) with
  // fonts_load_custom_font(resource_get_handle(RESOURCE_ID_ZELDA_FONT))
  s_hours_layer = text_layer_create(GRect(HOURS_X, HOURS_Y, HOURS_W, HOURS_H));
  text_layer_set_background_color(s_hours_layer, GColorClear);
  text_layer_set_text_color(s_hours_layer, GColorBlack);
  text_layer_set_font(s_hours_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_hours_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_hours_layer));

  // Minutes text — inside the A[  ] section of the HUD
  s_minutes_layer = text_layer_create(GRect(MINS_X, MINS_Y, MINS_W, MINS_H));
  text_layer_set_background_color(s_minutes_layer, GColorClear);
  text_layer_set_text_color(s_minutes_layer, GColorBlack);
  text_layer_set_font(s_minutes_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_minutes_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_minutes_layer));

  // Steps counter — below the rupee icon
  s_steps_layer = text_layer_create(GRect(STEPS_X, STEPS_Y, STEPS_W, STEPS_H));
  text_layer_set_background_color(s_steps_layer, GColorClear);
  text_layer_set_text_color(s_steps_layer, GColorBlack);
  text_layer_set_font(s_steps_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_steps_layer, GTextAlignmentLeft);
  layer_add_child(root, text_layer_get_layer(s_steps_layer));

  // Heart-rate number — next to the heart icon
  s_hr_layer = text_layer_create(GRect(HR_X, HR_Y, HR_W, HR_H));
  text_layer_set_background_color(s_hr_layer, GColorClear);
  text_layer_set_text_color(s_hr_layer, GColorBlack);
  text_layer_set_font(s_hr_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_hr_layer, GTextAlignmentLeft);
  layer_add_child(root, text_layer_get_layer(s_hr_layer));

  // Initial display
  time_t now = time(NULL);
  struct tm *tick_time = localtime(&now);
  update_time(tick_time);
  update_health();
}

static void window_unload(Window *window) {
  stop_animation();

  text_layer_destroy(s_hr_layer);
  text_layer_destroy(s_steps_layer);
  text_layer_destroy(s_minutes_layer);
  text_layer_destroy(s_hours_layer);

  bitmap_layer_destroy(s_cover_layer);
  gbitmap_destroy(s_cover_bitmap);

  bitmap_layer_destroy(s_bg_layer);
  if (s_still_bitmap) {
    gbitmap_destroy(s_still_bitmap);
  }
}

// ── Init / deinit ─────────────────────────────────────────────────────────────
static void init(void) {
  load_settings();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(accel_tap_handler);
  health_service_events_subscribe(health_handler, NULL);

  app_message_open(128, 128);
  app_message_register_inbox_received(inbox_received_handler);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  accel_tap_service_unsubscribe();
  health_service_events_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
