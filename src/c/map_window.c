/* =============================================================
   map_window.c – FuelWatch map view
   OSM tile backdrop (dithered, chunked from phone) +
   station dots projected from lat/lon coordinates.
   All resources destroyed on window unload.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "map_window.h"

/* Declared in main.c */
void send_map_request(void);
void tile_buf_free(void);

/* ----------------------------------------------------------
   Drawing constants (match JS projection constants exactly)
---------------------------------------------------------- */
#define MAP_PAD_TOP    16
#define MAP_PAD_BOT     2
#define MAP_PAD_SIDE    6
#define DOT_SELECTED    6
#define DOT_ADJACENT    4
#define DOT_OWN         3
#define CROSSHAIR_ARM   6
#define LABEL_W        72
#define LABEL_H        14
#define LABEL_OFFSET_Y  3
#define BBOX_PAD     0.20f

/* ----------------------------------------------------------
   Module state
---------------------------------------------------------- */
static Window    *s_window       = NULL;
static Layer     *s_canvas       = NULL;
static TextLayer *s_loading_layer = NULL;
static GBitmap   *s_tile_bitmap  = NULL;
static bool       s_tile_ready   = false;
static bool       s_tile_failed  = false;

/* Tile lat/lon bounds — set when tile arrives, used for dot projection */
static float      s_tile_min_lat = 0.f;
static float      s_tile_max_lat = 0.f;
static float      s_tile_min_lon = 0.f;
static float      s_tile_max_lon = 0.f;
static bool       s_tile_has_bounds = false;

/* ----------------------------------------------------------
   Projection (station dots — mirrors JS buildProjectionBbox)
---------------------------------------------------------- */
typedef struct {
  float min_lat, max_lat, min_lon, max_lon;
  float map_x0, map_y0, map_w, map_h;
} Proj;

static Proj build_proj(GRect bounds) {
  Proj p;
  p.map_x0 = bounds.origin.x + MAP_PAD_SIDE;
  p.map_y0 = bounds.origin.y + MAP_PAD_TOP;
  p.map_w  = bounds.size.w - MAP_PAD_SIDE * 2;
  p.map_h  = bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT;

  if (s_tile_has_bounds) {
    /* Use tile bounds with 5% padding so points near the tile edge
       don't project right to the screen boundary */
    float lat_span = s_tile_max_lat - s_tile_min_lat;
    float lon_span = s_tile_max_lon - s_tile_min_lon;
    float lat_pad  = lat_span * 0.15f;
    float lon_pad  = lon_span * 0.15f;
    p.min_lat = s_tile_min_lat - lat_pad;
    p.max_lat = s_tile_max_lat + lat_pad;
    p.min_lon = s_tile_min_lon - lon_pad;
    p.max_lon = s_tile_max_lon + lon_pad;
  } else {
    /* Fallback: simple bbox around own pos and selected station */
    AppState *state = app_state_get();
    uint8_t   sel   = state->selected_index;
    float ownLat = state->own_lat_e6 / 1000000.f;
    float ownLon = state->own_lon_e6 / 1000000.f;
    float stnLat = state->stations[sel].lat_e6 / 1000000.f;
    float stnLon = state->stations[sel].lon_e6 / 1000000.f;
    float minLat = ownLat < stnLat ? ownLat : stnLat;
    float maxLat = ownLat > stnLat ? ownLat : stnLat;
    float minLon = ownLon < stnLon ? ownLon : stnLon;
    float maxLon = ownLon > stnLon ? ownLon : stnLon;
    float ls = maxLat - minLat, lo = maxLon - minLon;
    if (ls < 0.005f) { float m=(minLat+maxLat)/2; minLat=m-0.0025f; maxLat=m+0.0025f; }
    if (lo < 0.005f) { float m=(minLon+maxLon)/2; minLon=m-0.0025f; maxLon=m+0.0025f; }
    p.min_lat = minLat - ls*BBOX_PAD; p.max_lat = maxLat + ls*BBOX_PAD;
    p.min_lon = minLon - lo*BBOX_PAD; p.max_lon = maxLon + lo*BBOX_PAD;
  }
  return p;
}

static GPoint proj_point(Proj *p, int32_t lat_e6, int32_t lon_e6) {
  float lat = lat_e6 / 1000000.f;
  float lon = lon_e6 / 1000000.f;
  int16_t x = (int16_t)(p->map_x0 + (lon-p->min_lon)/(p->max_lon-p->min_lon)*p->map_w);
  int16_t y = (int16_t)(p->map_y0 + (1.f-(lat-p->min_lat)/(p->max_lat-p->min_lat))*p->map_h);
  return GPoint(x, y);
}

