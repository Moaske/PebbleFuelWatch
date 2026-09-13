/* =============================================================
   map_window.c – FuelWatch map / detail view

   Buttons:
     UP / DOWN  – previous / next station (re-fetches the tile)
     SELECT     – toggle map view <-> detail view
     BACK       – return to the list

   MAP_PAD_TOP / BOT / SIDE live in station.h: the JS side sizes
   the tile bitmap from the same values, so they have exactly one
   definition and travel to the phone with the map request.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "list_window.h"
#include "map_window.h"

/* Declared in main.c */
void send_map_request(void);
void tile_buf_free(void);

/* ----------------------------------------------------------
   Per-platform detail metrics
---------------------------------------------------------- */
#ifdef PBL_PLATFORM_EMERY
  #define HDR_TEXT_Y     4    /* centres the label in the 32px bar */
  #define ROW_H         36
  #define VALUE_DROP     5    /* 24px value vs 20px label baseline */
  #define SIDE_PAD       8
#else
  #define HDR_TEXT_Y     1    /* centres the label in the 19px bar */
  #define ROW_H         30
  #define VALUE_DROP     4    /* 24px value vs 18px label baseline */
  #define SIDE_PAD       6
#endif

#define BBOX_PAD        0.15f
#define DOT_SELECTED     6
#define DOT_OWN          3
#define CROSSHAIR_ARM    6
#define LABEL_W         64
#define LABEL_H         14
#define LABEL_OFFSET_Y   3

/* ----------------------------------------------------------
   Module state
---------------------------------------------------------- */
static Window    *s_window        = NULL;
static Layer     *s_canvas        = NULL;
static TextLayer *s_loading_layer = NULL;
static GBitmap   *s_tile_bitmap   = NULL;
static bool       s_tile_ready    = false;
static bool       s_show_detail   = false;

/* Tile lat/lon bounds, read from the tile header */
static float s_tile_min_lat    = 0.f;
static float s_tile_max_lat    = 0.f;
static float s_tile_min_lon    = 0.f;
static float s_tile_max_lon    = 0.f;
static bool  s_tile_has_bounds = false;

/* Custom fonts */
static GFont s_font_header = NULL;
static GFont s_font_addr   = NULL;
static GFont s_font_label  = NULL;
static GFont s_font_value  = NULL;
static GFont s_font_small  = NULL;

/* A failed font load returns NULL; fall back rather than crash */
static GFont fnt(GFont f, const char *fallback_key) {
  return f ? f : fonts_get_system_font(fallback_key);
}

static void fonts_load_all(void) {
#ifdef PBL_PLATFORM_EMERY
  s_font_header = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_18));
  s_font_addr   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_20));
  s_font_label  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUI_20));
  s_font_small  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUI_18));
  s_font_value  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_24));
#else
  s_font_header = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_14));
  s_font_addr   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_18));
  s_font_label  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUI_18));
  s_font_small  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUI_14));
  s_font_value  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_18));
#endif
  /* Price and distance values keep the same size on every platform */
  

  APP_LOG(APP_LOG_LEVEL_INFO, "Map fonts loaded, heap=%d", (int)heap_bytes_free());
}

static void fonts_unload_all(void) {
  if (s_font_header) { fonts_unload_custom_font(s_font_header); s_font_header = NULL; }
  if (s_font_addr)   { fonts_unload_custom_font(s_font_addr);   s_font_addr   = NULL; }
  if (s_font_label)  { fonts_unload_custom_font(s_font_label);  s_font_label  = NULL; }
  if (s_font_value)  { fonts_unload_custom_font(s_font_value);  s_font_value  = NULL; }
  if (s_font_small)  { fonts_unload_custom_font(s_font_small);  s_font_small  = NULL; }
}

/* ----------------------------------------------------------
   Formatting helpers
---------------------------------------------------------- */
static void format_price(char *buf, size_t len, uint16_t mills) {
  if (mills == 0) {
    snprintf(buf, len, "---");
  } else {
    /* \xe2\x82\xac is the UTF-8 euro sign */
    snprintf(buf, len, "\xe2\x82\xac%u.%03u", mills / 1000, mills % 1000);
  }
}

