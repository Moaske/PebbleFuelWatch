/* =============================================================
   gauge_window.c – manual fuel level gauge

   Laid out after the reference mockup: the arc opens to the left
   with the hub on the right, sweeping 160 degrees from Empty at
   the bottom, through the half mark on the left, to Full at the
   top. Eight steps between E and F, so the finest click is an
   eighth.

   UP / DOWN    move the needle
   SELECT       stores the position (persists across app exits)
   BACK         returns to the map / detail view

   Every dimension derives from the window bounds and the radius,
   so 144x168 and 200x228 share one layout. The composition is
   centred horizontally as a whole rather than on the hub, which
   keeps the proportions of the mockup at both sizes.

   System fonts throughout: this window sits on top of map_window,
   which already holds five custom fonts, and the 64KB platforms
   have little headroom at that depth.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "gauge_window.h"

/* ----------------------------------------------------------
   Sweep. Pebble angles put 0 at twelve o'clock and run clockwise,
   so these land E low-left and F high-left, mirroring the mockup.
---------------------------------------------------------- */
#define SWEEP_E_DEG   192
#define SWEEP_F_DEG   352

/* Fractions of the radius, taken from the mockup */
#define HUB_DIV         5    /* hub radius      = r / 5  */
#define ARC_DIV        12    /* arc thickness   = r / 12 */
#define TICK_DIV        6    /* half-mark tick  = r / 6  */
#define NEEDLE_NUM     78    /* needle length   = r * 78 / 100 */
#define SCALE_NUM      80    /* dial drawn at 80% of the max that fits */

/* The arc reaches 0.99r above the hub and 0.978r below it */
#define VEXT_NUM      197    /* total height    = r * 197 / 100 */
#define ABOVE_NUM      99
#define BELOW_NUM      98

#ifdef PBL_PLATFORM_EMERY
  #define V_MARGIN      4
  #define H_MARGIN      6
  #define LABEL_W      30
  #define LABEL_H      30
  #define FONT_EF      FONT_KEY_GOTHIC_28_BOLD
  #define FONT_HDR     FONT_KEY_GOTHIC_18_BOLD
#else
  #define V_MARGIN      4
  #define H_MARGIN      4
  #define LABEL_W      20
  #define LABEL_H      24
  #define FONT_EF      FONT_KEY_GOTHIC_24_BOLD
  #define FONT_HDR     FONT_KEY_GOTHIC_14_BOLD
#endif

static const char *LEVEL_LABEL[FUEL_LEVEL_MAX + 1] = {
  "Empty", "1/8", "1/4", "3/8", "1/2", "5/8", "3/4", "7/8", "Full"
};

/* ----------------------------------------------------------
   Module state
---------------------------------------------------------- */
static Window  *s_window = NULL;
static Layer   *s_canvas = NULL;
static GBitmap *s_icon   = NULL;
static uint8_t  s_level  = FUEL_LEVEL_MAX;
static uint8_t  s_saved  = FUEL_LEVEL_MAX;

/* ----------------------------------------------------------
   Helpers
---------------------------------------------------------- */

/* Interpolate in trig units so both ends land exactly on E and F */
static int32_t level_to_trig(uint8_t level) {
  int32_t a0 = DEG_TO_TRIGANGLE(SWEEP_E_DEG);
  int32_t a1 = DEG_TO_TRIGANGLE(SWEEP_F_DEG);
  return a0 + ((a1 - a0) * (int32_t)level) / FUEL_LEVEL_STEPS;
}

static GPoint polar(GPoint c, int16_t radius, int32_t trig_angle) {
  int16_t dx = (int16_t)((sin_lookup(trig_angle) * radius) / TRIG_MAX_RATIO);
  int16_t dy = (int16_t)((cos_lookup(trig_angle) * radius) / TRIG_MAX_RATIO);
  return GPoint(c.x + dx, c.y - dy);
}

