/* =============================================================
   map_window.c – FuelWatch map view
   OSM tile backdrop (dithered, chunked from phone) +
   station dots + ActionBar with navigate action.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "map_window.h"

/* Declared in main.c */
void send_map_request(void);
void tile_buf_free(void);

/* ----------------------------------------------------------
   Layout constants
   ActionBarLayer is 30px wide on regular, 40px on Emery.
   Map area is inset by action bar width on the right.
   Title bar height increased to compensate for smaller map.
---------------------------------------------------------- */
#ifdef PBL_PLATFORM_EMERY
  #define ACTION_BAR_W   40
  #define MAP_PAD_TOP    24
#else
  #define ACTION_BAR_W   30
  #define MAP_PAD_TOP    20
#endif

#define MAP_PAD_BOT    2
#define MAP_PAD_SIDE   6
#define BBOX_PAD       0.15f

#define DOT_SELECTED   6
#define DOT_OWN        3
#define CROSSHAIR_ARM  6
#define LABEL_W        64
#define LABEL_H        14
#define LABEL_OFFSET_Y 3

/* ----------------------------------------------------------
   Module state
---------------------------------------------------------- */
static Window          *s_window        = NULL;
static Layer           *s_canvas        = NULL;
static TextLayer       *s_loading_layer = NULL;
static ActionBarLayer  *s_action_bar    = NULL;
static GBitmap         *s_nav_icon      = NULL;
static GBitmap         *s_tile_bitmap   = NULL;
static bool             s_tile_ready    = false;
static bool             s_tile_failed   = false;

/* Tile lat/lon bounds */
static float  s_tile_min_lat    = 0.f;
static float  s_tile_max_lat    = 0.f;
static float  s_tile_min_lon    = 0.f;
static float  s_tile_max_lon    = 0.f;
static bool   s_tile_has_bounds = false;

/* ----------------------------------------------------------
   Projection
---------------------------------------------------------- */
typedef struct {
  float min_lat, max_lat, min_lon, max_lon;
  float map_x0, map_y0, map_w, map_h;
} Proj;

static Proj build_proj(GRect bounds) {
  Proj p;
  /* Map area excludes action bar on right */
  p.map_x0 = bounds.origin.x + MAP_PAD_SIDE;
  p.map_y0 = bounds.origin.y + MAP_PAD_TOP;
  p.map_w  = bounds.size.w - MAP_PAD_SIDE * 2 - ACTION_BAR_W;
  p.map_h  = bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT;

  if (s_tile_has_bounds) {
    float lat_span = s_tile_max_lat - s_tile_min_lat;
    float lon_span = s_tile_max_lon - s_tile_min_lon;
    float lat_pad  = lat_span * BBOX_PAD;
    float lon_pad  = lon_span * BBOX_PAD;
    p.min_lat = s_tile_min_lat - lat_pad;
    p.max_lat = s_tile_max_lat + lat_pad;
    p.min_lon = s_tile_min_lon - lon_pad;
    p.max_lon = s_tile_max_lon + lon_pad;
  } else {
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
static void draw_station_dot(GContext *ctx, GPoint pt,
                              const char *label, uint8_t r, bool selected) {
  /* White halo */
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
                            bounds.size.w - MAP_PAD_SIDE * 2 - ACTION_BAR_W,
                            bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx,  GColorWhite);
#ifdef PBL_COLOR
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
#endif
    graphics_draw_bitmap_in_rect(ctx, s_tile_bitmap, tile_rect);
#ifdef PBL_COLOR
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
#endif
  }

  /* --- Title bar --------------------------------------- */
  uint8_t  sel = state->selected_index;
  Station *stn = &state->stations[sel];
  char     title[32];
  snprintf(title, sizeof(title), "%s", stn->name);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx,
    GRect(0, 0, bounds.size.w - ACTION_BAR_W, MAP_PAD_TOP),
    0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title,
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(4, 1, bounds.size.w - ACTION_BAR_W - 8, MAP_PAD_TOP - 2),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  /* --- Dots -------------------------------------------- */
  Proj proj = build_proj(bounds);

  /* Selected station */
  GPoint pb = proj_point(&proj, stn->lat_e6, stn->lon_e6);
  draw_station_dot(ctx, pb, stn->name, DOT_SELECTED, true);

  /* Own position crosshair */
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Own pos e6: %ld,%ld",
          (long)state->own_lat_e6, (long)state->own_lon_e6);
  GPoint own = proj_point(&proj, state->own_lat_e6, state->own_lon_e6);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Own pixel: %d,%d screen: %dx%d",
          (int)own.x, (int)own.y,
          (int)(proj.map_x0 + proj.map_w),
          (int)(proj.map_y0 + proj.map_h));

  /* White halo then coloured dot */
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx,  GColorWhite);
  graphics_fill_circle(ctx, own, DOT_OWN + 2);
  graphics_draw_line(ctx,
    GPoint(own.x - CROSSHAIR_ARM - 1, own.y),
    GPoint(own.x + CROSSHAIR_ARM + 1, own.y));
  graphics_draw_line(ctx,
    GPoint(own.x, own.y - CROSSHAIR_ARM - 1),
    GPoint(own.x, own.y + CROSSHAIR_ARM + 1));
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_context_set_fill_color(ctx,  GColorRed);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,  GColorBlack);
#endif
  graphics_fill_circle(ctx, own, DOT_OWN);
  graphics_draw_line(ctx,
    GPoint(own.x - CROSSHAIR_ARM, own.y),
    GPoint(own.x + CROSSHAIR_ARM, own.y));
  graphics_draw_line(ctx,
    GPoint(own.x, own.y - CROSSHAIR_ARM),
    GPoint(own.x, own.y + CROSSHAIR_ARM));
}

