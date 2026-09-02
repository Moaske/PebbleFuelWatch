#pragma once

/* =============================================================
   map_window.h – simple dot-map of nearby stations
   ============================================================= */

/* Push the map window onto the window stack.
   Reads selected_index from AppState set by list_window. */
void map_window_push(void);