/* =============================================================
   list_window.c – FuelWatch station list
   Two-line MenuLayer rows: name + price / address + distance
   Responsive to screen size (Basalt/Flint/Diorite 144px wide,
   Emery 200px wide).
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "list_window.h"
#include "map_window.h"

/* ----------------------------------------------------------
   Layout constants
---------------------------------------------------------- */
#define ROW_HEIGHT        38   // px — fits two lines on all platforms
#define HEADER_HEIGHT     22   // px — taller to fit station name font
#define FONT_NAME         FONT_KEY_GOTHIC_18_BOLD
#define FONT_DETAIL       FONT_KEY_GOTHIC_14
#define FONT_HEADER       FONT_KEY_GOTHIC_18_BOLD
#define PRICE_COL_WIDTH   52   // px reserved on the right for price
#define SIDE_PAD           4   // px left/right padding inside row
#define ROW_SEPARATOR_H    1   // px — separator line thickness
#define SCROLL_REPEAT_MS  100  // ms — button repeat interval when held

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

/* Format a price in mills (x1000), e.g. 1979 = 1.979 euro */
static void format_price(char *buf, size_t len, uint16_t mills) {
  if (mills == 0) {
    snprintf(buf, len, "---");
  } else {
    /* \xe2\x82\xac is the UTF-8 euro sign */
    snprintf(buf, len, "\xe2\x82\xac%u.%03u",
             mills / 1000,
             mills % 1000);
  }
}

/* Format distance: <1000m -> "400m", >=1000m -> "1.2km" */
static void format_dist(char *buf, size_t len, uint32_t dist_m) {
  if (dist_m < 1000) {
    snprintf(buf, len, "%um", (unsigned)dist_m);
  } else {
    uint32_t km_whole = dist_m / 1000;
    uint32_t km_tenth = (dist_m % 1000) / 100;
    snprintf(buf, len, "%u.%ukm", (unsigned)km_whole, (unsigned)km_tenth);
  }
}

/* ----------------------------------------------------------
   MenuLayer callbacks
---------------------------------------------------------- */

static uint16_t get_num_sections(MenuLayer *ml, void *ctx) {
  return 1;
}

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

  /* Black background */
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  /* White text */
  graphics_context_set_text_color(ctx, GColorWhite);

  /* "Nearby stations" — two thirds of width */
  graphics_draw_text(ctx,
    "Nearby stations",
    fonts_get_system_font(FONT_HEADER),
    GRect(SIDE_PAD, 1, w * 2 / 3, HEADER_HEIGHT),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentLeft,
    NULL);

  /* Fuel type label — remaining third, right-aligned */
  graphics_draw_text(ctx,
    fuel_type_label(state->fuel_type),
    fonts_get_system_font(FONT_HEADER),
    GRect(w * 2 / 3, 1, w / 3 - SIDE_PAD, HEADER_HEIGHT),
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

  /* --- Price column (right-aligned, top line) ----------- */
  char price_buf[12];
  format_price(price_buf, sizeof(price_buf), st->fuel_mills);

  graphics_draw_text(ctx,
    price_buf,
    fonts_get_system_font(FONT_NAME),
    GRect(w - PRICE_COL_WIDTH - SIDE_PAD, 1, PRICE_COL_WIDTH, 20),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentRight,
    NULL);

  /* --- Station name (left, top line) -------------------- */
  graphics_draw_text(ctx,
    st->name,
    fonts_get_system_font(FONT_NAME),
    GRect(SIDE_PAD, 1, w - PRICE_COL_WIDTH - SIDE_PAD * 3, 20),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentLeft,
    NULL);

    /* --- Detail line: address left, distance right, both bold --- */
  char dist_buf[16];
  format_dist(dist_buf, sizeof(dist_buf), st->dist_m);

  /* Address — left-aligned, clipped before distance */
  graphics_draw_text(ctx,
    st->address,
    fonts_get_system_font(FONT_DETAIL),
    GRect(SIDE_PAD, 21, w - 48 - SIDE_PAD * 2, 16),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentLeft,
    NULL);

  /* Distance — right-aligned, bold */
  graphics_draw_text(ctx,
    dist_buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(w - 48 - SIDE_PAD, 21, 48, 16),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentRight,
    NULL);
  
  /* --- 1px separator at bottom of cell ----------------- */
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
   Custom click handlers — prevent wrap-around at list ends
---------------------------------------------------------- */
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

  /* --- MenuLayer ---------------------------------------- */
  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections  = get_num_sections,
    .get_num_rows      = get_num_rows,
    .get_cell_height   = get_row_height,
    .get_header_height = get_header_height,
    .draw_header       = draw_header,
    .draw_row          = draw_row,
  });

  window_set_click_config_provider(window, click_config_provider);

#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_menu_layer, GColorCobaltBlue, GColorWhite);
#endif

  layer_add_child(root, menu_layer_get_layer(s_menu_layer));

  /* --- Status / loading layer --------------------------- */
  s_status_layer = text_layer_create(
    GRect(SIDE_PAD * 2, bounds.size.h / 3,
          bounds.size.w - SIDE_PAD * 4, 60));
  text_layer_set_font(s_status_layer,
    fonts_get_system_font(FONT_KEY_GOTHIC_18));
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

void list_window_destroy(void) {
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}