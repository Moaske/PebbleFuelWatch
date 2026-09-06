#pragma once
#include <pebble.h>

/* =============================================================
   map_window.h – map view with OSM tile backdrop + station dots
   ============================================================= */

/* Push the map window onto the stack */
void map_window_push(void);

/* Called by main.c when all tile chunks have been reassembled.
   Takes ownership of buf — map_window.c frees it. */
void map_window_tile_arrived(uint8_t *buf, size_t len);

/* Called when tile fetch failed on phone side */
void map_window_tile_failed(void);