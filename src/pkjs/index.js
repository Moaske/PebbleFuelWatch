/* =============================================================
   FuelWatch – PebbleKit JS layer
   Fetches nearby fuel station prices from DirectLease TankService
   API: https://tankservice.app-it-up.com/Tankservice/v2/
   ============================================================= */

/* ----------------------------------------------------------
   Clay – handles showConfiguration / webviewclosed automatically
---------------------------------------------------------- */
var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

/* ----------------------------------------------------------
   Constants
---------------------------------------------------------- */
var API_BASE      = 'https://tankservice.app-it-up.com/Tankservice/v2';
var API_PLACES    = API_BASE + '/places?fmt=web&country=NL&country=BE&lang=en';
var API_STATION   = API_BASE + '/places/{id}?_v48&lang=en';
var USER_AGENT    = 'HomeAssistant/pyfuelprices/2026.3.0';

var PLACES_TTL_MS = 24 * 60 * 60 * 1000;  // 24 hours
var PRICES_TTL_MS =  1 * 60 * 60 * 1000;  // 1 hour
var MAX_STATIONS  = 10;                    // stations sent to watch
var PRICE_FETCH_N =  5;                    // detail fetches per refresh
var DRIFT_KM      =  5.0;                  // re-filter threshold (km)

/* ----------------------------------------------------------
   Fuel type helpers
   Maps Clay setting value to the API fuel name patterns we match.
---------------------------------------------------------- */
var FUEL_TYPES = {
  'E10':    ['E10', 'EURO95', 'EURO 95'],
  'E5':     ['E5', 'EURO98', 'EURO 98', 'SP98'],
  'DIESEL': ['B7', 'DIESEL'],
  'LPG':    ['LPG', 'AUTOGAS', 'AUTO GAS']
};

function getFuelTypeSetting() {
  try {
    // Clay stores settings in localStorage under 'clay-settings'
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

/* Extract price for the selected fuel type from a fuels array.
   Returns price in euros (float), or null if not available. */
function extractFuelPrice(fuels, fuelType) {
  var patterns = FUEL_TYPES[fuelType] || FUEL_TYPES['E10'];
  for (var i = 0; i < fuels.length; i++) {
    var f = fuels[i];
    if (f.price === null || f.price === undefined) continue;
    var match = (f.name || '').match(/\(([^)]+)\)/);
    var type  = match ? match[1].toUpperCase() : (f.name || '').toUpperCase();
    for (var j = 0; j < patterns.length; j++) {
      if (type === patterns[j]) {
        return f.price / 1000;  // API returns millicents
      }
    }
  }
  return null;
}

/* ----------------------------------------------------------
   Tiny SHA-1 (RFC 3174) – no external deps needed
   Ported from Paul Johnston's public-domain implementation
---------------------------------------------------------- */
function sha1(msg) {
  function rotate(n, s) { return (n << s) | (n >>> (32 - s)); }
  function toHex(n) {
    var s = '', v;
    for (var i = 7; i >= 0; i--) {
      v = (n >>> (i * 4)) & 0xf;
      s += v.toString(16);
    }
    return s;
  }
  var msgLen = msg.length;
  var wordArray = [];
  for (var i = 0; i < msgLen - 3; i += 4) {
    wordArray.push(
      (msg.charCodeAt(i)   << 24) | (msg.charCodeAt(i+1) << 16) |
      (msg.charCodeAt(i+2) <<  8) |  msg.charCodeAt(i+3)
    );
  }
  var rem = msgLen % 4;
  var last = 0;
  for (var j = 0; j < rem; j++) last |= msg.charCodeAt(msgLen - rem + j) << (24 - j * 8);
  last |= 0x80 << (24 - rem * 8);
  wordArray.push(last);
  while (wordArray.length % 16 !== 14) wordArray.push(0);
  wordArray.push(msgLen >>> 29);
  wordArray.push((msgLen << 3) & 0xffffffff);

  var H = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
  var W = new Array(80);

  for (var b = 0; b < wordArray.length; b += 16) {
    for (var t = 0; t < 16; t++) W[t] = wordArray[b + t];
    for (var t = 16; t < 80; t++) W[t] = rotate(W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16], 1);
    var a = H[0], bb = H[1], c = H[2], d = H[3], e = H[4], temp;
    for (var t = 0; t < 80; t++) {
      if      (t < 20) temp = rotate(a,5) + ((bb & c) | (~bb & d)) + e + W[t] + 0x5A827999;
      else if (t < 40) temp = rotate(a,5) + (bb ^ c ^ d)           + e + W[t] + 0x6ED9EBA1;
      else if (t < 60) temp = rotate(a,5) + ((bb & c) | (bb & d) | (c & d)) + e + W[t] + 0x8F1BBCDC;
      else             temp = rotate(a,5) + (bb ^ c ^ d)           + e + W[t] + 0xCA62C1D6;
      e = d; d = c; c = rotate(bb, 30); bb = a; a = temp & 0xffffffff;
    }
    H[0] = (H[0] + a) & 0xffffffff; H[1] = (H[1] + bb) & 0xffffffff;
    H[2] = (H[2] + c) & 0xffffffff; H[3] = (H[3] + d)  & 0xffffffff;
    H[4] = (H[4] + e) & 0xffffffff;
  }
  return toHex(H[0]) + toHex(H[1]) + toHex(H[2]) + toHex(H[3]) + toHex(H[4]);
}