/* ----------------------------------------------------------
   ActionBar click handler — navigate to selected station
---------------------------------------------------------- */
static void action_bar_select_click(ClickRecognizerRef recognizer, void *ctx) {
  AppState *state = app_state_get();
  uint8_t   sel   = state->selected_index;
  Station  *stn   = &state->stations[sel];

  /* Send navigate request to phone JS layer */
  /* Pack lat,lon as "lat_e6,lon_e6" string on Navigate key */
  char nav_str[32];
  snprintf(nav_str, sizeof(nav_str), "%ld,%ld",
           (long)stn->lat_e6, (long)stn->lon_e6);
  APP_LOG(APP_LOG_LEVEL_INFO, "Navigate to: %s", nav_str);

  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_cstring(out, MESSAGE_KEY_Navigate, nav_str);
    app_message_outbox_send();
  }
}

static void action_bar_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, action_bar_select_click);
}

/* ----------------------------------------------------------
   Bitmap creation from tile buffer
   Header (21 bytes): w(2) h(2) bw(1) minLat(4) maxLat(4) minLon(4) maxLon(4)
   Pixels: 1-bit packed, ceil(w/8) bytes per row
---------------------------------------------------------- */
static void create_bitmap_from_tile(const uint8_t *buf, size_t len) {
  if (len < 19) return;

  int w    = (buf[0] << 8) | buf[1];
  int h    = (buf[2] << 8) | buf[3];
  int isBW = buf[4];
  (void)isBW;

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

  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Tile header: %dx%d bw=%d len=%d", w, h, isBW, (int)len);
  APP_LOG(APP_LOG_LEVEL_INFO, "Creating bitmap %dx%d bw=%d, heap=%d",
          w, h, isBW, (int)heap_bytes_free());

  int    src_stride  = (w + 7) / 8;
  size_t bw_expected = (size_t)src_stride * h;
  if (len < 21 + bw_expected) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "1-bit tile short: %d need %d",
            (int)(len-21), (int)bw_expected);
    return;
  }

  GBitmap *bmp = gbitmap_create_blank(GSize(w, h), GBitmapFormat1Bit);
  if (!bmp) { APP_LOG(APP_LOG_LEVEL_ERROR, "1-bit bitmap alloc failed"); return; }

  uint8_t  *dst       = gbitmap_get_data(bmp);
  uint16_t  dst_stride = gbitmap_get_bytes_per_row(bmp);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Strides: src=%d dst=%d", src_stride, (int)dst_stride);

  for (int y = 0; y < h; y++) {
    memset(dst + (size_t)y * dst_stride, 0, dst_stride);
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

  /* Canvas fills full window — action bar overlays right side */
  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  /* Loading overlay */
  s_loading_layer = text_layer_create(
    GRect(0, bounds.size.h / 2 - 10,
          bounds.size.w - ACTION_BAR_W, 20));
  text_layer_set_text(s_loading_layer, "Loading map...");
  text_layer_set_text_alignment(s_loading_layer, GTextAlignmentCenter);
  text_layer_set_font(s_loading_layer,
    fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_background_color(s_loading_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_loading_layer));

  /* Navigation icon — platform sized */
#ifdef PBL_PLATFORM_EMERY
  s_nav_icon = gbitmap_create_with_resource(RESOURCE_ID_NAVIGATE_ICON25);
#else
  s_nav_icon = gbitmap_create_with_resource(RESOURCE_ID_NAVIGATE_ICON18);
#endif

  /* ActionBarLayer on the right */
  s_action_bar = action_bar_layer_create();
  if (s_nav_icon) {
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_nav_icon);
  }
#ifdef PBL_COLOR
  action_bar_layer_set_background_color(s_action_bar, GColorCobaltBlue);
#endif
  /* add_to_window must come BEFORE set_click_config_provider
     so the action bar owns the window click config first */
  action_bar_layer_add_to_window(s_action_bar, window);
  action_bar_layer_set_click_config_provider(s_action_bar,
                                             action_bar_click_config);
}

static void window_appear(Window *window) {
  s_tile_ready      = false;
  s_tile_failed     = false;
  s_tile_has_bounds = false;
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }
  layer_set_hidden(text_layer_get_layer(s_loading_layer), false);
  send_map_request();
}

static void window_unload(Window *window) {
  /* Destroy in order: bitmap, action bar icon, action bar, layers */
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }
  if (s_action_bar) {
    action_bar_layer_remove_from_window(s_action_bar);
    action_bar_layer_destroy(s_action_bar);
    s_action_bar = NULL;
  }
  if (s_nav_icon) {
    gbitmap_destroy(s_nav_icon);
    s_nav_icon = NULL;
  }
  if (s_canvas) {
    layer_destroy(s_canvas);
    s_canvas = NULL;
  }
  if (s_loading_layer) {
    text_layer_destroy(s_loading_layer);
    s_loading_layer = NULL;
  }
  s_tile_ready      = false;
  s_tile_failed     = false;
  s_tile_has_bounds = false;
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
    free(buf);
    return;
  }
  create_bitmap_from_tile(buf, len);
  free(buf);
  s_tile_ready = (s_tile_bitmap != NULL);
  layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
  layer_mark_dirty(s_canvas);
}

void map_window_tile_failed(void) {
  if (!s_window || !window_stack_contains_window(s_window)) return;
  s_tile_failed = true;
  layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
  layer_mark_dirty(s_canvas);
}