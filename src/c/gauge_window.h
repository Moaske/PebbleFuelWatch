#pragma once
#include <pebble.h>

/* =============================================================
   gauge_window.h – manual fuel level gauge

   Reached by long-pressing SELECT on the map / detail view.
   UP / DOWN move the needle, SELECT stores the position on the
   watch so it survives leaving the app.
   ============================================================= */

void gauge_window_push(void);