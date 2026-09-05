/* =============================================================
   FuelWatch – PebbleKit JS layer
   Fetches nearby fuel station prices from ANWB Onderweg API
   Single bounding box call returns stations + prices for NL + BE
   API: https://api.anwb.nl/routing/points-of-interest/v3/all
   ============================================================= */

/* ----------------------------------------------------------
   Clay – handles showConfiguration / webviewclosed
---------------------------------------------------------- */
var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

/* ----------------------------------------------------------
   Constants
---------------------------------------------------------- */
var API_BASE     = 'https://api.anwb.nl/routing/points-of-interest/v3/all';
var API_TYPE     = 'FUEL_STATION';
var BBOX_RADIUS  = 0.12;          // degrees (~13km) around position
var CACHE_TTL_MS = 15 * 60 * 1000; // 15 minutes — single call so cheap
var MAX_STATIONS = 10;
var DRIFT_KM     = 2.0;           // re-fetch if moved more than 2km

/* ----------------------------------------------------------
   Fuel type mapping: Clay setting value -> ANWB fuelType string
---------------------------------------------------------- */
var FUEL_TYPE_MAP = {
  'E10':    'EURO95',
  'E5':     'EURO98',
  'DIESEL': 'DIESEL',
  'LPG':    'AUTOGAS'
};

/* ----------------------------------------------------------
   Distance between two lat/lon points (Haversine, returns km)
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

/* ----------------------------------------------------------
   localStorage cache helpers
---------------------------------------------------------- */
function cacheGet(key) {
  try { return JSON.parse(localStorage.getItem(key)); } catch(e) { return null; }
}
function cacheSet(key, val) {
  try { localStorage.setItem(key, JSON.stringify(val)); } catch(e) {}
}

/* ----------------------------------------------------------
   Fuel type setting from Clay localStorage
---------------------------------------------------------- */
function getFuelTypeSetting() {
  try {
    var raw = localStorage.getItem('clay-settings');
    if (raw) {
      var settings = JSON.parse(raw);
      if (settings && settings.FuelType && typeof settings.FuelType === 'string') {
        return settings.FuelType;
      }
    }
  } catch(e) {
    console.log('[FuelWatch] Could not read fuel type setting: ' + e.message);
  }
  return 'E10';
}

/* ----------------------------------------------------------
   XHR-based fetch wrapper (PebbleKit JS has no fetch API)
---------------------------------------------------------- */
function fetchWithTimeout(url, options, timeoutMs) {
  timeoutMs = timeoutMs || 15000;
  return new Promise(function(resolve, reject) {
    var xhr    = new XMLHttpRequest();
    var method = (options && options.method) ? options.method : 'GET';
    xhr.open(method, url, true);

    if (options && options.headers) {
      Object.keys(options.headers).forEach(function(key) {
        xhr.setRequestHeader(key, options.headers[key]);
      });
    }

    var timer = setTimeout(function() {
      xhr.abort();
      reject(new Error('Request timed out'));
    }, timeoutMs);

    xhr.onload = function() {
      clearTimeout(timer);
      var responseText = xhr.responseText;
      resolve({
        ok:     xhr.status >= 200 && xhr.status < 300,
        status: xhr.status,
        json:   function() {
          return new Promise(function(res, rej) {
            try { res(JSON.parse(responseText)); }
            catch(e) { rej(new Error('JSON parse error')); }
          });
        }
      });
    };

    xhr.onerror = function() {
      clearTimeout(timer);
      reject(new Error('Network error'));
    };

    xhr.send(null);
  });
}

/* ----------------------------------------------------------
   Build ANWB bounding box URL from position
---------------------------------------------------------- */
function buildUrl(lat, lon) {
  var minLat = (lat - BBOX_RADIUS).toFixed(6);
  var minLon = (lon - BBOX_RADIUS).toFixed(6);
  var maxLat = (lat + BBOX_RADIUS).toFixed(6);
  var maxLon = (lon + BBOX_RADIUS).toFixed(6);
  return API_BASE +
    '?type-filter=' + API_TYPE +
    '&bounding-box-filter=' + minLat + '%2C' + minLon +
    '%2C' + maxLat + '%2C' + maxLon;
}

/* ----------------------------------------------------------
   Fetch stations from ANWB — single call, returns everything
---------------------------------------------------------- */
function fetchStations(lat, lon) {
  var url = buildUrl(lat, lon);
  console.log('[FuelWatch] Fetching from ANWB: ' + url);

  return fetchWithTimeout(url, { method: 'GET' }).then(function(r) {
    if (!r.ok) throw new Error('ANWB fetch failed: HTTP ' + r.status);
    return r.json();
  }).then(function(data) {
    if (!data || !Array.isArray(data.value)) {
      throw new Error('Unexpected ANWB response format');
    }
    console.log('[FuelWatch] ANWB returned ' + data.value.length + ' stations');
    return data.value;
  });
}

