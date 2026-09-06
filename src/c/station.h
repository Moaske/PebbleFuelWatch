#pragma once
#include <pebble.h>

/* =============================================================
   station.h – shared data structures for FuelWatch
   ============================================================= */

#define MAX_STATIONS      10
#define STATION_NAME_LEN  21   // 20 chars + null
#define STATION_ADDR_LEN  25   // 24 chars + null

/* Tile receive buffer — max size:
   Colour: mapW * mapH bytes (GColor8)  e.g. 188*210 = 39480
   B&W:    ceil(mapW/8) * mapH bytes    e.g. 24*152  = 3648
   Add 5 bytes header. Use generous upper bound. */
#define TILE_BUF_MAX  42000

/* Chunk protocol */
#define IMG_CHUNK_BYTES  2048

/* AppMessage keys generated from package.json messageKeys:
   MESSAGE_KEY_STATUS, MESSAGE_KEY_STATION,
   MESSAGE_KEY_OWN_LAT, MESSAGE_KEY_OWN_LON,
   MESSAGE_KEY_FuelType,
   MESSAGE_KEY_MapRequest, MESSAGE_KEY_MapScreenW,
   MESSAGE_KEY_MapScreenH, MESSAGE_KEY_MapSelected,
   MESSAGE_KEY_Roads,
   MESSAGE_KEY_TileChunkI, MESSAGE_KEY_TileChunkN,
   MESSAGE_KEY_TileData, MESSAGE_KEY_MapBW
*/

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
---------------------------------------------------------- */
typedef struct {
  char     name[STATION_NAME_LEN];
  char     address[STATION_ADDR_LEN];
  uint32_t dist_m;
  uint16_t fuel_mills;
  int32_t  lat_e6;
  int32_t  lon_e6;
} Station;

/* ----------------------------------------------------------
   Shared app state
---------------------------------------------------------- */
typedef struct {
  Station  stations[MAX_STATIONS];
  uint8_t  count;
  int32_t  own_lat_e6;
  int32_t  own_lon_e6;
  uint8_t  status;
  uint8_t  selected_index;
  uint8_t  fuel_type;
  int16_t  screen_w;
  int16_t  screen_h;
} AppState;

AppState *app_state_get(void);