/* ----------------------------------------------------------
   UUID v4 generator
---------------------------------------------------------- */
function generateUUID() {
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
    var r = (Math.random() * 16) | 0;
    var v = c === 'x' ? r : (r & 0x3) | 0x8;
    return v.toString(16);
  });
}

/* ----------------------------------------------------------
   X-Checksum header generator
---------------------------------------------------------- */
function generateChecksum(url) {
  var now       = new Date();
  var dateStr   = now.toISOString().slice(0, 10).replace(/-/g, '');
  var deviceId  = generateUUID();
  var dateUuid  = dateStr + '_' + deviceId;
  var timestamp = Math.floor(Date.now() / 1000);
  var parts     = url.split('/');
  var filePath  = '/' + parts.slice(3).join('/');
  var baseStr   = dateUuid + '/' + timestamp + '/' + filePath + '/X-Checksum';
  return dateUuid + '/' + timestamp + '/' + sha1(baseStr);
}

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
   XHR-based fetch wrapper (PebbleKit JS has no fetch API)
---------------------------------------------------------- */
function fetchWithTimeout(url, options, timeoutMs) {
  timeoutMs = timeoutMs || 10000;
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
      reject(new Error('Request timed out: ' + url));
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
      reject(new Error('Network error: ' + url));
    };

    xhr.send(null);
  });
}

/* ----------------------------------------------------------
   Fetch the full places list (NL + BE)
---------------------------------------------------------- */
function fetchPlaces() {
  console.log('[FuelWatch] Fetching full places list');
  return fetchWithTimeout(API_PLACES, {
    method:  'GET',
    headers: {
      'User-Agent': USER_AGENT,
      'X-Checksum': generateChecksum(API_PLACES)
    }
  }).then(function(r) {
    if (r.status === 403) throw new Error('IP_BLOCKED');
    if (!r.ok) throw new Error('Places fetch failed: HTTP ' + r.status);
    return r.json();
  }).then(function(data) {
    if (!Array.isArray(data)) throw new Error('Places response is not an array');
    cacheSet('fw_places', data);
    cacheSet('fw_places_ts', Date.now());
    console.log('[FuelWatch] Places list cached: ' + data.length + ' stations');
    return data;
  });
}

/* ----------------------------------------------------------
   Get places list from cache or fetch fresh
---------------------------------------------------------- */
function getPlaces() {
  var cached = cacheGet('fw_places');
  var ts     = cacheGet('fw_places_ts') || 0;
  var age    = Date.now() - ts;
  if (cached && Array.isArray(cached) && age < PLACES_TTL_MS) {
    console.log('[FuelWatch] Using cached places list (' +
      Math.round(age / 60000) + ' min old)');
    return Promise.resolve(cached);
  }
  return fetchPlaces();
}