/* ----------------------------------------------------------
   Draw helpers
---------------------------------------------------------- */
static void draw_station_dot(GContext *ctx, GPoint pt, const char *label,
                              uint8_t r, bool selected) {
  /* White halo behind dot so it's visible over any tile content */
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, pt, r + 2);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, selected ? GColorCobaltBlue : GColorDarkGray);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_circle(ctx, pt, r);

  if (selected) {
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorCobaltBlue);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_draw_circle(ctx, pt, r + 3);
  }

  /* White background box behind label for readability over tile */
  GRect label_rect = GRect(pt.x - LABEL_W/2,
                           pt.y - r - LABEL_OFFSET_Y - LABEL_H,
                           LABEL_W, LABEL_H);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, label_rect, 0, GCornerNone);
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

  /* White background */
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  /* --- Tile backdrop ------------------------------------ */
  if (s_tile_ready && s_tile_bitmap) {
    GRect tile_rect = GRect(MAP_PAD_SIDE, MAP_PAD_TOP,
                            bounds.size.w - MAP_PAD_SIDE * 2,
                            bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT);
    /* For 1-bit bitmaps on colour displays: set context colours so
       1-bits render as black (roads/features) on white background */
    /* GCompOpAssign copies bitmap pixels directly to display.
       For 1-bit on colour: Pebble uses current fill_color for 0-bits
       and stroke_color for 1-bits. Set black-on-white explicitly. */
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx,  GColorWhite);
#ifdef PBL_COLOR
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
#endif
    graphics_draw_bitmap_in_rect(ctx, s_tile_bitmap, tile_rect);
    /* Reset compositing mode for subsequent drawing */
#ifdef PBL_COLOR
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
#endif
  }

  /* --- Title bar --------------------------------------- */
  uint8_t  sel  = state->selected_index;
  Station *stn  = &state->stations[sel];
  char     title[32];
  snprintf(title, sizeof(title), "%s", stn->name);
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

  /* --- Two dots: selected station + own position ------- */
  Proj proj = build_proj(bounds);

  /* Selected station */
  GPoint pb = proj_point(&proj, stn->lat_e6, stn->lon_e6);
  draw_station_dot(ctx, pb, stn->name, DOT_SELECTED, true);

  /* Own position crosshair — white halo first for visibility over tile */
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Own pos e6: %ld,%ld bounds: %.4f-%.4f / %.4f-%.4f",
          (long)state->own_lat_e6, (long)state->own_lon_e6,
          (double)proj.min_lat, (double)proj.max_lat,
          (double)proj.min_lon, (double)proj.max_lon);
  GPoint own = proj_point(&proj, state->own_lat_e6, state->own_lon_e6);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Own pixel: %d,%d screen: %dx%d",
          (int)own.x, (int)own.y,
          (int)(proj.map_x0 + proj.map_w), (int)(proj.map_y0 + proj.map_h));
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx,  GColorWhite);
  graphics_fill_circle(ctx, own, DOT_OWN + 2);
  graphics_draw_line(ctx, GPoint(own.x - CROSSHAIR_ARM - 1, own.y),
                          GPoint(own.x + CROSSHAIR_ARM + 1, own.y));
  graphics_draw_line(ctx, GPoint(own.x, own.y - CROSSHAIR_ARM - 1),
                          GPoint(own.x, own.y + CROSSHAIR_ARM + 1));
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_context_set_fill_color(ctx,  GColorRed);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,  GColorBlack);
#endif
  graphics_fill_circle(ctx, own, DOT_OWN);
  graphics_draw_line(ctx, GPoint(own.x - CROSSHAIR_ARM, own.y),
                          GPoint(own.x + CROSSHAIR_ARM, own.y));
  graphics_draw_line(ctx, GPoint(own.x, own.y - CROSSHAIR_ARM),
                          GPoint(own.x, own.y + CROSSHAIR_ARM));
}

