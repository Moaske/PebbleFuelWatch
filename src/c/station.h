#pragma once
#include <pebble.h>

/* =============================================================
   station.h – shared data structures for FuelWatch
   ============================================================= */

#define MAX_STATIONS     10
#define STATION_NAME_LEN 21   // 20 chars + null
#define STATION_ADDR_LEN 25   // 24 chars + null
#define MAX_ROAD_SEGS   80    // max road segments in map message

/* ----------------------------------------------------------
   AppMessage keys — all generated from package.json messageKeys.
   Available as MESSAGE_KEY_* in C, messageKeys.* in JS:
     MESSAGE_KEY_STATUS
     MESSAGE_KEY_STATION
     MESSAGE_KEY_OWN_LAT
     MESSAGE_KEY_OWN_LON
     MESSAGE_KEY_FuelType
     MESSAGE_KEY_MapRequest
     MESSAGE_KEY_MapScreenW
     MESSAGE_KEY_MapScreenH
     MESSAGE_KEY_MapSelected
     MESSAGE_KEY_Roads
---------------------------------------------------------- */

/* Status codes */
#define STATUS_OK       0
#define STATUS_ERROR    1
#define STATUS_BLOCKED  2
#define STATUS_LOCERR   3

/* Fuel type codes */
#define FUEL_E10    0
#define FUEL_E5     1
#define FUEL_DIESEL 2
#define FUEL_LPG    3

/* Road type codes — must match JS packing */
#define ROAD_MAJOR  0   // motorway, trunk, primary
#define ROAD_MEDIUM 1   // secondary, tertiary
#define ROAD_MINOR  2   // residential, unclassified

static inline const char *fuel_type_label(uint8_t fuel_type) {
  switch (fuel_type) {
    case FUEL_E5:     return "E5";
    case FUEL_DIESEL: return "Diesel";
    case FUEL_LPG:    return "LPG";
    default:          return "E10";
  }
}

/* ----------------------------------------------------------
   Station struct
   Prices in millicents x1000: 1979 = €1.979, 0 = unavailable
---------------------------------------------------------- */
typedef struct {
  char     name[STATION_NAME_LEN];
  char     address[STATION_ADDR_LEN];
  uint32_t dist_m;        // distance in metres
  uint16_t fuel_mills;    // price x1000
  int32_t  lat_e6;        // latitude  x 1e6
  int32_t  lon_e6;        // longitude x 1e6
} Station;

/* ----------------------------------------------------------
   Road segment — pre-projected to screen pixels by phone
---------------------------------------------------------- */
typedef struct {
  int16_t x1, y1, x2, y2;
  uint8_t road_type;      // ROAD_MAJOR / ROAD_MEDIUM / ROAD_MINOR
} RoadSegment;

/* ----------------------------------------------------------
   Shared app state
---------------------------------------------------------- */
typedef struct {
  Station     stations[MAX_STATIONS];
  uint8_t     count;
  int32_t     own_lat_e6;
  int32_t     own_lon_e6;
  uint8_t     status;
  uint8_t     selected_index;
  uint8_t     fuel_type;
  RoadSegment roads[MAX_ROAD_SEGS];
  uint8_t     road_count;
  int16_t     screen_w;   // set in init, sent with map request
  int16_t     screen_h;
} AppState;

AppState *app_state_get(void);