static void format_dist(char *buf, size_t len, uint32_t dist_m) {
  if (dist_m < 1000) {
    snprintf(buf, len, "%um", (unsigned)dist_m);
  } else {
    snprintf(buf, len, "%u.%ukm",
             (unsigned)(dist_m / 1000),
             (unsigned)((dist_m % 1000) / 100));
  }
}

/* ----------------------------------------------------------
   Projection
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
    float lat_span = s_tile_max_lat - s_tile_min_lat;
    float lon_span = s_tile_max_lon - s_tile_min_lon;
    float lat_pad  = lat_span * BBOX_PAD;
    float lon_pad  = lon_span * BBOX_PAD;
    p.min_lat = s_tile_min_lat - lat_pad;
    p.max_lat = s_tile_max_lat + lat_pad;
    p.min_lon = s_tile_min_lon - lon_pad;
    p.max_lon = s_tile_max_lon + lon_pad;
  } else {
    /* Before the tile lands: bbox of own position + selected station */
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
   Shared chrome
---------------------------------------------------------- */
static void draw_title_bar(GContext *ctx, GRect bounds, const char *title) {
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorCobaltBlue);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, MAP_PAD_TOP),
                     0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, title,
    fnt(s_font_header, FONT_KEY_GOTHIC_14_BOLD),
    GRect(SIDE_PAD, HDR_TEXT_Y, bounds.size.w - SIDE_PAD * 2, MAP_PAD_TOP),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_station_dot(GContext *ctx, GPoint pt,
                             const char *label, uint8_t r) {
  /* White halo keeps the dot readable over any tile content */
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, pt, r + 2);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx,   GColorCobaltBlue);
  graphics_context_set_stroke_color(ctx, GColorCobaltBlue);