/* ----------------------------------------------------------
   Bitmap creation from received tile buffer
   Header: [w_hi, w_lo, h_hi, h_lo, bw_flag, pixels...]
   Colour: GColor8 (1 byte/pixel)
   B&W:    1-bit packed (ceil(w/8) bytes/row, MSB=leftmost pixel)
---------------------------------------------------------- */
static void create_bitmap_from_tile(const uint8_t *buf, size_t len) {
  if (len < 19) return;

  int w    = (buf[0] << 8) | buf[1];
  int h    = (buf[2] << 8) | buf[3];
  int isBW = buf[4];
  
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Tile header: %dx%d bw=%d len=%d",
        w, h, isBW, (int)len);

  /* Read tile bounds from header (int32 big-endian, e6 units) */
  int32_t minLatE6 = (int32_t)((buf[5]<<24)|(buf[6]<<16)|(buf[7]<<8)|buf[8]);
  int32_t maxLatE6 = (int32_t)((buf[9]<<24)|(buf[10]<<16)|(buf[11]<<8)|buf[12]);
  int32_t minLonE6 = (int32_t)((buf[13]<<24)|(buf[14]<<16)|(buf[15]<<8)|buf[16]);
  int32_t maxLonE6 = (int32_t)((buf[17]<<24)|(buf[18]<<16)|(buf[19]<<8)|buf[20]);

  s_tile_min_lat    = minLatE6 / 1000000.f;
  s_tile_max_lat    = maxLatE6 / 1000000.f;
  s_tile_min_lon    = minLonE6 / 1000000.f;
  s_tile_max_lon    = maxLonE6 / 1000000.f;
  s_tile_has_bounds = true;

  const uint8_t *px = buf + 21;

  if (w <= 0 || h <= 0 || w > 300 || h > 300) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Bad tile dims %dx%d", w, h);
    return;
  }

  /* Destroy previous bitmap before allocating new one */
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }

  APP_LOG(APP_LOG_LEVEL_INFO, "Creating bitmap %dx%d bw=%d, heap=%d",
          w, h, isBW, (int)heap_bytes_free());

  /* 1-bit packed bitmap — works on all platforms, smallest transfer */
  (void)isBW;  /* always B&W for now */
  int src_stride = (w + 7) / 8;
  size_t bw_expected = (size_t)src_stride * h;
  if (len < 21 + bw_expected) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "1-bit tile short: %d need %d",
            (int)(len - 21), (int)bw_expected);
    return;
  }
  GBitmap *bmp = gbitmap_create_blank(GSize(w, h), GBitmapFormat1Bit);
  if (!bmp) { APP_LOG(APP_LOG_LEVEL_ERROR, "1-bit bitmap alloc failed"); return; }
  uint8_t  *dst       = gbitmap_get_data(bmp);
  uint16_t  dst_stride = gbitmap_get_bytes_per_row(bmp);
  /* dst_stride may be padded to 4-byte boundary (e.g. 28 for w=188)
     src_stride is exactly ceil(w/8) = 24 bytes — copy only src bytes per row */
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Strides: src=%d dst=%d", src_stride, (int)dst_stride);
  for (int y = 0; y < h; y++) {
    memset(dst + (size_t)y * dst_stride, 0, dst_stride);  // clear padding bytes
    memcpy(dst + (size_t)y * dst_stride,
           px  + (size_t)y * src_stride,
           src_stride);
  }
  s_tile_bitmap = bmp;

  APP_LOG(APP_LOG_LEVEL_INFO, "Bitmap created, heap=%d", (int)heap_bytes_free());
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
  s_tile_ready      = false;
  s_tile_failed     = false;
  s_tile_has_bounds = false;
  /* Destroy any previous tile so heap is free before new arrives */
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }
  layer_set_hidden(text_layer_get_layer(s_loading_layer), false);
  send_map_request();
}

static void window_unload(Window *window) {
  /* Destroy all resources in order: bitmap first, then layers */
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }
  if (s_canvas) {
    layer_destroy(s_canvas);
    s_canvas = NULL;
  }
  if (s_loading_layer) {
    text_layer_destroy(s_loading_layer);
    s_loading_layer = NULL;
  }
  s_tile_ready  = false;
  s_tile_failed = false;
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

void map_window_tile_arrived(uint8_t *buf, size_t len) {
  if (!s_window || !window_stack_contains_window(s_window)) {
    /* Window was closed before tile arrived — free the buffer */
    free(buf);
    return;
  }
  create_bitmap_from_tile(buf, len);
  free(buf);  /* Done with the reassembly buffer */

  s_tile_ready = (s_tile_bitmap != NULL);
  layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
  layer_mark_dirty(s_canvas);
}

void map_window_tile_failed(void) {
  if (!s_window || !window_stack_contains_window(s_window)) return;
  s_tile_failed = true;
  /* Hide loading, show dots-only map */
  layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
  layer_mark_dirty(s_canvas);
}