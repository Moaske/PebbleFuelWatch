/* =============================================================
   main.c – FuelWatch
   AppMessage handling, window stack, tile chunk reassembly.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "list_window.h"
#include "map_window.h"

/* ----------------------------------------------------------
   Global app state
---------------------------------------------------------- */
static AppState s_state;

AppState *app_state_get(void) {
  return &s_state;
}

/* Tile reassembly buffer — malloced on first chunk, freed after
   bitmap is created in map_window.c */
static uint8_t *s_tile_buf      = NULL;
static size_t   s_tile_buf_size = 0;
static size_t   s_tile_received = 0;
static int      s_tile_chunks_n = 0;

void tile_buf_free(void) {
  if (s_tile_buf) {
    free(s_tile_buf);
    s_tile_buf      = NULL;
    s_tile_buf_size = 0;
    s_tile_received = 0;
    s_tile_chunks_n = 0;
  }
}

#define INBOX_SIZE  2560
#define OUTBOX_SIZE  256

/* ----------------------------------------------------------
   Parse station line
   Format: id|name|address|dist_m|fuel_mills|lat_e6|lon_e6
---------------------------------------------------------- */
static bool parse_station_line(char *line, Station *out) {
  char *p   = line;
  char *sep;
  int field = 0;

  while (field < 7) {
    sep = strchr(p, '|');
    if (sep) *sep = '\0';
    switch (field) {
      case 0: /* id — skip */ break;
      case 1: strncpy(out->name,    p, STATION_NAME_LEN - 1);
              out->name[STATION_NAME_LEN - 1] = '\0';    break;
      case 2: strncpy(out->address, p, STATION_ADDR_LEN - 1);
              out->address[STATION_ADDR_LEN - 1] = '\0';  break;
      case 3: out->dist_m     = (uint32_t)atoi(p); break;
      case 4: out->fuel_mills = (uint16_t)atoi(p); break;
      case 5: out->lat_e6     = (int32_t) atoi(p); break;
      case 6: out->lon_e6     = (int32_t) atoi(p); break;
    }
    field++;
    if (!sep) break;
    p = sep + 1;
  }
  return (field >= 6);
}

static void parse_stations_payload(const char *payload) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Payload length: %d", (int)strlen(payload));
  char *buf = malloc(strlen(payload) + 1);
  if (!buf) return;
  strcpy(buf, payload);
  s_state.count = 0;
  char *p = buf;
  while (p && *p && s_state.count < MAX_STATIONS) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    Station st;
    memset(&st, 0, sizeof(st));
    if (parse_station_line(p, &st)) s_state.stations[s_state.count++] = st;
    p = nl ? nl + 1 : NULL;
  }
  free(buf);
  APP_LOG(APP_LOG_LEVEL_INFO, "Parsed %d stations", s_state.count);
}

/* ----------------------------------------------------------
   Send map request to phone
   Includes screen dimensions, selected station, B&W flag
---------------------------------------------------------- */
void send_map_request(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_int16(out, MESSAGE_KEY_MapRequest,  1);
  dict_write_int16(out, MESSAGE_KEY_MapScreenW,  s_state.screen_w);
  dict_write_int16(out, MESSAGE_KEY_MapScreenH,  s_state.screen_h);
  dict_write_int16(out, MESSAGE_KEY_MapSelected, s_state.selected_index);
#ifdef PBL_COLOR
  dict_write_int16(out, MESSAGE_KEY_MapBW, 0);
#else
  dict_write_int16(out, MESSAGE_KEY_MapBW, 1);
#endif
  app_message_outbox_send();
  APP_LOG(APP_LOG_LEVEL_INFO, "Map request sent (sel=%d, %dx%d, bw=%d)",
          s_state.selected_index, s_state.screen_w, s_state.screen_h,
#ifdef PBL_COLOR
          0);
#else
          1);
#endif
}

/* ----------------------------------------------------------
   Fuel type parser
---------------------------------------------------------- */
static uint8_t parse_fuel_type(const char *str) {
  if (!str)                      return FUEL_E10;
  if (strcmp(str, "E5")     == 0) return FUEL_E5;
  if (strcmp(str, "DIESEL") == 0) return FUEL_DIESEL;
  if (strcmp(str, "LPG")    == 0) return FUEL_LPG;
  return FUEL_E10;
}

