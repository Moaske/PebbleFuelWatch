/* =============================================================
   map_window.c – FuelWatch map view
   Draws a minimal dot-map: current position + 3 nearby stations.
   No tile data — just projected dots, labels, and a crosshair.
   Responsive: uses actual screen bounds throughout.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "map_window.h"

/* ----------------------------------------------------------
   Drawing constants
---------------------------------------------------------- */
#define DOT_SELECTED    6   // px radius for selected station
#define DOT_ADJACENT    4   // px radius for neighbouring stations
#define DOT_OWN         3   // px radius for current position
#define CROSSHAIR_ARM   6   // px length of each crosshair arm
#define LABEL_OFFSET_Y  3   // px gap between dot top and label
#define LABEL_W        72   // max label width px
#define LABEL_H        14   // label height px
#define GRID_LINES      4   // number of grid lines each axis
#define MAP_PAD_TOP    16   // px — reserved for title bar
#define MAP_PAD_BOT     2   // px — bottom margin
#define MAP_PAD_SIDE    6   // px — left/right margin
#define BBOX_PAD     0.20f  // fractional padding around bounding box

/* ----------------------------------------------------------
   Module state
---------------------------------------------------------- */
static Window  *s_window;
static Layer   *s_canvas;

/* ----------------------------------------------------------
   Projection helpers
   All coords stored as int32 ×1e6 (lat_e6 / lon_e6).
   We work in float only during the projection calculation.
---------------------------------------------------------- */
typedef struct {
  float min_lat, max_lat;
  float min_lon, max_lon;
  float map_x0, map_y0;   // pixel origin of map area
  float map_w,  map_h;    // pixel size of map area
} Projection;

/* Build a Projection from a set of lat/lon points (in e6 units).
   Points array: [[lat_e6, lon_e6], ...], count = number of points. */
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

  /* Ensure a minimum span so a single point doesn't collapse the bbox */
  float lat_span = max_lat - min_lat;
  float lon_span = max_lon - min_lon;
  if (lat_span < 0.005f) {
    float mid = (min_lat + max_lat) / 2.0f;
    min_lat = mid - 0.0025f;
    max_lat = mid + 0.0025f;
    lat_span = 0.005f;
  }
  if (lon_span < 0.005f) {
    float mid = (min_lon + max_lon) / 2.0f;
    min_lon = mid - 0.0025f;
    max_lon = mid + 0.0025f;
    lon_span = 0.005f;
  }

  /* Add padding around the bounding box */
  float lat_pad = lat_span * BBOX_PAD;
  float lon_pad = lon_span * BBOX_PAD;
  p.min_lat = min_lat - lat_pad;
  p.max_lat = max_lat + lat_pad;
  p.min_lon = min_lon - lon_pad;
  p.max_lon = max_lon + lon_pad;

  /* Map area within canvas (leave room for title bar + margins) */
  p.map_x0 = canvas_bounds.origin.x + MAP_PAD_SIDE;
  p.map_y0 = canvas_bounds.origin.y + MAP_PAD_TOP;
  p.map_w  = canvas_bounds.size.w - MAP_PAD_SIDE * 2;
  p.map_h  = canvas_bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT;

  return p;
}

/* Project a single lat/lon (in e6 units) to a pixel GPoint */
static GPoint project(Projection *p, int32_t lat_e6, int32_t lon_e6) {
  float lat = lat_e6 / 1000000.0f;
  float lon = lon_e6 / 1000000.0f;

  /* Longitude → X: left to right */
  float x = p->map_x0 +
             (lon - p->min_lon) / (p->max_lon - p->min_lon) * p->map_w;

  /* Latitude → Y: north is up, so invert */
  float y = p->map_y0 +
             (1.0f - (lat - p->min_lat) / (p->max_lat - p->min_lat)) * p->map_h;

  return GPoint((int16_t)x, (int16_t)y);
}

/* ----------------------------------------------------------
   Draw helpers
---------------------------------------------------------- */

/* Draw a filled circle (GContext has no fill_circle, we use
   graphics_fill_radial on SDK ≥3, or a manual approach on older) */
static void draw_filled_circle(GContext *ctx, GPoint centre, uint8_t r) {
#ifdef PBL_SDK_3
  graphics_fill_circle(ctx, centre, r);
#else
  /* Fallback: draw filled rect approximation for very small radii */
  graphics_fill_rect(ctx,
    GRect(centre.x - r, centre.y - r, r * 2, r * 2),
    r, GCornersAll);
#endif
}

/* Draw a crosshair at a point (own position marker) */
static void draw_crosshair(GContext *ctx, GPoint c, uint8_t arm) {
  graphics_draw_line(ctx,
    GPoint(c.x - arm, c.y), GPoint(c.x + arm, c.y));
  graphics_draw_line(ctx,
    GPoint(c.x, c.y - arm), GPoint(c.x, c.y + arm));
}