static int16_t clamp16(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* ----------------------------------------------------------
   Canvas
---------------------------------------------------------- */
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect   bounds = layer_get_bounds(layer);
  int16_t w = bounds.size.w;
  int16_t h = bounds.size.h;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  /* --- Title bar carries the readout, keeping the dial clean --- */
  char title[24];
  snprintf(title, sizeof(title), "Fuel: %s", LEVEL_LABEL[s_level]);
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, GRect(0, 0, w, MAP_PAD_TOP), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title,
    fonts_get_system_font(FONT_HDR),
    GRect(MAP_PAD_SIDE, 1, w - MAP_PAD_SIDE * 2, MAP_PAD_TOP),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  /* --- Radius: as large as the shorter constraint allows ------
     height  : r * VEXT/100 must fit below the header
     width   : arc (r) + hub (r/5) + gap + label must fit across  */
  int16_t r_by_h = (int16_t)(((int32_t)(h - MAP_PAD_TOP - V_MARGIN * 2) * 100) / VEXT_NUM);
  int16_t r_by_w = (int16_t)(((int32_t)(w - H_MARGIN * 2 - LABEL_W - 6) * HUB_DIV)
                             / (HUB_DIV + 1));
  int16_t r = r_by_h < r_by_w ? r_by_h : r_by_w;
  r = (int16_t)(((int32_t)r * SCALE_NUM) / 100);   /* leave the dial some air */

  int16_t hub_r    = r / HUB_DIV;
  int16_t arc_w    = r / ARC_DIV;   if (arc_w < 2) arc_w = 2;
  int16_t tick_len = r / TICK_DIV;

  /* Centre the whole composition, not the hub */
  int16_t used_w = r + hub_r + 6 + LABEL_W;
  int16_t left   = (w - used_w) / 2;  if (left < H_MARGIN) left = H_MARGIN;

  /* Centre the dial in the space under the header rather than
     pinning it to the top, now that it no longer fills the screen */
  int16_t comp_h = (int16_t)(((int32_t)r * VEXT_NUM) / 100);
  int16_t top_sp = (h - MAP_PAD_TOP - comp_h) / 2;
  if (top_sp < V_MARGIN) top_sp = V_MARGIN;

  GPoint  c = GPoint(left + r,
                     MAP_PAD_TOP + top_sp + (int16_t)(((int32_t)r * ABOVE_NUM) / 100));

  GRect arc_box = GRect(c.x - r, c.y - r, r * 2, r * 2);

  /* --- Fuel pump decal, right of the hub, centred in the gap
         before the E/F labels. The needle only ever sweeps the left
         half, so nothing passes over it here. --- */
  if (s_icon) {
    GRect   ib   = gbitmap_get_bounds(s_icon);
    int16_t gap0 = c.x + hub_r;
    int16_t gap1 = w - H_MARGIN - LABEL_W;
    int16_t ix   = (gap0 + gap1) / 2 - ib.size.w / 2;
    if (ix < gap0 + 1) ix = gap0 + 1;
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_icon,
      GRect(ix, c.y - ib.size.h / 2, ib.size.w, ib.size.h));
  }

  /* --- Arc --- */
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, arc_w);
  graphics_draw_arc(ctx, arc_box, GOvalScaleModeFitCircle,
                    DEG_TO_TRIGANGLE(SWEEP_E_DEG),
                    DEG_TO_TRIGANGLE(SWEEP_F_DEG));

  /* Reserve tank: the last eighth before Empty, red where we have it */
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorRed);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  graphics_draw_arc(ctx, arc_box, GOvalScaleModeFitCircle,
                    level_to_trig(0), level_to_trig(1));

  /* --- Half-mark tick, pointing inward from the arc --- */
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, arc_w);
  int32_t half = level_to_trig(FUEL_LEVEL_STEPS / 2);
  graphics_draw_line(ctx,
    polar(c, r - arc_w / 2, half),
    polar(c, r - arc_w / 2 - tick_len, half));
  graphics_context_set_stroke_width(ctx, 1);

  /* --- E and F, right-aligned level with the arc ends --- */
  int16_t f_y = clamp16(polar(c, r, level_to_trig(FUEL_LEVEL_MAX)).y - LABEL_H / 2,
                        MAP_PAD_TOP + 2, h - LABEL_H - 2);
  int16_t e_y = clamp16(polar(c, r, level_to_trig(0)).y - LABEL_H / 2,
                        MAP_PAD_TOP + 2, h - LABEL_H - 2);
  GFont ef = fonts_get_system_font(FONT_EF);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "F", ef,
    GRect(w - H_MARGIN - LABEL_W, f_y, LABEL_W, LABEL_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "E", ef,
    GRect(w - H_MARGIN - LABEL_W, e_y, LABEL_W, LABEL_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  /* --- Stored position, so committed reads apart from current --- */
  GPoint sp = polar(c, r - arc_w / 2 - tick_len / 2, level_to_trig(s_saved));
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorDarkGray);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_circle(ctx, sp, arc_w / 2 + 1);

  /* --- Needle: tapered triangle from the hub --- */
  int32_t na     = level_to_trig(s_level);
  int16_t tip_r  = (int16_t)(((int32_t)r * NEEDLE_NUM) / 100);
  GPoint  pts[3] = {
    polar(c, tip_r, na),
    polar(c, hub_r, na + DEG_TO_TRIGANGLE(90)),
    polar(c, hub_r, na - DEG_TO_TRIGANGLE(90))
  };
  GPathInfo info   = { .num_points = 3, .points = pts };
  GPath    *needle = gpath_create(&info);
  if (needle) {
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorRed);
#else
    graphics_context_set_fill_color(ctx, GColorBlack);
#endif
    gpath_draw_filled(ctx, needle);
    gpath_destroy(needle);
  }

  /* Hub cap sits over the needle root */
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, c, hub_r);
}

