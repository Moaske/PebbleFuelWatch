/* =============================================================
   list_window.c – FuelWatch station list
   Two-line MenuLayer rows: name + price / address + distance
   Responsive to screen size and platform:
     Emery  (200x228): larger custom fonts, touch handlers
     Others (144x168): smaller custom fonts, button handlers
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "list_window.h"
#include "map_window.h"

/* ----------------------------------------------------------
   Platform-responsive layout constants
---------------------------------------------------------- */
#ifdef PBL_PLATFORM_EMERY
  #define ROW_HEIGHT       52
  #define HEADER_HEIGHT    26
  #define PRICE_COL_WIDTH  62
  #define DIST_COL_WIDTH   54
  #define NAME_Y            2
  #define NAME_H           24
  #define DETAIL_Y         26
  #define DETAIL_H         26
#else
  #define ROW_HEIGHT       43
  #define HEADER_HEIGHT    20
  #define PRICE_COL_WIDTH  50
  #define DIST_COL_WIDTH   46
  #define NAME_Y            1
  #define NAME_H           20
  #define DETAIL_Y         20
  #define DETAIL_H         21
#endif

#define SIDE_PAD          4    // px left/right padding
#define ROW_SEPARATOR_H   1    // px separator thickness
#define SCROLL_REPEAT_MS  100  // ms button repeat interval

/* ----------------------------------------------------------
   Custom font handles — loaded in window_load, freed in window_unload
---------------------------------------------------------- */
static GFont s_font_name;      // station name + price (bold, larger)
static GFont s_font_detail;    // address (regular, smaller)
static GFont s_font_header;    // header text (bold)
static GFont s_font_distance;  // distance (bold, same size as detail)

/* ----------------------------------------------------------
   Module-level state
---------------------------------------------------------- */
static Window    *s_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_status_layer;
static bool       s_data_ready = false;

/* ----------------------------------------------------------
   Helpers
---------------------------------------------------------- */

static void format_price(char *buf, size_t len, uint16_t mills) {
  if (mills == 0) {
    snprintf(buf, len, "---");
  } else {
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
   MenuLayer callbacks
---------------------------------------------------------- */

static uint16_t get_num_sections(MenuLayer *ml, void *ctx) { return 1; }

static uint16_t get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  AppState *state = app_state_get();
  if (!s_data_ready || state->status != STATUS_OK) return 0;
  return state->count;
}

static int16_t get_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return ROW_HEIGHT;
}

static int16_t get_header_height(MenuLayer *ml, uint16_t section, void *ctx) {
  return HEADER_HEIGHT;
}

static void draw_header(GContext *ctx, const Layer *cell_layer,
                        uint16_t section, void *cb_ctx) {
  AppState *state  = app_state_get();
  GRect     bounds = layer_get_bounds(cell_layer);
  int16_t   w      = bounds.size.w;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);

  graphics_draw_text(ctx,
    "Nearby you:",
    s_font_header,
    GRect(SIDE_PAD, 1, w * 2 / 3, HEADER_HEIGHT - 2),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentLeft,
    NULL);

  graphics_draw_text(ctx,
    fuel_type_label(state->fuel_type),
    s_font_header,
    GRect(w * 2 / 3, 1, w / 3 - SIDE_PAD, HEADER_HEIGHT - 2),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentRight,
    NULL);
}

