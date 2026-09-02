#pragma once
#include <pebble.h>

/* =============================================================
   station.h – shared data structures for FuelWatch
   ============================================================= */

#define MAX_STATIONS     10
#define STATION_NAME_LEN 21   // 20 chars + null
#define STATION_ADDR_LEN 25   // 24 chars + null

/* AppMessage keys – must match index.js */
#define KEY_STATUS    0
#define KEY_STATIONS  1
#define KEY_OWN_LAT   2
#define KEY_OWN_LON   3

/* Status codes – must match index.js */
#define STATUS_OK       0
#define STATUS_ERROR    1
#define STATUS_BLOCKED  2
#define STATUS_LOCERR   3

/* Prices stored as millicents ×1000: 1979 = €1.979
   0 means price not available. */
typedef struct {
  uint32_t id;
  char     name[STATION_NAME_LEN];
  char     address[STATION_ADDR_LEN];
  uint32_t dist_m;        // distance in metres
  uint16_t e10_mills;     // price ×1000, e.g. 1979 = €1.979
  uint16_t diesel_mills;  // same
  int32_t  lat_e6;        // latitude  × 1e6
  int32_t  lon_e6;        // longitude × 1e6
} Station;

/* Shared app state – owned by main.c, read by both windows */
typedef struct {
  Station  stations[MAX_STATIONS];
  uint8_t  count;
  int32_t  own_lat_e6;
  int32_t  own_lon_e6;
  uint8_t  status;
  uint8_t  selected_index;
} AppState;

/* Returns a pointer to the single global AppState instance */
AppState *app_state_get(void);