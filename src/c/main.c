/* =============================================================
   main.c – FuelWatch
   Handles app lifecycle, AppMessage unpacking, window stack.
   ============================================================= */

#include <pebble.h>
#include "station.h"
#include "list_window.h"

/* ----------------------------------------------------------
   Global app state (single instance)
---------------------------------------------------------- */
static AppState s_state;

AppState *app_state_get(void) {
  return &s_state;
}

/* ----------------------------------------------------------
   AppMessage inbox size
   Worst case: 10 stations x ~75 bytes per packed line + dict overhead
---------------------------------------------------------- */
#define INBOX_SIZE  2048
#define OUTBOX_SIZE   64

/* ----------------------------------------------------------
   Parse one packed station line into a Station struct.
   Format: id|name|address|dist_m|fuel_mills|lat_e6|lon_e6
   Prices in mills (x1000): 1979 = 1.979 euro
   Uses strchr field splitting — safe inside a strchr loop.
---------------------------------------------------------- */
static bool parse_station_line(char *line, Station *out) {
  char *p   = line;
  char *sep;
  int field = 0;

  while (field < 7) {
    sep = strchr(p, '|');
    if (sep) *sep = '\0';

    switch (field) {
      case 0: /* id — string, not used on watch side, skip */ break;
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

  return (field == 7);
}

/* ----------------------------------------------------------
   Parse the full packed stations string (newline-separated)
   into s_state.stations[].
---------------------------------------------------------- */
static void parse_stations_payload(const char *payload) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Payload length: %d", (int)strlen(payload));

  char *buf = malloc(strlen(payload) + 1);
  if (!buf) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "malloc failed for payload");
    return;
  }
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

  Tuple *status_t    = dict_find(iter, KEY_STATUS);
  Tuple *stations_t  = dict_find(iter, KEY_STATIONS);
  Tuple *lat_t       = dict_find(iter, KEY_OWN_LAT);
  Tuple *lon_t       = dict_find(iter, KEY_OWN_LON);
  Tuple *fuel_type_t = dict_find(iter, KEY_FUEL_TYPE);

  /* Clay sends fuel type independently on settings save */
   if (fuel_type_t && fuel_type_t->type == TUPLE_CSTRING) {
    s_state.fuel_type = parse_fuel_type(fuel_type_t->value->cstring);
    APP_LOG(APP_LOG_LEVEL_INFO, "Fuel type set to: %d", s_state.fuel_type);
    list_window_data_arrived();
    /* Request a fresh data fetch with the new fuel type */
    DictionaryIterator *out;
    if (app_message_outbox_begin(&out) == APP_MSG_OK) {
      dict_write_uint8(out, KEY_STATUS, 0);
      app_message_outbox_send();
    }
    return;
  }

  if (!status_t) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "No status key in message");
    return;
  }

  s_state.status = (uint8_t)status_t->value->int32;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Status: %d", s_state.status);

  if (s_state.status == STATUS_OK) {
    if (lat_t) s_state.own_lat_e6 = lat_t->value->int32;
    if (lon_t) s_state.own_lon_e6 = lon_t->value->int32;
    if (stations_t && stations_t->type == TUPLE_CSTRING) {
      parse_stations_payload(stations_t->value->cstring);
    } else {
      APP_LOG(APP_LOG_LEVEL_WARNING, "No stations tuple or wrong type");
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
  s_state.fuel_type = FUEL_E10;  // default until Clay sends a value

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