/* =============================================================
   FuelWatch – PebbleKit JS layer
   Station data: ANWB Onderweg API (single bbox call, NL+BE)
   Map backdrop: OSM raster tile, dithered + chunked to watch
   ============================================================= */

/* ----------------------------------------------------------
   Clay + message keys
---------------------------------------------------------- */
var Clay        = require('@rebble/clay');
var clayConfig  = require('./config');
var messageKeys = require('message_keys');
var clay        = new Clay(clayConfig);

/* ----------------------------------------------------------
   Constants
---------------------------------------------------------- */
var API_ANWB   = 'https://api.anwb.nl/routing/points-of-interest/v3/all';
var API_TYPE   = 'FUEL_STATION';
var OSM_TILE = 'https://tile.openstreetmap.org/{z}/{x}/{y}.png';

var BBOX_RADIUS    = 0.12;            // degrees for station list
var CACHE_TTL_MS   = 15 * 60 * 1000; // 15 min station cache
var TILE_CACHE_TTL = 60 * 60 * 1000; // 1 hour tile cache (increase after tuning)
var MAX_STATIONS   = 10;
var DRIFT_KM       = 2.0;
var TILE_ZOOM_MAX  = 15;              // OSM zoom level (street detail)
var TILE_ZOOM_MIN  = 13;              // OSM zoom level (wider area)
var IMG_CHUNK_BYTES = 2048;           // bytes per AppMessage chunk

/* ----------------------------------------------------------
   Fuel type mapping
---------------------------------------------------------- */
var FUEL_TYPE_MAP = {
  'E10':    'EURO95',
  'E5':     'EURO98',
  'DIESEL': 'DIESEL',
  'LPG':    'AUTOGAS'
};

