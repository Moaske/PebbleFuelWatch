/* =============================================================
   map_window.c – FuelWatch map view
   Shows road network + station dots, all pre-projected by phone.
   Roads arrive after window is pushed — shows loading state first.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "map_window.h"
/* Defined in main.c */
void send_map_request(void);

/* ----------------------------------------------------------
   Drawing constants
---------------------------------------------------------- */
#define DOT_SELECTED    6
#define DOT_ADJACENT    4
#define DOT_OWN         3
#define CROSSHAIR_ARM   6
#define LABEL_OFFSET_Y  3
#define LABEL_W        72
#define LABEL_H        14
#define MAP_PAD_TOP    16
#define MAP_PAD_BOT     2
#define MAP_PAD_SIDE    6
#define BBOX_PAD     0.20f

/* ----------------------------------------------------------
   Module state
---------------------------------------------------------- */
static Window    *s_window;
static Layer     *s_canvas;
static TextLayer *s_loading_layer;
static bool       s_roads_ready = false;

/* ----------------------------------------------------------
   Projection helpers (used for station dots only —
   roads arrive pre-projected as pixel coords)
---------------------------------------------------------- */
typedef struct {
  float min_lat, max_lat;
  float min_lon, max_lon;
  float map_x0, map_y0;
  float map_w,  map_h;
} Projection;

static Projection build_projection(int32_t (*pts)[2], int count,
                                   GRect canvas_bounds) {
  Projection p;
  float min_lat =  999.0f, max_lat = -999.0f;
  float min_lon =  999.0f, max_lon = -999.0f;

  for (int i = 0; i < count; i++) {
    float lat = pts[i][0] / 1000000.0f;
    float lon = pts[i][1] / 1000000.0f;
    if (lat < min_lat) min_lat = lat;
    if (lat > max_lat) max_lat = lat;
    if (lon < min_lon) min_lon = lon;
    if (lon > max_lon) max_lon = lon;
  }

  float lat_span = max_lat - min_lat;
  float lon_span = max_lon - min_lon;
  if (lat_span < 0.005f) {
    float mid = (min_lat + max_lat) / 2.0f;
    min_lat = mid - 0.0025f; max_lat = mid + 0.0025f; lat_span = 0.005f;
  }
  if (lon_span < 0.005f) {
    float mid = (min_lon + max_lon) / 2.0f;
    min_lon = mid - 0.0025f; max_lon = mid + 0.0025f; lon_span = 0.005f;
  }

  float lat_pad = lat_span * BBOX_PAD;
  float lon_pad = lon_span * BBOX_PAD;
  p.min_lat = min_lat - lat_pad; p.max_lat = max_lat + lat_pad;
  p.min_lon = min_lon - lon_pad; p.max_lon = max_lon + lon_pad;

  p.map_x0 = canvas_bounds.origin.x + MAP_PAD_SIDE;
  p.map_y0 = canvas_bounds.origin.y + MAP_PAD_TOP;
  p.map_w  = canvas_bounds.size.w - MAP_PAD_SIDE * 2;
  p.map_h  = canvas_bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT;

  return p;
}

static GPoint project(Projection *p, int32_t lat_e6, int32_t lon_e6) {
  float lat = lat_e6 / 1000000.0f;
  float lon = lon_e6 / 1000000.0f;
  float x = p->map_x0 + (lon - p->min_lon) / (p->max_lon - p->min_lon) * p->map_w;
  float y = p->map_y0 + (1.0f - (lat - p->min_lat) / (p->max_lat - p->min_lat)) * p->map_h;
  return GPoint((int16_t)x, (int16_t)y);
}

/* ----------------------------------------------------------
   Draw helpers
---------------------------------------------------------- */
static void draw_filled_circle(GContext *ctx, GPoint centre, uint8_t r) {
  graphics_fill_circle(ctx, centre, r);
}

static void draw_crosshair(GContext *ctx, GPoint c, uint8_t arm) {
  graphics_draw_line(ctx, GPoint(c.x - arm, c.y), GPoint(c.x + arm, c.y));
  graphics_draw_line(ctx, GPoint(c.x, c.y - arm), GPoint(c.x, c.y + arm));
}

static void draw_station_dot(GContext *ctx, GPoint pt,
                              const char *label, uint8_t radius,
                              bool selected) {
#ifdef PBL_COLOR
  GColor dot_color = selected ? GColorCobaltBlue : GColorDarkGray;
  graphics_context_set_fill_color(ctx, dot_color);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  draw_filled_circle(ctx, pt, radius);

  if (selected) {
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorCobaltBlue);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_draw_circle(ctx, pt, radius + 2);
  }

  GRect label_rect = GRect(
    pt.x - LABEL_W / 2,
    pt.y - radius - LABEL_OFFSET_Y - LABEL_H,
    LABEL_W, LABEL_H);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, label,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    label_rect,
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