/* ----------------------------------------------------------
   Fetch price detail for a single station
   Returns { price (for selected fuel type), address, brand, name }
   price is null if the selected fuel type is not available.
---------------------------------------------------------- */
function fetchStationPrices(stationId, fuelType) {
  var url      = API_STATION.replace('{id}', stationId);
  var checksum = generateChecksum(url);
  return fetchWithTimeout(url, {
    method:  'GET',
    headers: {
      'User-Agent': USER_AGENT,
      'X-Checksum': checksum
    }
  }).then(function(r) {
    if (r.status === 403) throw new Error('IP_BLOCKED');
    if (!r.ok) throw new Error('Station fetch failed: HTTP ' + r.status);
    return r.json();
  }).then(function(data) {
    return {
      price:   extractFuelPrice(data.fuels || [], fuelType),
      address: data.address || '',
      brand:   data.brand   || '',
      name:    data.name    || ''
    };
  });
}

/* ----------------------------------------------------------
   Get prices for a station – from cache or fetch fresh.
   Cache is keyed by stationId + fuelType so changing fuel type
   bypasses the cache correctly.
---------------------------------------------------------- */
function getStationPrices(stationId, fuelType) {
  var cacheKey   = 'fw_prices_' + fuelType;
  var priceCache = cacheGet(cacheKey) || {};
  var cached     = priceCache[stationId];
  var now        = Date.now();

  if (cached && (now - cached.ts) < PRICES_TTL_MS) {
    return Promise.resolve(cached);
  }

  return fetchStationPrices(stationId, fuelType).then(function(data) {
    priceCache[stationId] = {
      price:   data.price,
      address: data.address,
      brand:   data.brand,
      name:    data.name,
      ts:      now
    };
    cacheSet(cacheKey, priceCache);
    return priceCache[stationId];
  });
}

/* ----------------------------------------------------------
   AppMessage packing
   Format per station: id|name|address|dist_m|fuel_mills|lat_e6|lon_e6
   (7 fields — single price for the selected fuel type)
   Stations separated by newline, sent as single string on KEY_STATIONS.
---------------------------------------------------------- */
var KEY_STATUS    = 0;
var KEY_STATIONS  = 1;
var KEY_OWN_LAT   = 2;
var KEY_OWN_LON   = 3;

var STATUS_OK      = 0;
var STATUS_ERROR   = 1;
var STATUS_BLOCKED = 2;
var STATUS_LOCERR  = 3;

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
    msg[KEY_STATUS] = (result.code === 'blocked') ? STATUS_BLOCKED : STATUS_ERROR;
  }
  Pebble.sendAppMessage(msg, function() {
    console.log('[FuelWatch] AppMessage sent OK');
  }, function(e) {
    console.error('[FuelWatch] AppMessage send failed: ' + e.error.message);
  });
}

/* ----------------------------------------------------------
   Mock station data for emulator testing (used on IP_BLOCKED)
---------------------------------------------------------- */
var MOCK_STATIONS = [
  { id: 1001, lat: 51.5730, lon: 5.1015, name: 'Shell',   address: 'Rijksweg 12, Tilburg',
    prices: { E10: 1.979, E5: 2.049, DIESEL: 1.849, LPG: 0.939 } },
  { id: 1002, lat: 51.5698, lon: 5.0988, name: 'TinQ',    address: 'Bredaseweg 44, Tilburg',
    prices: { E10: 1.949, E5: 2.019, DIESEL: 1.829, LPG: 0.919 } },
  { id: 1003, lat: 51.5745, lon: 5.0955, name: 'BP',      address: 'Ringbaan West 8, Tilburg',
    prices: { E10: 1.969, E5: 2.039, DIESEL: 1.839, LPG: null   } },
  { id: 1004, lat: 51.5672, lon: 5.1034, name: 'Q8',      address: 'Scharnerweg 47, Tilburg',
    prices: { E10: 1.989, E5: 2.059, DIESEL: 1.859, LPG: 0.929 } },
  { id: 1005, lat: 51.5760, lon: 5.1050, name: 'Esso',    address: 'Spoorlaan 400, Tilburg',
    prices: { E10: 1.959, E5: 2.029, DIESEL: 1.835, LPG: null   } },
  { id: 1006, lat: 51.5650, lon: 5.0970, name: 'Texaco',  address: 'Koningshoeven 1, Tilburg',
    prices: { E10: 1.999, E5: 2.069, DIESEL: 1.869, LPG: 0.949 } },
  { id: 1007, lat: 51.5800, lon: 5.0900, name: 'Tamoil',  address: 'Hasseltweg 22, Tilburg',
    prices: { E10: 1.939, E5: 2.009, DIESEL: 1.819, LPG: null   } }
];