/* ----------------------------------------------------------
   Helpers
---------------------------------------------------------- */
function haversine(lat1, lon1, lat2, lon2) {
  var R    = 6371;
  var dLat = (lat2 - lat1) * Math.PI / 180;
  var dLon = (lon2 - lon1) * Math.PI / 180;
  var a    = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
             Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
             Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function cacheGet(key) {
  try { return JSON.parse(localStorage.getItem(key)); } catch(e) { return null; }
}
function cacheSet(key, val) {
  try { localStorage.setItem(key, JSON.stringify(val)); } catch(e) {}
}

function getFuelTypeSetting() {
  try {
    var raw = localStorage.getItem('clay-settings');
    if (raw) {
      var s = JSON.parse(raw);
      if (s && s.FuelType && typeof s.FuelType === 'string') return s.FuelType;
    }
  } catch(e) {}
  return 'E10';
}

/* ----------------------------------------------------------
   XHR wrapper
---------------------------------------------------------- */
function fetchWithTimeout(url, options, timeoutMs) {
  timeoutMs = timeoutMs || 15000;
  return new Promise(function(resolve, reject) {
    var xhr    = new XMLHttpRequest();
    var method = (options && options.method) ? options.method : 'GET';
    xhr.open(method, url, true);

    // Request binary response when needed
    if (options && options.binary) {
      xhr.responseType = 'arraybuffer';
    }

    if (options && options.headers) {
      Object.keys(options.headers).forEach(function(k) {
        xhr.setRequestHeader(k, options.headers[k]);
      });
    }

    var timer = setTimeout(function() {
      xhr.abort();
      reject(new Error('Timeout'));
    }, timeoutMs);

    xhr.onload = function() {
      clearTimeout(timer);
      if (options && options.binary) {
        resolve({
          ok:     xhr.status >= 200 && xhr.status < 300,
          status: xhr.status,
          buffer: xhr.response
        });
      } else {
        var text = xhr.responseText;
        resolve({
          ok:     xhr.status >= 200 && xhr.status < 300,
          status: xhr.status,
          json:   function() {
            return new Promise(function(res, rej) {
              try { res(JSON.parse(text)); } catch(e) { rej(new Error('JSON error')); }
            });
          }
        });
      }
    };
    xhr.onerror = function() {
      clearTimeout(timer);
      reject(new Error('Network error'));
    };
    xhr.send(null);
  });
}

/* ----------------------------------------------------------
   ANWB station fetch
---------------------------------------------------------- */
function buildAnwbUrl(lat, lon) {
  var r = BBOX_RADIUS;
  return API_ANWB +
    '?type-filter=' + API_TYPE +
    '&bounding-box-filter=' +
    (lat - r).toFixed(6) + '%2C' + (lon - r).toFixed(6) + '%2C' +
    (lat + r).toFixed(6) + '%2C' + (lon + r).toFixed(6);
}

function fetchStations(lat, lon) {
  return fetchWithTimeout(buildAnwbUrl(lat, lon), { method: 'GET' })
    .then(function(r) {
      if (!r.ok) throw new Error('ANWB HTTP ' + r.status);
      return r.json();
    })
    .then(function(data) {
      if (!data || !Array.isArray(data.value)) throw new Error('Bad ANWB response');
      return data.value;
    });
}

function parseStation(raw, lat, lon, anwbFuelType) {
  var coords = raw.coordinates;
  if (!coords) return null;
  var price = null;
  var prices = raw.prices || [];
  for (var i = 0; i < prices.length; i++) {
    if (prices[i].fuelType === anwbFuelType && prices[i].value != null) {
      price = Math.round(prices[i].value * 1000) / 1000;
      break;
    }
  }
  var addr    = raw.address || {};
  var address = (addr.streetAddress || '') + (addr.city ? ', ' + addr.city : '');
  return {
    id:      raw.id,
    lat:     coords.latitude,
    lon:     coords.longitude,
    name:    raw.title || 'Unknown',
    address: address,
    dist:    haversine(lat, lon, coords.latitude, coords.longitude),
    price:   price
  };
}

/* ----------------------------------------------------------
   OSM tile coordinate math
   Converts lat/lon + zoom to tile x/y integers
---------------------------------------------------------- */
function latLonToTile(lat, lon, zoom) {
  var n = Math.pow(2, zoom);
  var x = Math.floor((lon + 180) / 360 * n);
  var latRad = lat * Math.PI / 180;
  var y = Math.floor((1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2 * n);
  return { x: x, y: y };
}

function tileUrl(z, x, y) {
  return OSM_TILE.replace('{z}', z).replace('{x}', x).replace('{y}', y);
}

/* Calculate the lat/lon bounds of an OSM tile */
function tileBounds(z, x, y) {
  var n    = Math.pow(2, z);
  var minLon = x / n * 360 - 180;
  var maxLon = (x + 1) / n * 360 - 180;
  var maxLat = Math.atan(Math.sinh(Math.PI * (1 - 2 * y / n))) * 180 / Math.PI;
  var minLat = Math.atan(Math.sinh(Math.PI * (1 - 2 * (y + 1) / n))) * 180 / Math.PI;
  return { minLat: minLat, maxLat: maxLat, minLon: minLon, maxLon: maxLon };
}

/* ----------------------------------------------------------
   Convert ArrayBuffer PNG to Pebble bitmap bytes.
   Colour platforms: GColor8 (1 byte/pixel, aarrggbb 2bpc)
   B&W platforms:   1-bit packed (1 byte/8 pixels, MSB first)
   Header: [w_hi, w_lo, h_hi, h_lo, bw_flag(0/1), pixels...]
   Uses canvas + Floyd-Steinberg dithering (from ScaleMates).
---------------------------------------------------------- */
function pngToPebbleBitmap(arrayBuffer, targetW, targetH, isBW, bounds) {
  return new Promise(function(resolve, reject) {
    var blob = new Blob([arrayBuffer], { type: 'image/png' });
    var url  = URL.createObjectURL(blob);
    var img  = new Image();

    img.onload = function() {
      URL.revokeObjectURL(url);

      var canvas  = document.createElement('canvas');
      canvas.width  = targetW;
      canvas.height = targetH;
      var ctx = canvas.getContext('2d');
      ctx.drawImage(img, 0, 0, targetW, targetH);

      var d = ctx.getImageData(0, 0, targetW, targetH).data;

      /* Floyd-Steinberg dithering on greyscale (for B&W)
         or per-channel (for colour) */
      if (isBW) {
        /* Convert to greyscale with error diffusion, output 1-bit packed */
        var grey = new Float32Array(targetW * targetH);
        for (var i = 0, p = 0; i < d.length; i += 4, p++) {
          grey[p] = 0.299 * d[i] + 0.587 * d[i+1] + 0.114 * d[i+2];
        }

        /* 1-bit: ceil(w/8) bytes per row */
        /* Header: w(2) h(2) bw(1) minLat(4) maxLat(4) minLon(4) maxLon(4) = 19 bytes */
        var minLatE6 = Math.round(bounds.minLat * 1e6);
        var maxLatE6 = Math.round(bounds.maxLat * 1e6);
        var minLonE6 = Math.round(bounds.minLon * 1e6);
        var maxLonE6 = Math.round(bounds.maxLon * 1e6);
        var stride  = Math.ceil(targetW / 8);
        var pixBytes = stride * targetH;
        var bytes   = new Uint8Array(21 + pixBytes);
        bytes[0] = (targetW >> 8) & 0xFF;
        bytes[1] =  targetW       & 0xFF;
        bytes[2] = (targetH >> 8) & 0xFF;
        bytes[3] =  targetH       & 0xFF;
        bytes[4] = 1; /* B&W flag */
        [minLatE6, maxLatE6, minLonE6, maxLonE6].forEach(function(v, i) {
          var base = 5 + i * 4;
          bytes[base]   = (v >> 24) & 0xFF;
          bytes[base+1] = (v >> 16) & 0xFF;
          bytes[base+2] = (v >>  8) & 0xFF;
          bytes[base+3] =  v        & 0xFF;
        });

        for (var y = 0; y < targetH; y++) {
          for (var x = 0; x < targetW; x++) {
            var idx   = y * targetW + x;
            var oldv  = grey[idx];
            var newv  = oldv > 127 ? 255 : 0;
            var qe    = oldv - newv;
            grey[idx] = newv;

            /* Distribute error */
            if (x + 1 < targetW)              grey[idx + 1]          += qe * 7 / 16;
            if (y + 1 < targetH) {
              if (x > 0)                       grey[idx + targetW - 1] += qe * 3 / 16;
                                               grey[idx + targetW]     += qe * 5 / 16;
              if (x + 1 < targetW)             grey[idx + targetW + 1] += qe * 1 / 16;
            }

            /* Pack into byte: white=1, black=0; MSB = leftmost pixel */
            if (newv > 127) {
              bytes[21 + y * stride + Math.floor(x / 8)] |= (1 << (x % 8));
            }
          }
        }

        console.log('[FuelWatch] Tile 1-bit ' + targetW + 'x' + targetH +
                    ' -> ' + bytes.length + ' bytes');
        resolve(bytes);

      } else {
        /* GColor8: Floyd-Steinberg per channel, 2bpc */
        var err = new Float32Array(targetW * targetH * 3);
        for (var i = 0, e = 0; i < d.length; i += 4, e += 3) {
          err[e]     = d[i];
          err[e + 1] = d[i + 1];
          err[e + 2] = d[i + 2];
        }

        function diffuse(idx, amount) {
          var v = err[idx] + amount;
          err[idx] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }

        /* Header: w(2) h(2) bw(1) minLat(4) maxLat(4) minLon(4) maxLon(4) = 19 bytes */
        var minLatE6 = Math.round(bounds.minLat * 1e6);
        var maxLatE6 = Math.round(bounds.maxLat * 1e6);
        var minLonE6 = Math.round(bounds.minLon * 1e6);
        var maxLonE6 = Math.round(bounds.maxLon * 1e6);
        var bytes = new Uint8Array(21 + targetW * targetH);
        bytes[0] = (targetW >> 8) & 0xFF;
        bytes[1] =  targetW       & 0xFF;
        bytes[2] = (targetH >> 8) & 0xFF;
        bytes[3] =  targetH       & 0xFF;
        bytes[4] = 0; /* colour flag */
        /* bounds packed as int32 big-endian */
        [minLatE6, maxLatE6, minLonE6, maxLonE6].forEach(function(v, i) {
          var base = 5 + i * 4;
          bytes[base]   = (v >> 24) & 0xFF;
          bytes[base+1] = (v >> 16) & 0xFF;
          bytes[base+2] = (v >>  8) & 0xFF;
          bytes[base+3] =  v        & 0xFF;
        });

        var o = 21;
        for (var y = 0; y < targetH; y++) {
          for (var x = 0; x < targetW; x++) {
            var base = (y * targetW + x) * 3;
            var lvl  = [0, 0, 0];
            for (var c = 0; c < 3; c++) {
              var oldv = err[base + c];
              var q    = (oldv / 85 + 0.5) | 0;
              if (q < 0) q = 0; else if (q > 3) q = 3;
              lvl[c]   = q;
              var qe   = oldv - q * 85;
              if (x + 1 < targetW)    diffuse(base + 3 + c,                  qe * 7 / 16);
              if (y + 1 < targetH) {
                var below = ((y + 1) * targetW + x) * 3 + c;
                if (x > 0)            diffuse(below - 3,                      qe * 3 / 16);
                                      diffuse(below,                          qe * 5 / 16);
                if (x + 1 < targetW)  diffuse(below + 3,                      qe * 1 / 16);
              }
            }
            bytes[o++] = 0xC0 | (lvl[0] << 4) | (lvl[1] << 2) | lvl[2];
          }
        }

        console.log('[FuelWatch] Tile GColor8 ' + targetW + 'x' + targetH +
                    ' -> ' + bytes.length + ' bytes');
        resolve(bytes);
      }
    };

    img.onerror = function() {
      URL.revokeObjectURL(url);
      reject(new Error('PNG decode failed'));
    };
    img.src = url;
  });
}

/* ----------------------------------------------------------
   Send tile chunks to watch (same pattern as ScaleMates)
   chunk 0 carries the header bytes
---------------------------------------------------------- */
function sendTileChunk(tileBytes, chunkIdx, totalChunks) {
  var start  = chunkIdx * IMG_CHUNK_BYTES;
  var end    = Math.min(start + IMG_CHUNK_BYTES, tileBytes.length);
  var chunk  = Array.from(tileBytes.subarray(start, end));

  var msg = {};
  msg[messageKeys.TileChunkI] = chunkIdx;
  msg[messageKeys.TileChunkN] = totalChunks;
  msg[messageKeys.TileData]   = chunk;

  Pebble.sendAppMessage(msg,
    function() {
      console.log('[FuelWatch] Tile chunk ' + chunkIdx + '/' + totalChunks + ' sent');
      if (chunkIdx + 1 < totalChunks) {
        sendTileChunk(tileBytes, chunkIdx + 1, totalChunks);
      } else {
        console.log('[FuelWatch] Tile transfer complete');
      }
    },
    function(e) {
      console.warn('[FuelWatch] Chunk ' + chunkIdx + ' failed, retrying');
      setTimeout(function() {
        sendTileChunk(tileBytes, chunkIdx, totalChunks);
      }, 500);
    }
  );
}

/* ----------------------------------------------------------
   Map tile fetch + process pipeline
---------------------------------------------------------- */
function fetchAndSendTile(lat, lon, screenW, screenH, isBW, zoom, sel) {
  zoom = zoom || TILE_ZOOM_MAX;
  var tile   = latLonToTile(lat, lon, zoom);
  var url    = tileUrl(zoom, tile.x, tile.y);

  /* Map area dimensions (mirrors C-side MAP_PAD_* constants) */
  var padTop  = 16;
  var padBot  = 2;
  var padSide = 6;
  var mapW    = screenW - padSide * 2;
  var mapH    = screenH - padTop - padBot;

  /* Cache key includes selected station so different stations
     always get their own tile even if they share the same tile coords */
  var cacheKey = 'fw_tile_' + zoom + '_' + tile.x + '_' + tile.y +
                 '_s' + (sel || 0) + '_' + (isBW ? '1' : '0');
  var cached   = cacheGet(cacheKey);
  var now      = Date.now();

  if (cached && cached.ts && (now - cached.ts) < TILE_CACHE_TTL && cached.bytes) {
    console.log('[FuelWatch] Using cached tile');
    var bytes       = new Uint8Array(cached.bytes);
    var totalChunks = Math.ceil(bytes.length / IMG_CHUNK_BYTES);
    sendTileChunk(bytes, 0, totalChunks);
    return;
  }

  var bounds = tileBounds(zoom, tile.x, tile.y);
  console.log('[FuelWatch] Fetching OSM tile: ' + url);
  fetchWithTimeout(url, { method: 'GET', binary: true }, 15000)
    .then(function(r) {
      if (!r.ok) throw new Error('Tile HTTP ' + r.status);
      return pngToPebbleBitmap(r.buffer, mapW, mapH, isBW, bounds);
    })
    .then(function(bytes) {
      /* Cache the processed bytes as regular array */
      cacheSet(cacheKey, { bytes: Array.from(bytes), ts: now });

      var totalChunks = Math.ceil(bytes.length / IMG_CHUNK_BYTES);
      console.log('[FuelWatch] Sending tile: ' + bytes.length +
                  ' bytes, ' + totalChunks + ' chunks');
      sendTileChunk(bytes, 0, totalChunks);
    })
    .catch(function(err) {
      console.error('[FuelWatch] Tile fetch failed: ' + err.message);
      /* Send empty chunk to signal failure — C side hides loading overlay */
      var msg = {};
      msg[messageKeys.TileChunkI] = 0;
      msg[messageKeys.TileChunkN] = 0;
      msg[messageKeys.TileData]   = [];
      Pebble.sendAppMessage(msg);
    });
}

/* ----------------------------------------------------------
   AppMessage packing — station list
---------------------------------------------------------- */
var STATUS_OK    = 0;
var STATUS_ERROR = 1;

function packStation(s) {
  var fuelMills = s.price ? Math.round(s.price * 1000) : 0;
  var distM     = Math.round(s.dist * 1000);
  var latE6     = Math.round(s.lat  * 1e6);
  var lonE6     = Math.round(s.lon  * 1e6);
  var name      = (s.name    || '').substring(0, 20).replace(/[|\n]/g, ' ');
  var address   = (s.address || '').substring(0, 24).replace(/[|\n]/g, ' ');
  return [s.id, name, address, distM, fuelMills, latE6, lonE6].join('|');
}

function sendToWatch(result) {
  var msg = {};
  if (result.status === 'ok') {
    msg[messageKeys.STATUS]  = STATUS_OK;
    msg[messageKeys.STATION] = result.stations.map(packStation).join('\n');
    msg[messageKeys.OWN_LAT] = Math.round(result.lat * 1e6);
    msg[messageKeys.OWN_LON] = Math.round(result.lon * 1e6);
  } else {
    msg[messageKeys.STATUS]  = STATUS_ERROR;
  }
  Pebble.sendAppMessage(msg,
    function() { console.log('[FuelWatch] AppMessage sent OK'); },
    function(e) { console.error('[FuelWatch] AppMessage failed: ' + e.error.message); });
}

/* ----------------------------------------------------------
   Core refresh — station list
---------------------------------------------------------- */
var s_lastStations = null;
var s_lastLat      = null;
var s_lastLon      = null;

function refresh(lat, lon) {
  var fuelType     = getFuelTypeSetting();
  var anwbFuelType = FUEL_TYPE_MAP[fuelType] || 'EURO95';
  console.log('[FuelWatch] Refreshing for ' + lat + ',' + lon + ' fuel: ' + fuelType);

  var cacheKey = 'fw_anwb_' + fuelType + '_' +
                 Math.round(lat * 100) + '_' + Math.round(lon * 100);
  var cached   = cacheGet(cacheKey);
  var now      = Date.now();

  if (cached && cached.ts && (now - cached.ts) < CACHE_TTL_MS) {
    console.log('[FuelWatch] Using cached ANWB data (' +
      Math.round((now - cached.ts) / 60000) + ' min old)');
    s_lastStations = cached.stations;
    s_lastLat = lat; s_lastLon = lon;
    sendToWatch({ status: 'ok', stations: cached.stations, lat: lat, lon: lon });
    return;
  }

  fetchStations(lat, lon).then(function(rawStations) {
    var stations = rawStations
      .map(function(raw) { return parseStation(raw, lat, lon, anwbFuelType); })
      .filter(function(s) { return s !== null && s.price !== null; })
      .sort(function(a, b) { return a.dist - b.dist; })
      .slice(0, MAX_STATIONS);

    console.log('[FuelWatch] Stations with ' + fuelType + ': ' + stations.length);

    if (stations.length === 0) {
      sendToWatch({ status: 'error', code: 'no_stations' });
      return;
    }

    cacheSet(cacheKey, { stations: stations, ts: now });
    s_lastStations = stations;
    s_lastLat = lat; s_lastLon = lon;
    cacheSet('fw_last_lat', lat);
    cacheSet('fw_last_lon', lon);
    sendToWatch({ status: 'ok', stations: stations, lat: lat, lon: lon });

  }).catch(function(err) {
    console.error('[FuelWatch] Refresh failed: ' + err.message);
    sendToWatch({ status: 'error', code: 'fetch_failed' });
  });
}

/* ----------------------------------------------------------
   Map request handler
---------------------------------------------------------- */

/* Pick zoom so both own position and station fit in one tile with padding.
   At zoom 15 tile ~1.2km, zoom 14 ~2.5km, zoom 13 ~5km */
function pickZoom(lat1, lon1, lat2, lon2) {
  var dist = haversine(lat1, lon1, lat2, lon2);
  if (dist < 0.55) return 15;
  if (dist < 1.2)  return 14;
  return 13;
}

function handleMapRequest(selectedIndex, screenW, screenH, isBW) {
  if (!s_lastStations || s_lastStations.length === 0) {
    console.warn('[FuelWatch] Map request but no station data');
    return;
  }
  var sel  = Math.min(selectedIndex, s_lastStations.length - 1);
  var stn  = s_lastStations[sel];
  var zoom = pickZoom(s_lastLat, s_lastLon, stn.lat, stn.lon);
  /* Bias midpoint 60% toward own position so tile covers more
     of the area between us and the station */
  var midLat = s_lastLat * 0.6 + stn.lat * 0.4;
  var midLon = s_lastLon * 0.6 + stn.lon * 0.4;
  var dist = haversine(s_lastLat, s_lastLon, stn.lat, stn.lon);
  console.log('[FuelWatch] Map: dist=' + dist.toFixed(2) + 'km zoom=' + zoom);
  /* Always use 1-bit B&W, tile centred on midpoint */
  fetchAndSendTile(midLat, midLon, screenW, screenH, true, zoom, sel);
}

/* ----------------------------------------------------------
   Geolocation
---------------------------------------------------------- */
function getLocation() {
  if (!navigator.geolocation) {
    sendToWatch({ status: 'error', code: 'no_geolocation' });
    return;
  }
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      var lat = pos.coords.latitude;
      var lon = pos.coords.longitude;
      console.log('[FuelWatch] Position: ' + lat + ',' + lon);

      var lastLat = cacheGet('fw_last_lat');
      var lastLon = cacheGet('fw_last_lon');
      if (lastLat !== null && lastLon !== null) {
        var drift = haversine(lat, lon, lastLat, lastLon);
        console.log('[FuelWatch] Drift: ' + drift.toFixed(2) + 'km');
        if (drift > DRIFT_KM) {
          cacheSet('fw_last_lat', null);
          cacheSet('fw_last_lon', null);
          try {
            var keys = Object.keys(localStorage);
            keys.forEach(function(k) {
              if (k.indexOf('fw_anwb_') === 0 || k.indexOf('fw_tile_') === 0)
                localStorage.removeItem(k);
            });
          } catch(e) {}
        }
      }
      refresh(lat, lon);
    },
    function(err) {
      console.error('[FuelWatch] Geolocation error: ' + err.message);
      sendToWatch({ status: 'error', code: 'location_failed' });
    },
    { timeout: 15000, maximumAge: 60000 }
  );
}

