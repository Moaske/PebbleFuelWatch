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