/* Draw a station dot + label above it.
   Label is clipped to LABEL_W. Highlight ring drawn for selected. */
static void draw_station_dot(GContext *ctx, GPoint pt,
                              const char *label,
                              uint8_t radius,
                              bool selected) {
#ifdef PBL_COLOR
  GColor dot_color = selected ? GColorCobaltBlue : GColorDarkGray;
  graphics_context_set_fill_color(ctx, dot_color);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif

  draw_filled_circle(ctx, pt, radius);

  /* Extra ring for selected station */
  if (selected) {
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorCobaltBlue);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_draw_circle(ctx, pt, radius + 2);
  }

  /* Label — positioned above the dot */
  GRect label_rect = GRect(
    pt.x - LABEL_W / 2,
    pt.y - radius - LABEL_OFFSET_Y - LABEL_H,
    LABEL_W,
    LABEL_H
  );

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx,
    label,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    label_rect,
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentCenter,
    NULL);
}

/* ----------------------------------------------------------
   Canvas draw callback
---------------------------------------------------------- */
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  AppState *state  = app_state_get();
  GRect     bounds = layer_get_bounds(layer);

  /* --- Background --------------------------------------- */
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  /* --- Title bar ---------------------------------------- */
  uint8_t sel = state->selected_index;
  Station *selected = &state->stations[sel];

  char title[32];
  snprintf(title, sizeof(title), "%s", selected->name);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, MAP_PAD_TOP),
                     0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, MAP_PAD_TOP),
                     0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
#endif

  graphics_draw_text(ctx,
    title,
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(4, 1, bounds.size.w - 8, MAP_PAD_TOP - 2),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentLeft,
    NULL);

  /* --- Determine which stations to plot ----------------- */
  /* Selected + one above + one below in the list (clamped) */
  int idx_a = (sel > 0)                    ? sel - 1 : -1;
  int idx_b = sel;
  int idx_c = (sel < state->count - 1)     ? sel + 1 : -1;

  /* Build point array for projection:
     [0] = own position
     [1] = selected station
     [2] = adjacent above (if exists, else repeat selected)
     [3] = adjacent below (if exists, else repeat selected)   */
  int32_t pts[4][2];
  pts[0][0] = state->own_lat_e6;
  pts[0][1] = state->own_lon_e6;
  pts[1][0] = state->stations[idx_b].lat_e6;
  pts[1][1] = state->stations[idx_b].lon_e6;
  pts[2][0] = (idx_a >= 0) ? state->stations[idx_a].lat_e6 : pts[1][0];
  pts[2][1] = (idx_a >= 0) ? state->stations[idx_a].lon_e6 : pts[1][1];
  pts[3][0] = (idx_c >= 0) ? state->stations[idx_c].lat_e6 : pts[1][0];
  pts[3][1] = (idx_c >= 0) ? state->stations[idx_c].lon_e6 : pts[1][1];

  Projection proj = build_projection(pts, 4, bounds);

  /* --- Faint grid --------------------------------------- */
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorLightGray);
#else
  /* On B&W skip the grid — too noisy at 1-bit */
  // (intentionally empty)
  bool draw_grid = false;
  (void)draw_grid;
#endif

#ifdef PBL_COLOR
  for (int i = 1; i < GRID_LINES; i++) {
    int16_t gx = (int16_t)(proj.map_x0 + proj.map_w * i / GRID_LINES);
    int16_t gy = (int16_t)(proj.map_y0 + proj.map_h * i / GRID_LINES);
    graphics_draw_line(ctx,
      GPoint(gx, (int16_t)proj.map_y0),
      GPoint(gx, (int16_t)(proj.map_y0 + proj.map_h)));
    graphics_draw_line(ctx,
      GPoint((int16_t)proj.map_x0, gy),
      GPoint((int16_t)(proj.map_x0 + proj.map_w), gy));
  }
#endif

  /* --- Adjacent stations (draw before selected so selected
         dot renders on top if they overlap) --------------- */
  if (idx_a >= 0) {
    GPoint pa = project(&proj,
      state->stations[idx_a].lat_e6,
      state->stations[idx_a].lon_e6);
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorDarkGray);
#else
    graphics_context_set_fill_color(ctx, GColorBlack);
#endif
    draw_station_dot(ctx, pa, state->stations[idx_a].name,
                     DOT_ADJACENT, false);
  }
  if (idx_c >= 0) {
    GPoint pc = project(&proj,
      state->stations[idx_c].lat_e6,
      state->stations[idx_c].lon_e6);
    draw_station_dot(ctx, pc, state->stations[idx_c].name,
                     DOT_ADJACENT, false);
  }

  /* --- Selected station --------------------------------- */
  GPoint pb = project(&proj,
    state->stations[idx_b].lat_e6,
    state->stations[idx_b].lon_e6);
  draw_station_dot(ctx, pb, selected->name, DOT_SELECTED, true);

  /* --- Own position ------------------------------------- */
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
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
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
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}