/* ----------------------------------------------------------
   Canvas draw callback
---------------------------------------------------------- */
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  AppState *state  = app_state_get();
  GRect     bounds = layer_get_bounds(layer);

  /* Background */
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  /* Title bar */
  uint8_t   sel      = state->selected_index;
  Station  *selected = &state->stations[sel];
  char      title[32];
  snprintf(title, sizeof(title), "%s", selected->name);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, MAP_PAD_TOP), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title,
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(4, 1, bounds.size.w - 8, MAP_PAD_TOP - 2),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  /* --- Road network (drawn first, behind dots) ---------- */
  if (s_roads_ready && state->road_count > 0) {
    for (int i = 0; i < state->road_count; i++) {
      RoadSegment *seg = &state->roads[i];

      switch (seg->road_type) {
        case ROAD_MAJOR:
#ifdef PBL_COLOR
          graphics_context_set_stroke_color(ctx, GColorDarkGray);
#else
          graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
          /* Draw twice offset by 1px for thicker appearance */
          graphics_draw_line(ctx, GPoint(seg->x1, seg->y1),
                                  GPoint(seg->x2, seg->y2));
          graphics_draw_line(ctx, GPoint(seg->x1 + 1, seg->y1),
                                  GPoint(seg->x2 + 1, seg->y2));
          break;

        case ROAD_MEDIUM:
#ifdef PBL_COLOR
          graphics_context_set_stroke_color(ctx, GColorDarkGray);
#else
          graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
          graphics_draw_line(ctx, GPoint(seg->x1, seg->y1),
                                  GPoint(seg->x2, seg->y2));
          break;

        case ROAD_MINOR:
#ifdef PBL_COLOR
          graphics_context_set_stroke_color(ctx, GColorLightGray);
#else
          /* On B&W draw minor roads as dashed — every other pixel */
          if (((seg->x1 + seg->y1) / 3) % 2 == 0) {
            graphics_context_set_stroke_color(ctx, GColorBlack);
            graphics_draw_line(ctx, GPoint(seg->x1, seg->y1),
                                    GPoint(seg->x2, seg->y2));
          }
          break;
#endif
#ifdef PBL_COLOR
          graphics_draw_line(ctx, GPoint(seg->x1, seg->y1),
                                  GPoint(seg->x2, seg->y2));
          break;
#endif
      }
    }
  }

  /* --- Station dots ------------------------------------- */
  int idx_a = (sel > 0)                  ? sel - 1 : -1;
  int idx_b = sel;
  int idx_c = (sel < state->count - 1)   ? sel + 1 : -1;

  int32_t pts[4][2];
  pts[0][0] = state->own_lat_e6;       pts[0][1] = state->own_lon_e6;
  pts[1][0] = state->stations[idx_b].lat_e6; pts[1][1] = state->stations[idx_b].lon_e6;
  pts[2][0] = (idx_a >= 0) ? state->stations[idx_a].lat_e6 : pts[1][0];
  pts[2][1] = (idx_a >= 0) ? state->stations[idx_a].lon_e6 : pts[1][1];
  pts[3][0] = (idx_c >= 0) ? state->stations[idx_c].lat_e6 : pts[1][0];
  pts[3][1] = (idx_c >= 0) ? state->stations[idx_c].lon_e6 : pts[1][1];

  Projection proj = build_projection(pts, 4, bounds);

  if (idx_a >= 0) {
    GPoint pa = project(&proj, state->stations[idx_a].lat_e6,
                               state->stations[idx_a].lon_e6);
    draw_station_dot(ctx, pa, state->stations[idx_a].name, DOT_ADJACENT, false);
  }
  if (idx_c >= 0) {
    GPoint pc = project(&proj, state->stations[idx_c].lat_e6,
                               state->stations[idx_c].lon_e6);
    draw_station_dot(ctx, pc, state->stations[idx_c].name, DOT_ADJACENT, false);
  }

  GPoint pb = project(&proj, state->stations[idx_b].lat_e6,
                             state->stations[idx_b].lon_e6);
  draw_station_dot(ctx, pb, selected->name, DOT_SELECTED, true);

  /* Own position */
  GPoint own = project(&proj, state->own_lat_e6, state->own_lon_e6);
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_context_set_fill_color(ctx,  GColorRed);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,  GColorBlack);
#endif
  draw_filled_circle(ctx, own, DOT_OWN);
  draw_crosshair(ctx, own, CROSSHAIR_ARM);
}

/* ----------------------------------------------------------
   Window callbacks
---------------------------------------------------------- */
static void window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  /* Loading overlay — shown while roads are being fetched */
  s_loading_layer = text_layer_create(
    GRect(0, bounds.size.h / 2 - 10, bounds.size.w, 20));
  text_layer_set_text(s_loading_layer, "Loading map...");
  text_layer_set_text_alignment(s_loading_layer, GTextAlignmentCenter);
  text_layer_set_font(s_loading_layer,
    fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_background_color(s_loading_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_loading_layer));
}

static void window_appear(Window *window) {
  /* Request road data from phone every time map appears */
  s_roads_ready = false;
  layer_set_hidden(text_layer_get_layer(s_loading_layer), false);
  send_map_request();
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  text_layer_destroy(s_loading_layer);
  s_canvas        = NULL;
  s_loading_layer = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

/* ----------------------------------------------------------
   Public API
---------------------------------------------------------- */
void map_window_push(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .appear = window_appear,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

void map_window_roads_arrived(void) {
  s_roads_ready = true;
  if (s_window && window_stack_contains_window(s_window)) {
    layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
    layer_mark_dirty(s_canvas);
  }
}