/* ----------------------------------------------------------
   AppMessage callbacks
---------------------------------------------------------- */
static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *status_t    = dict_find(iter, MESSAGE_KEY_STATUS);
  Tuple *stations_t  = dict_find(iter, MESSAGE_KEY_STATION);
  Tuple *lat_t       = dict_find(iter, MESSAGE_KEY_OWN_LAT);
  Tuple *lon_t       = dict_find(iter, MESSAGE_KEY_OWN_LON);
  Tuple *fuel_type_t = dict_find(iter, MESSAGE_KEY_FuelType);
  Tuple *chunk_i_t   = dict_find(iter, MESSAGE_KEY_TileChunkI);
  Tuple *chunk_n_t   = dict_find(iter, MESSAGE_KEY_TileChunkN);
  Tuple *tile_data_t = dict_find(iter, MESSAGE_KEY_TileData);

  /* --- Tile chunk --- */
  if (tile_data_t) {
    int chunk_i = chunk_i_t ? (int)chunk_i_t->value->int32 : 0;
    int chunk_n = chunk_n_t ? (int)chunk_n_t->value->int32 : 0;

    /* chunk_n == 0 means tile fetch failed — hide loading */
    if (chunk_n == 0) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "Tile fetch failed on phone side");
      map_window_tile_failed();
      return;
    }

    uint8_t *data = tile_data_t->value->data;
    uint16_t dlen = tile_data_t->length;

    /* First chunk: allocate buffer */
    if (chunk_i == 0) {
      tile_buf_free();
      /* Exact size from header bytes if available */
      size_t exact = (size_t)chunk_n * IMG_CHUNK_BYTES;
      if (dlen >= 5) {
        int w = (data[0] << 8) | data[1];
        int h = (data[2] << 8) | data[3];
        int bw = data[4];
        size_t px_bytes = bw ? ((size_t)((w + 7) / 8) * h)
                             : ((size_t)w * h);
        size_t calc = 5 + px_bytes;
        if (calc > 0 && calc <= TILE_BUF_MAX) exact = calc;
      }
      s_tile_buf      = malloc(exact);
      s_tile_buf_size = s_tile_buf ? exact : 0;
      s_tile_chunks_n = chunk_n;
      s_tile_received = 0;
      if (!s_tile_buf) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Tile buf malloc failed (%d bytes)", (int)exact);
        return;
      }
    }

    if (s_tile_buf && s_tile_received + dlen <= s_tile_buf_size) {
      memcpy(s_tile_buf + s_tile_received, data, dlen);
      s_tile_received += dlen;
    }

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Tile chunk %d/%d (%d bytes)", chunk_i, chunk_n, dlen);

    /* Last chunk: hand buffer to map window */
    if (chunk_i + 1 >= chunk_n) {
      APP_LOG(APP_LOG_LEVEL_INFO, "Tile complete: %d bytes", (int)s_tile_received);
      map_window_tile_arrived(s_tile_buf, s_tile_received);
      /* map_window takes ownership — null our pointer, don't free */
      s_tile_buf      = NULL;
      s_tile_buf_size = 0;
      s_tile_received = 0;
    }
    return;
  }

  /* --- Fuel type update (from Clay settings) --- */
  if (fuel_type_t && fuel_type_t->type == TUPLE_CSTRING) {
    uint8_t new_fuel = parse_fuel_type(fuel_type_t->value->cstring);
    bool changed = (new_fuel != s_state.fuel_type);
    s_state.fuel_type = new_fuel;
    APP_LOG(APP_LOG_LEVEL_INFO, "Fuel type: %d", s_state.fuel_type);
    list_window_data_arrived();
    if (changed) {
      DictionaryIterator *out;
      if (app_message_outbox_begin(&out) == APP_MSG_OK) {
        dict_write_uint8(out, MESSAGE_KEY_STATUS, 0);
        app_message_outbox_send();
      }
    }
    return;
  }

  /* --- Station data --- */
  if (!status_t) return;
  s_state.status = (uint8_t)status_t->value->int32;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Status: %d", s_state.status);

  if (s_state.status == STATUS_OK) {
    if (lat_t) s_state.own_lat_e6 = lat_t->value->int32;
    if (lon_t) s_state.own_lon_e6 = lon_t->value->int32;
    if (stations_t && stations_t->type == TUPLE_CSTRING)
      parse_stations_payload(stations_t->value->cstring);
  }
  list_window_data_arrived();
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
}
static void outbox_failed(DictionaryIterator *iter,
                          AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", (int)reason);
}

/* ----------------------------------------------------------
   App lifecycle
---------------------------------------------------------- */
static void init(void) {
  memset(&s_state, 0, sizeof(s_state));
  s_state.status    = STATUS_ERROR;
  s_state.fuel_type = FUEL_E10;
#ifdef PBL_PLATFORM_EMERY
  s_state.screen_w  = 200;
  s_state.screen_h  = 228;
#else
  s_state.screen_w  = 144;
  s_state.screen_h  = 168;
#endif

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);

  list_window_push();
}

static void deinit(void) {
  tile_buf_free();
  list_window_destroy();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}