/* ----------------------------------------------------------
   PebbleKit JS event handlers
---------------------------------------------------------- */
Pebble.addEventListener('ready', function() {
  console.log('[FuelWatch] PebbleKit JS ready');
  // Clear tile cache on every startup to avoid stale cached data
  try {
    var keys = Object.keys(localStorage);
    keys.forEach(function(k) {
      if (k.indexOf('fw_tile_') === 0) localStorage.removeItem(k);
    });
  } catch(e) {}
  getLocation();
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  /* Inbound from watch: keys are STRING names.
     Outbound to watch: use numeric messageKeys.X values. */
  console.log('[FuelWatch] appmessage: ' + JSON.stringify(payload));

  if (typeof payload.MapRequest !== 'undefined') {
    var sel     = parseInt(payload.MapSelected, 10) || 0;
    var screenW = parseInt(payload.MapScreenW,  10) || 144;
    var screenH = parseInt(payload.MapScreenH,  10) || 168;
    var isBW    = payload.MapBW === 1 || payload.MapBW === '1';
    console.log('[FuelWatch] Map request: sel=' + sel +
                ' ' + screenW + 'x' + screenH + ' bw=' + isBW);
    handleMapRequest(sel, screenW, screenH, isBW);
    return;
  }

  console.log('[FuelWatch] Refresh requested');
  getLocation();
});