#else
  graphics_context_set_fill_color(ctx,   GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  graphics_fill_circle(ctx, pt, r);
  graphics_draw_circle(ctx, pt, r + 3);

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
   Detail view
   The address box is measured rather than fixed, so it grows to
   however many lines the text needs.
---------------------------------------------------------- */
static void draw_detail(GContext *ctx, GRect bounds, Station *stn) {
  AppState *state = app_state_get();
  int16_t   w     = bounds.size.w;
  int16_t   avail = w - SIDE_PAD * 2;
  int16_t   y     = MAP_PAD_TOP + 4;

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  draw_title_bar(ctx, bounds, stn->name);

  graphics_context_set_text_color(ctx, GColorBlack);

  /* --- Address: bold, measured so it wraps to as many lines as fit ---
     The cap is derived from what is left once the two value rows and
     the counter have their space, so it stays right if the metrics
     change. TrailingEllipsis wraps and marks any genuine overflow. */
  GFont addr_font  = fnt(s_font_addr, FONT_KEY_GOTHIC_18_BOLD);
  int16_t max_addr_h = bounds.size.h
                     - 24            /* position counter strip */
                     - (MAP_PAD_TOP + 4)
                     - 5             /* separator */
                     - ROW_H * 2     /* price + distance rows */
                     - 6;            /* gap below address */
  if (max_addr_h < 20) max_addr_h = 20;

  GSize addr_size = graphics_text_layout_get_content_size(
                      stn->address, addr_font,
                      GRect(0, 0, avail, max_addr_h),
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  if (addr_size.h > max_addr_h) addr_size.h = max_addr_h;

  graphics_draw_text(ctx, stn->address, addr_font,
    GRect(SIDE_PAD, y, avail, addr_size.h + 4),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  y += addr_size.h + 6;

  /* --- Separator --- */
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorLightGray);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  graphics_draw_line(ctx, GPoint(SIDE_PAD, y), GPoint(w - SIDE_PAD, y));
  y += 5;

  GFont label_font = fnt(s_font_label, FONT_KEY_GOTHIC_18);
  GFont value_font = fnt(s_font_value, FONT_KEY_GOTHIC_24_BOLD);
  int16_t label_w  = avail / 2;
  int16_t value_w  = avail - label_w;

  /* --- Fuel type / price --- */
  char price_buf[12];
  format_price(price_buf, sizeof(price_buf), stn->fuel_mills);

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, fuel_type_label(state->fuel_type), label_font,
    GRect(SIDE_PAD, y, label_w, ROW_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, price_buf, value_font,
    GRect(SIDE_PAD + label_w, y - VALUE_DROP, value_w, ROW_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
  y += ROW_H;

  /* --- Distance --- */
  char dist_buf[16];
  format_dist(dist_buf, sizeof(dist_buf), stn->dist_m);

  graphics_draw_text(ctx, "Distance", label_font,
    GRect(SIDE_PAD, y, label_w, ROW_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, dist_buf, value_font,
    GRect(SIDE_PAD + label_w, y - VALUE_DROP, value_w, ROW_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  /* --- Position counter, bottom right --- */
  char pos_buf[12];
  snprintf(pos_buf, sizeof(pos_buf), "%d/%d",
           state->selected_index + 1, state->count);
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorDarkGray);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, pos_buf,
    fnt(s_font_small, FONT_KEY_GOTHIC_14),
    GRect(SIDE_PAD, bounds.size.h - 24, avail, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

/* ----------------------------------------------------------
   Canvas draw callback
---------------------------------------------------------- */
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  AppState *state  = app_state_get();
  GRect     bounds = layer_get_bounds(layer);
  uint8_t   sel    = state->selected_index;
  Station  *stn    = &state->stations[sel];

  if (s_show_detail) {
    draw_detail(ctx, bounds, stn);
    return;
  }

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  /* --- Tile backdrop ------------------------------------ */
  if (s_tile_ready && s_tile_bitmap) {
    GRect tile_rect = GRect(MAP_PAD_SIDE, MAP_PAD_TOP,
                            bounds.size.w - MAP_PAD_SIDE * 2,
                            bounds.size.h - MAP_PAD_TOP - MAP_PAD_BOT);
    /* 1-bit bitmaps take stroke_color for 1-bits, fill_color for 0-bits */
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx,   GColorWhite);
#ifdef PBL_COLOR
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
#endif
    graphics_draw_bitmap_in_rect(ctx, s_tile_bitmap, tile_rect);
  }

  draw_title_bar(ctx, bounds, stn->name);

  /* --- Markers ----------------------------------------- */
  Proj proj = build_proj(bounds);

  GPoint pb = proj_point(&proj, stn->lat_e6, stn->lon_e6);
  draw_station_dot(ctx, pb, stn->name, DOT_SELECTED);

  GPoint own = proj_point(&proj, state->own_lat_e6, state->own_lon_e6);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx,   GColorWhite);
  graphics_fill_circle(ctx, own, DOT_OWN + 2);
  graphics_draw_line(ctx, GPoint(own.x - CROSSHAIR_ARM - 1, own.y),
                          GPoint(own.x + CROSSHAIR_ARM + 1, own.y));
  graphics_draw_line(ctx, GPoint(own.x, own.y - CROSSHAIR_ARM - 1),
                          GPoint(own.x, own.y + CROSSHAIR_ARM + 1));
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorRed);
  graphics_context_set_fill_color(ctx,   GColorRed);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,   GColorBlack);
#endif
  graphics_fill_circle(ctx, own, DOT_OWN);
  graphics_draw_line(ctx, GPoint(own.x - CROSSHAIR_ARM, own.y),
                          GPoint(own.x + CROSSHAIR_ARM, own.y));
  graphics_draw_line(ctx, GPoint(own.x, own.y - CROSSHAIR_ARM),
                          GPoint(own.x, own.y + CROSSHAIR_ARM));
}

/* ----------------------------------------------------------
   Station switching
---------------------------------------------------------- */
static void select_station(uint8_t new_index) {
  AppState *state = app_state_get();
  if (new_index >= state->count)          return;
  if (new_index == state->selected_index) return;

  state->selected_index = new_index;

  /* Keep the list highlight in step for when the user goes back */
  list_window_sync_selection();

  /* A new station means a new tile — drop the old one and re-request */
  s_tile_ready      = false;
  s_tile_has_bounds = false;
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }
  if (!s_show_detail && s_loading_layer) {
    layer_set_hidden(text_layer_get_layer(s_loading_layer), false);
  }
  layer_mark_dirty(s_canvas);
  send_map_request();
}

/* ----------------------------------------------------------
   Click handlers
---------------------------------------------------------- */
static void up_click(ClickRecognizerRef recognizer, void *ctx) {
  AppState *state = app_state_get();
  if (state->selected_index > 0) {
    select_station(state->selected_index - 1);
  }
}

static void down_click(ClickRecognizerRef recognizer, void *ctx) {
  AppState *state = app_state_get();
  if (state->count > 0 &&
      state->selected_index < (uint8_t)(state->count - 1)) {
    select_station(state->selected_index + 1);
  }
}

static void select_click(ClickRecognizerRef recognizer, void *ctx) {
  s_show_detail = !s_show_detail;
  if (s_loading_layer) {
    /* The detail view never shows the loading overlay */
    layer_set_hidden(text_layer_get_layer(s_loading_layer),
                     s_show_detail || s_tile_ready);
  }
  layer_mark_dirty(s_canvas);
}

static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP,     up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

/* ----------------------------------------------------------
   Bitmap from the reassembled tile buffer
   Header (21 bytes): w(2) h(2) bw(1) minLat(4) maxLat(4) minLon(4) maxLon(4)
   Pixels: 1-bit packed, ceil(w/8) bytes per row, LSB = leftmost pixel
---------------------------------------------------------- */
static void create_bitmap_from_tile(const uint8_t *buf, size_t len) {
  if (len < 21) return;

  int w = (buf[0] << 8) | buf[1];
  int h = (buf[2] << 8) | buf[3];

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

  int    src_stride  = (w + 7) / 8;
  size_t bw_expected = (size_t)src_stride * h;
  if (len < 21 + bw_expected) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Tile short: %d need %d",
            (int)(len - 21), (int)bw_expected);
    return;
  }

  GBitmap *bmp = gbitmap_create_blank(GSize(w, h), GBitmapFormat1Bit);
  if (!bmp) { APP_LOG(APP_LOG_LEVEL_ERROR, "Bitmap alloc failed"); return; }

  uint8_t  *dst        = gbitmap_get_data(bmp);
  uint16_t  dst_stride = gbitmap_get_bytes_per_row(bmp);
  for (int y = 0; y < h; y++) {
    memset(dst + (size_t)y * dst_stride, 0, dst_stride);
    memcpy(dst + (size_t)y * dst_stride,
           px  + (size_t)y * src_stride,
           src_stride);
  }
  s_tile_bitmap = bmp;
  APP_LOG(APP_LOG_LEVEL_INFO, "Tile bitmap %dx%d, heap=%d",
          w, h, (int)heap_bytes_free());
}

