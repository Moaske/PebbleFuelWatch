#pragma once
#include <pebble.h>

/* =============================================================
   station.h – shared data structures for FuelWatch
   ============================================================= */

#define MAX_STATIONS     10
#define STATION_NAME_LEN 21   // 20 chars + null
#define STATION_ADDR_LEN 25   // 24 chars + null

/* AppMessage keys – must match index.js and package.json messageKeys.
   Keys 0-3 are our own; KEY_FUEL_TYPE matches the Clay messageKey. */
#define KEY_STATUS     0
#define KEY_STATIONS   1
#define KEY_OWN_LAT    2
#define KEY_OWN_LON    3
#define KEY_FUEL_TYPE  MESSAGE_KEY_FuelType  // generated from package.json

/* Status codes – must match index.js */
#define STATUS_OK       0
#define STATUS_ERROR    1
#define STATUS_BLOCKED  2
#define STATUS_LOCERR   3

/* Fuel type codes – must match config.json option values */
#define FUEL_E10    0
#define FUEL_E5     1
#define FUEL_DIESEL 2
#define FUEL_LPG    3

/* Human-readable label for each fuel type (shown in list header) */
static inline const char *fuel_type_label(uint8_t fuel_type) {
  switch (fuel_type) {
    case FUEL_E5:     return "E5";
    case FUEL_DIESEL: return "Diesel";
    case FUEL_LPG:    return "LPG";
    default:          return "E10";
  }
}

/* Prices stored as millicents x1000: 1979 = 1.979 euro
   0 means price not available. */
typedef struct {
  char     name[STATION_NAME_LEN];
  char     address[STATION_ADDR_LEN];
  uint32_t dist_m;        // distance in metres
  uint16_t fuel_mills;    // price x1000 for selected fuel type
  int32_t  lat_e6;        // latitude  x 1e6
  int32_t  lon_e6;        // longitude x 1e6
} Station;

/* Shared app state – owned by main.c, read by both windows */
typedef struct {
  Station  stations[MAX_STATIONS];
  uint8_t  count;
  int32_t  own_lat_e6;
  int32_t  own_lon_e6;
  uint8_t  status;
  uint8_t  selected_index;
  uint8_t  fuel_type;     // FUEL_E10 / FUEL_E5 / FUEL_DIESEL / FUEL_LPG
} AppState;

/* Returns a pointer to the single global AppState instance */
AppState *app_state_get(void);