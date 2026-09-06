/* =============================================================
   main.c – FuelWatch
   Handles app lifecycle, AppMessage unpacking, window stack.
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

#define INBOX_SIZE  2560
#define OUTBOX_SIZE  256

/* ----------------------------------------------------------
   Parse one packed station line into a Station struct.
   Format: name|address|dist_m|fuel_mills|lat_e6|lon_e6
   (id field dropped — ANWB IDs are strings, unused on watch)
---------------------------------------------------------- */
static bool parse_station_line(char *line, Station *out) {
  char *p   = line;
  char *sep;
  int field = 0;

  while (field < 6) {
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

/* ----------------------------------------------------------
   Parse stations payload (newline-separated lines)
---------------------------------------------------------- */
static void parse_stations_payload(const char *payload) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Payload length: %d", (int)strlen(payload));

  char *buf = malloc(strlen(payload) + 1);
  if (!buf) { APP_LOG(APP_LOG_LEVEL_ERROR, "malloc failed"); return; }
  strcpy(buf, payload);

  s_state.count = 0;
  char *p = buf;
  while (p && *p && s_state.count < MAX_STATIONS) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    Station st;
    memset(&st, 0, sizeof(st));
    if (parse_station_line(p, &st)) {
      s_state.stations[s_state.count++] = st;
    }
    p = nl ? nl + 1 : NULL;
  }

  free(buf);
  APP_LOG(APP_LOG_LEVEL_INFO, "Parsed %d stations", s_state.count);
}

/* ----------------------------------------------------------
   Parse road segments payload
   Format per line: x1|y1|x2|y2|type  (all integers)
---------------------------------------------------------- */
static void parse_roads_payload(const char *payload) {
  char *buf = malloc(strlen(payload) + 1);
  if (!buf) return;
  strcpy(buf, payload);

  s_state.road_count = 0;
  char *p = buf;

  while (p && *p && s_state.road_count < MAX_ROAD_SEGS) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';

    // Parse: x1|y1|x2|y2|type
    RoadSegment seg;
    char *tok = p;
    char *sep;
    int field = 0;
    bool ok = true;

    while (field < 5 && ok) {
      sep = strchr(tok, '|');
      if (sep) *sep = '\0';
      switch (field) {
        case 0: seg.x1        = (int16_t)atoi(tok); break;
        case 1: seg.y1        = (int16_t)atoi(tok); break;
        case 2: seg.x2        = (int16_t)atoi(tok); break;
        case 3: seg.y2        = (int16_t)atoi(tok); break;
        case 4: seg.road_type = (uint8_t) atoi(tok); break;
      }
      field++;
      if (!sep) { if (field < 5) ok = false; break; }
      tok = sep + 1;
    }

    if (ok && field >= 5) {
      s_state.roads[s_state.road_count++] = seg;
    }

    p = nl ? nl + 1 : NULL;
  }

  free(buf);
  APP_LOG(APP_LOG_LEVEL_INFO, "Parsed %d road segments", s_state.road_count);
}

/* ----------------------------------------------------------
   Send map request to phone
   Sends screen dimensions, selected station index, own position
---------------------------------------------------------- */
void send_map_request(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;

  dict_write_int16(out, MESSAGE_KEY_MapRequest,  1);
  dict_write_int16(out, MESSAGE_KEY_MapScreenW,  s_state.screen_w);
  dict_write_int16(out, MESSAGE_KEY_MapScreenH,  s_state.screen_h);
  dict_write_int16(out, MESSAGE_KEY_MapSelected, s_state.selected_index);

  app_message_outbox_send();
  APP_LOG(APP_LOG_LEVEL_INFO, "Map request sent (sel=%d, %dx%d)",
          s_state.selected_index, s_state.screen_w, s_state.screen_h);
}

/* ----------------------------------------------------------
   Map fuel type string from Clay to FUEL_* constant
---------------------------------------------------------- */
static uint8_t parse_fuel_type(const char *str) {
  if (!str) return FUEL_E10;
  if (strcmp(str, "E5")     == 0) return FUEL_E5;
  if (strcmp(str, "DIESEL") == 0) return FUEL_DIESEL;
  if (strcmp(str, "LPG")    == 0) return FUEL_LPG;
  return FUEL_E10;
}

/* ----------------------------------------------------------
   AppMessage callbacks
---------------------------------------------------------- */
static void inbox_received(DictionaryIterator *iter, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Inbox received");

  Tuple *status_t    = dict_find(iter, MESSAGE_KEY_STATUS);
  Tuple *stations_t  = dict_find(iter, MESSAGE_KEY_STATION);
  Tuple *lat_t       = dict_find(iter, MESSAGE_KEY_OWN_LAT);
  Tuple *lon_t       = dict_find(iter, MESSAGE_KEY_OWN_LON);
  Tuple *fuel_type_t = dict_find(iter, MESSAGE_KEY_FuelType);
  Tuple *roads_t     = dict_find(iter, MESSAGE_KEY_Roads);

  /* Clay fuel type update */
  if (fuel_type_t && fuel_type_t->type == TUPLE_CSTRING) {
    uint8_t new_fuel_type = parse_fuel_type(fuel_type_t->value->cstring);
    APP_LOG(APP_LOG_LEVEL_INFO, "Fuel type: %d", new_fuel_type);
    
    bool changed = (new_fuel_type != s_state.fuel_type);
    s_state.fuel_type = new_fuel_type;
    list_window_data_arrived();
    
    /* Only request fresh data if fuel type actually changed */
    if (changed) {
      DictionaryIterator *out;
      if (app_message_outbox_begin(&out) == APP_MSG_OK) {
        dict_write_uint8(out, MESSAGE_KEY_STATUS, 0);
        app_message_outbox_send();
      }
    }
    return;
  }

  /* Road segments for map view */
  if (roads_t && roads_t->type == TUPLE_CSTRING) {
    parse_roads_payload(roads_t->value->cstring);
    map_window_roads_arrived();
    return;
  }

  if (!status_t) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "No status key");
    return;
  }

  s_state.status = (uint8_t)status_t->value->int32;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Status: %d", s_state.status);

  if (s_state.status == STATUS_OK) {
    if (lat_t) s_state.own_lat_e6 = lat_t->value->int32;
    if (lon_t) s_state.own_lon_e6 = lon_t->value->int32;
    if (stations_t && stations_t->type == TUPLE_CSTRING) {
      parse_stations_payload(stations_t->value->cstring);
    }
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
  s_state.screen_w = 200;
  s_state.screen_h = 228;
#else
  s_state.screen_w = 144;
  s_state.screen_h = 168;
#endif

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);

  list_window_push();
}

static void deinit(void) {
  list_window_destroy();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}