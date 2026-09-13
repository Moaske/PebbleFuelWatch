#pragma once

/* =============================================================
   list_window.h – scrollable station list window for FuelWatch
   ============================================================= */

/* Push the list window onto the window stack */
void list_window_push(void);

/* Called by main.c when new data (or an error) has arrived */
void list_window_data_arrived(void);

/* Called from main.c deinit */
void list_window_destroy(void);

/* Move the MenuLayer highlight to AppState.selected_index.
   Called by map_window when the user pages through stations. */
void list_window_sync_selection(void);