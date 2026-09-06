#pragma once

/* =============================================================
   map_window.h – map view showing nearby stations + roads
   ============================================================= */

/* Push the map window — also triggers road data request to phone */
void map_window_push(void);

/* Called by main.c when road segments have arrived from phone */
void map_window_roads_arrived(void);