/* ----------------------------------------------------------
   Parse a single ANWB station into our internal format.
   Returns null if the station has no usable data.
   Belgian prices have floating point noise — we round to 3dp.
---------------------------------------------------------- */
function parseStation(raw, lat, lon, anwbFuelType) {
  var coords = raw.coordinates;
  if (!coords || !coords.latitude || !coords.longitude) return null;

  var stLat = coords.latitude;
  var stLon = coords.longitude;

  // Find price for selected fuel type
  var price = null;
  var prices = raw.prices || [];
  for (var i = 0; i < prices.length; i++) {
    if (prices[i].fuelType === anwbFuelType && prices[i].value != null) {
      price = Math.round(prices[i].value * 1000) / 1000;  // round to 3dp
      break;
    }
  }

  // Build address string
  var addr = raw.address || {};
  var address = (addr.streetAddress || '') +
    (addr.city ? ', ' + addr.city : '');

  return {
    id:      raw.id,
    lat:     stLat,
    lon:     stLon,
    name:    raw.title || 'Unknown',
    address: address,
    dist:    haversine(lat, lon, stLat, stLon),
    price:   price
  };
}

/* ----------------------------------------------------------
   AppMessage packing
   Format: id|name|address|dist_m|fuel_mills|lat_e6|lon_e6
   7 fields, stations separated by newline
---------------------------------------------------------- */
var KEY_STATUS   = 0;
var KEY_STATIONS = 1;
var KEY_OWN_LAT  = 2;
var KEY_OWN_LON  = 3;

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
    var packed = result.stations.map(packStation).join('\n');
    msg[KEY_STATUS]   = STATUS_OK;
    msg[KEY_STATIONS] = packed;
    msg[KEY_OWN_LAT]  = Math.round(result.lat * 1e6);
    msg[KEY_OWN_LON]  = Math.round(result.lon * 1e6);
  } else {
    msg[KEY_STATUS] = STATUS_ERROR;
  }
  Pebble.sendAppMessage(msg, function() {
    console.log('[FuelWatch] AppMessage sent OK');
  }, function(e) {
    console.error('[FuelWatch] AppMessage send failed: ' + e.error.message);
  });
}

/* ----------------------------------------------------------
   Core refresh
---------------------------------------------------------- */
function refresh(lat, lon) {
  var fuelType     = getFuelTypeSetting();
  var anwbFuelType = FUEL_TYPE_MAP[fuelType] || 'EURO95';

  console.log('[FuelWatch] Refreshing for ' + lat + ',' + lon +
              ' fuel: ' + fuelType + ' (' + anwbFuelType + ')');

  // Check cache — keyed by rounded position + fuel type
  var cacheKey    = 'fw_anwb_' + fuelType + '_' +
                    Math.round(lat * 100) + '_' + Math.round(lon * 100);
  var cached      = cacheGet(cacheKey);
  var now         = Date.now();

  if (cached && cached.ts && (now - cached.ts) < CACHE_TTL_MS) {
    console.log('[FuelWatch] Using cached ANWB data (' +
      Math.round((now - cached.ts) / 60000) + ' min old)');
    sendToWatch({ status: 'ok', stations: cached.stations, lat: lat, lon: lon });
    return;
  }

  fetchStations(lat, lon).then(function(rawStations) {
    // Parse, filter to those with the selected fuel price, sort by distance
    var stations = rawStations
      .map(function(raw) { return parseStation(raw, lat, lon, anwbFuelType); })
      .filter(function(s) { return s !== null && s.price !== null; })
      .sort(function(a, b) { return a.dist - b.dist; })
      .slice(0, MAX_STATIONS);

    console.log('[FuelWatch] Stations with ' + fuelType + ' price: ' + stations.length);

    if (stations.length === 0) {
      sendToWatch({ status: 'error', code: 'no_stations' });
      return;
    }

    // Cache result
    cacheSet(cacheKey, { stations: stations, ts: now });

    // Store position for drift detection
    cacheSet('fw_last_lat', lat);
    cacheSet('fw_last_lon', lon);

    sendToWatch({ status: 'ok', stations: stations, lat: lat, lon: lon });

  }).catch(function(err) {
    console.error('[FuelWatch] Refresh failed: ' + err.message);
    sendToWatch({ status: 'error', code: 'fetch_failed' });
  });
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

      // Drift check — if moved significantly, invalidate cache
      var lastLat = cacheGet('fw_last_lat');
      var lastLon = cacheGet('fw_last_lon');
      if (lastLat !== null && lastLon !== null) {
        var drift = haversine(lat, lon, lastLat, lastLon);
        console.log('[FuelWatch] Position drift: ' + drift.toFixed(2) + 'km');
        if (drift > DRIFT_KM) {
          console.log('[FuelWatch] Drift exceeds threshold, cache invalidated');
          cacheSet('fw_last_lat', null);
          cacheSet('fw_last_lon', null);
          // Clear all ANWB caches so we fetch fresh for new position
          try {
            var keys = Object.keys(localStorage);
            keys.forEach(function(k) {
              if (k.indexOf('fw_anwb_') === 0) localStorage.removeItem(k);
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
  getLocation();
});

Pebble.addEventListener('appmessage', function() {
  console.log('[FuelWatch] Refresh requested by watch');
  getLocation();
});