/* ----------------------------------------------------------
   Window callbacks
---------------------------------------------------------- */
static void window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  fonts_load_all();

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  s_loading_layer = text_layer_create(
    GRect(0, bounds.size.h / 2 - 10, bounds.size.w, 20));
  text_layer_set_text(s_loading_layer, "Loading map...");
  text_layer_set_text_alignment(s_loading_layer, GTextAlignmentCenter);
  text_layer_set_font(s_loading_layer, fnt(s_font_small, FONT_KEY_GOTHIC_14));
  text_layer_set_background_color(s_loading_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_loading_layer));

  window_set_click_config_provider(window, click_config_provider);
}

static void window_appear(Window *window) {
  s_tile_ready      = false;
  s_tile_has_bounds = false;
  s_show_detail     = false;
  if (s_tile_bitmap) {
    gbitmap_destroy(s_tile_bitmap);
    s_tile_bitmap = NULL;
  }
  layer_set_hidden(text_layer_get_layer(s_loading_layer), false);
  send_map_request();
}

static void window_unload(Window *window) {
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
  fonts_unload_all();
  s_tile_ready      = false;
  s_tile_has_bounds = false;
  s_show_detail     = false;
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
  if (s_loading_layer) {
    layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
  }
  layer_mark_dirty(s_canvas);
}

void map_window_tile_failed(void) {
  if (!s_window || !window_stack_contains_window(s_window)) return;
  if (s_loading_layer) {
    layer_set_hidden(text_layer_get_layer(s_loading_layer), true);
  }
  layer_mark_dirty(s_canvas);
}