function useMockData(lat, lon) {
  var fuelType = getFuelTypeSetting();
  console.log('[FuelWatch] Using mock data for fuel type: ' + fuelType);

  var stations = MOCK_STATIONS
    .map(function(s) {
      return {
        id:      s.id,
        lat:     s.lat,
        lon:     s.lon,
        name:    s.name,
        address: s.address,
        dist:    haversine(lat, lon, s.lat, s.lon),
        price:   s.prices[fuelType] || null
      };
    })
    .filter(function(s) { return s.price !== null; })  // exclude unavailable
    .sort(function(a, b) { return a.dist - b.dist; });

  sendToWatch({ status: 'ok', stations: stations, lat: lat, lon: lon });
}

/* ----------------------------------------------------------
   Core refresh: given a position, build the nearest-N list
   with prices and send to watch.
   Stations without a price for the selected fuel type are excluded.
---------------------------------------------------------- */
function refresh(lat, lon) {
  var fuelType = getFuelTypeSetting();
  console.log('[FuelWatch] Refreshing for position ' + lat + ',' + lon +
              ' fuel: ' + fuelType);

  getPlaces().then(function(places) {
    var annotated = places.map(function(s) {
      return {
        id:    s.id,
        lat:   s.lat,
        lon:   s.lng,
        name:  s.name  || s.brand || ('Station ' + s.id),
        city:  s.city  || '',
        brand: s.brand || '',
        dist:  haversine(lat, lon, s.lat, s.lng)
      };
    });

    annotated.sort(function(a, b) { return a.dist - b.dist; });

    // Fetch prices for the nearest candidates — we fetch more than
    // MAX_STATIONS because some may not carry the selected fuel type
    var candidates    = annotated.slice(0, MAX_STATIONS * 3);
    var pricePromises = candidates.map(function(s) {
      return getStationPrices(s.id, fuelType).then(function(data) {
        return {
          id:      s.id,
          lat:     s.lat,
          lon:     s.lon,
          name:    data.name    || s.name,
          address: data.address || s.city,
          dist:    s.dist,
          price:   data.price   // null if fuel type unavailable
        };
      }).catch(function(err) {
        console.warn('[FuelWatch] Price fetch failed for ' + s.id + ': ' + err.message);
        return null;  // drop this station on fetch error
      });
    });

    Promise.all(pricePromises).then(function(results) {
      var stations = results
        .filter(function(s) { return s !== null && s.price !== null; })
        .slice(0, MAX_STATIONS);

      console.log('[FuelWatch] Stations with ' + fuelType + ' price: ' + stations.length);

      cacheSet('fw_last_lat', lat);
      cacheSet('fw_last_lon', lon);
      sendToWatch({ status: 'ok', stations: stations, lat: lat, lon: lon });
    });

  }).catch(function(err) {
    console.error('[FuelWatch] Refresh failed: ' + err.message);
    if (err.message === 'IP_BLOCKED') {
      useMockData(lat, lon);
    } else {
      sendToWatch({ status: 'error', code: 'fetch_failed' });
    }
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
      var lastLat = cacheGet('fw_last_lat');
      var lastLon = cacheGet('fw_last_lon');
      if (lastLat !== null && lastLon !== null) {
        var drift = haversine(lat, lon, lastLat, lastLon);
        console.log('[FuelWatch] Position drift: ' + drift.toFixed(2) + 'km');
        if (drift > DRIFT_KM) {
          console.log('[FuelWatch] Drift exceeds threshold, resetting position cache');
          cacheSet('fw_last_lat', null);
          cacheSet('fw_last_lon', null);
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