static void draw_row(GContext *ctx, const Layer *cell_layer,
                     MenuIndex *idx, void *cb_ctx) {
  AppState *state     = app_state_get();
  Station  *st        = &state->stations[idx->row];
  GRect     bounds    = layer_get_bounds(cell_layer);
  bool      highlight = menu_layer_is_index_selected(s_menu_layer, idx);
  int16_t   w         = bounds.size.w;

  graphics_context_set_text_color(ctx, highlight ? GColorWhite : GColorBlack);

  /* Price — right-aligned, top line */
  char price_buf[12];
  format_price(price_buf, sizeof(price_buf), st->fuel_mills);
  graphics_draw_text(ctx, price_buf, s_font_name,
    GRect(w - PRICE_COL_WIDTH - SIDE_PAD, NAME_Y, PRICE_COL_WIDTH, NAME_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  /* Station name — left, top line */
  graphics_draw_text(ctx, st->name, s_font_name,
    GRect(SIDE_PAD, NAME_Y, w - PRICE_COL_WIDTH - SIDE_PAD * 3, NAME_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  /* Address — left, detail line */
  graphics_draw_text(ctx, st->address, s_font_detail,
    GRect(SIDE_PAD, DETAIL_Y, w - DIST_COL_WIDTH - SIDE_PAD * 2, DETAIL_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  /* Distance — right, detail line, bold */
  char dist_buf[16];
  format_dist(dist_buf, sizeof(dist_buf), st->dist_m);
  graphics_draw_text(ctx, dist_buf, s_font_distance,
    GRect(w - DIST_COL_WIDTH - SIDE_PAD, DETAIL_Y, DIST_COL_WIDTH, DETAIL_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  /* Separator */
  if (!highlight) {
    GColor sep_color;
#ifdef PBL_COLOR
    sep_color = GColorLightGray;
#else
    sep_color = GColorBlack;
#endif
    graphics_context_set_stroke_color(ctx, sep_color);
    graphics_draw_line(ctx,
      GPoint(0,             bounds.size.h - ROW_SEPARATOR_H),
      GPoint(bounds.size.w, bounds.size.h - ROW_SEPARATOR_H));
  }
}

/* ----------------------------------------------------------
   Click / touch handlers
   Emery: MenuLayer native click+touch config handles everything.
   Others: custom provider with clamped UP/DOWN (no wrap).
---------------------------------------------------------- */
#ifdef PBL_PLATFORM_EMERY

static void select_click(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  AppState *state = app_state_get();
  state->selected_index = (uint8_t)idx->row;
  map_window_push();
}

#else

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  MenuIndex idx = menu_layer_get_selected_index(s_menu_layer);
  if (idx.row > 0) {
    idx.row--;
    menu_layer_set_selected_index(s_menu_layer, idx, MenuRowAlignCenter, true);
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  AppState *state = app_state_get();
  MenuIndex idx   = menu_layer_get_selected_index(s_menu_layer);
  if (state->count > 0 && idx.row < (uint16_t)(state->count - 1)) {
    idx.row++;
    menu_layer_set_selected_index(s_menu_layer, idx, MenuRowAlignCenter, true);
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  MenuIndex idx   = menu_layer_get_selected_index(s_menu_layer);
  AppState *state = app_state_get();
  state->selected_index = (uint8_t)idx.row;
  map_window_push();
}

static void click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   SCROLL_REPEAT_MS, up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, SCROLL_REPEAT_MS, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

#endif

/* ----------------------------------------------------------
   Status / loading text layer
---------------------------------------------------------- */
static void update_status_layer(void) {
  AppState *state = app_state_get();

  if (s_data_ready && state->status == STATUS_OK) {
    layer_set_hidden(text_layer_get_layer(s_status_layer), true);
    menu_layer_reload_data(s_menu_layer);
    return;
  }

  layer_set_hidden(text_layer_get_layer(s_status_layer), false);

  const char *msg;
  if (!s_data_ready) {
    msg = "Locating\nnearby stations...";
  } else {
    switch (state->status) {
      case STATUS_BLOCKED: msg = "Service unavailable\n(rate limited)"; break;
      case STATUS_LOCERR:  msg = "Could not determine\nyour location";  break;
      default:             msg = "Could not fetch\nfuel prices";        break;
    }
  }
  text_layer_set_text(s_status_layer, msg);
}

/* ----------------------------------------------------------
   Window callbacks
---------------------------------------------------------- */
static void window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);

  /* Load custom fonts */
#ifdef PBL_PLATFORM_EMERY
  s_font_name     = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_20));
  s_font_detail   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUI_18));
  s_font_header   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_20));
  s_font_distance = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_18));
#else
  s_font_name     = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_16));
  s_font_detail   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUI_14));
  s_font_header   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_14));
  s_font_distance = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_SEGOEUIB_14));
#endif

  /* MenuLayer */
  s_menu_layer = menu_layer_create(bounds);

#ifdef PBL_PLATFORM_EMERY
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = get_num_sections,
    .get_num_rows      = get_num_rows,
    .get_cell_height   = get_row_height,
    .get_header_height = get_header_height,
    .draw_header       = draw_header,
    .draw_row          = draw_row,
    .select_click      = select_click,
  });
#else
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = get_num_sections,
    .get_num_rows      = get_num_rows,
    .get_cell_height   = get_row_height,
    .get_header_height = get_header_height,
    .draw_header       = draw_header,
    .draw_row          = draw_row,
  });
  window_set_click_config_provider(window, click_config_provider);
#endif

#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_menu_layer, GColorCobaltBlue, GColorWhite);
#endif

    layer_add_child(root, menu_layer_get_layer(s_menu_layer));
#ifdef PBL_PLATFORM_EMERY
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  app_touch_navigation_enable(true);  // opt into system touch nav
#endif

  /* Status layer */
  s_status_layer = text_layer_create(
    GRect(SIDE_PAD * 2, bounds.size.h / 3,
          bounds.size.w - SIDE_PAD * 4, 80));
  text_layer_set_font(s_status_layer, s_font_name);
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_status_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  update_status_layer();
}

static void window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_status_layer);
  s_menu_layer   = NULL;
  s_status_layer = NULL;

  /* Unload custom fonts */
  fonts_unload_custom_font(s_font_name);
  fonts_unload_custom_font(s_font_detail);
  fonts_unload_custom_font(s_font_header);
  fonts_unload_custom_font(s_font_distance);
}

/* ----------------------------------------------------------
   Public API
---------------------------------------------------------- */
void list_window_push(void) {
  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

void list_window_data_arrived(void) {
  s_data_ready = true;
  if (s_window && window_stack_contains_window(s_window)) {
    update_status_layer();
  }
}

void list_window_sync_selection(void) {
  if (!s_menu_layer) return;
  AppState *state = app_state_get();
  if (state->selected_index >= state->count) return;
  MenuIndex idx = MenuIndex(0, state->selected_index);
  menu_layer_set_selected_index(s_menu_layer, idx, MenuRowAlignCenter, false);
}

void list_window_destroy(void) {
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}