/* ----------------------------------------------------------
   Buttons
---------------------------------------------------------- */
static void up_click(ClickRecognizerRef recognizer, void *ctx) {
  if (s_level < FUEL_LEVEL_MAX) { s_level++; layer_mark_dirty(s_canvas); }
}

static void down_click(ClickRecognizerRef recognizer, void *ctx) {
  if (s_level > 0) { s_level--; layer_mark_dirty(s_canvas); }
}

static void select_click(ClickRecognizerRef recognizer, void *ctx) {
  s_saved = s_level;
  persist_write_int(PERSIST_KEY_FUEL_LEVEL, (int32_t)s_saved);
  vibes_short_pulse();
  APP_LOG(APP_LOG_LEVEL_INFO, "Fuel level stored: %s", LEVEL_LABEL[s_saved]);
  layer_mark_dirty(s_canvas);
}

static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP,     up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

/* ----------------------------------------------------------
   Window lifecycle
---------------------------------------------------------- */
static void window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  if (persist_exists(PERSIST_KEY_FUEL_LEVEL)) {
    int32_t v = persist_read_int(PERSIST_KEY_FUEL_LEVEL);
    if (v < 0)               v = 0;
    if (v > FUEL_LEVEL_MAX)  v = FUEL_LEVEL_MAX;
    s_saved = (uint8_t)v;
  } else {
    s_saved = FUEL_LEVEL_MAX;
  }
  s_level = s_saved;

  s_icon = gbitmap_create_with_resource(RESOURCE_ID_FUELWATCH_APP_ICON);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  window_set_click_config_provider(window, click_config_provider);
}

static void window_unload(Window *window) {
  if (s_icon)   { gbitmap_destroy(s_icon); s_icon = NULL; }
  if (s_canvas) { layer_destroy(s_canvas); s_canvas = NULL; }
  window_destroy(s_window);
  s_window = NULL;
}

void gauge_window_push(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}