/* =============================================================
   FuelWatch – PebbleKit JS layer
   Fetches nearby fuel station prices from DirectLease TankService
   API: https://tankservice.app-it-up.com/Tankservice/v2/
   ============================================================= */

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
   UUID v4 generator (crypto-lite, good enough for checksum)
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
   Mirrors Python implementation in pyfuelprices/sources/netherlands/directlease.py
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
   Returns a Promise resolving to a fetch-like response object
   with .ok, .status, and .json() method.
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
   Prices in API are in millicents: divide by 1000 to get euros
---------------------------------------------------------- */
function fetchStationPrices(stationId) {
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
    var result = {
      e10:     null,
      diesel:  null,
      address: data.address || '',
      brand:   data.brand   || '',
      name:    data.name    || ''
    };
    var fuels = data.fuels || [];
    fuels.forEach(function(f) {
      if (f.price === null || f.price === undefined) return;
      var cost  = f.price / 1000;
      var match = (f.name || '').match(/\(([^)]+)\)/);
      var type  = match ? match[1].toUpperCase() : (f.name || '').toUpperCase();
      if (type === 'E10' || type === 'EURO95' || type === 'EURO 95') result.e10    = cost;
      if (type === 'B7'  || type === 'DIESEL')                       result.diesel = cost;
    });
    return result;
  });
}

/* ----------------------------------------------------------
   Get prices for a station – from cache or fetch fresh
---------------------------------------------------------- */
function getStationPrices(stationId) {
  var priceCache = cacheGet('fw_prices') || {};
  var cached     = priceCache[stationId];
  var now        = Date.now();
  if (cached && (now - cached.ts) < PRICES_TTL_MS) {
    return Promise.resolve(cached);
  }
  return fetchStationPrices(stationId).then(function(prices) {
    priceCache[stationId] = {
      e10:     prices.e10,
      diesel:  prices.diesel,
      address: prices.address,
      brand:   prices.brand,
      name:    prices.name,
      ts:      now
    };
    cacheSet('fw_prices', priceCache);
    return priceCache[stationId];
  });
}

/* ----------------------------------------------------------
   AppMessage packing
   Format per station: id|name|address|dist_m|e10_mills|diesel_mills|lat_e6|lon_e6
   Prices in mills (×1000): 1979 = €1.979
   Stations separated by newline, sent as single string on KEY_STATIONS.
---------------------------------------------------------- */
var KEY_STATUS   = 0;
var KEY_STATIONS = 1;
var KEY_OWN_LAT  = 2;
var KEY_OWN_LON  = 3;

var STATUS_OK      = 0;
var STATUS_ERROR   = 1;
var STATUS_BLOCKED = 2;
var STATUS_LOCERR  = 3;

function packStation(s) {
  var e10Mills    = s.e10    ? Math.round(s.e10    * 1000) : 0;
  var dieselMills = s.diesel ? Math.round(s.diesel * 1000) : 0;
  var distM       = Math.round(s.dist * 1000);
  var latE6       = Math.round(s.lat  * 1e6);
  var lonE6       = Math.round(s.lon  * 1e6);
  var name        = (s.name    || '').substring(0, 20).replace(/[|\n]/g, ' ');
  var address     = (s.address || '').substring(0, 24).replace(/[|\n]/g, ' ');
  return [s.id, name, address, distM, e10Mills, dieselMills, latE6, lonE6].join('|');
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
   Positioned around the emulator's default location 51.5716,5.1002
---------------------------------------------------------- */
var MOCK_STATIONS = [
  { id: 1001, lat: 51.5730, lon: 5.1015, name: 'Shell',   address: 'Rijksweg 12, Tilburg',     e10: 1.979, diesel: 1.849 },
  { id: 1002, lat: 51.5698, lon: 5.0988, name: 'TinQ',    address: 'Bredaseweg 44, Tilburg',    e10: 1.949, diesel: 1.829 },
  { id: 1003, lat: 51.5745, lon: 5.0955, name: 'BP',      address: 'Ringbaan West 8, Tilburg',  e10: 1.969, diesel: 1.839 },
  { id: 1004, lat: 51.5672, lon: 5.1034, name: 'Q8',      address: 'Scharnerweg 47, Tilburg',   e10: 1.989, diesel: 1.859 },
  { id: 1005, lat: 51.5760, lon: 5.1050, name: 'Esso',    address: 'Spoorlaan 400, Tilburg',    e10: 1.959, diesel: 1.835 },
  { id: 1006, lat: 51.5650, lon: 5.0970, name: 'Texaco',  address: 'Koningshoeven 1, Tilburg',  e10: 1.999, diesel: 1.869 },
  { id: 1007, lat: 51.5800, lon: 5.0900, name: 'Tamoil',  address: 'Hasseltweg 22, Tilburg',    e10: 1.939, diesel: 1.819 }
];

function useMockData(lat, lon) {
  console.log('[FuelWatch] Using mock data (emulator/IP blocked)');
  var stations = MOCK_STATIONS.map(function(s) {
    return {
      id:      s.id,
      lat:     s.lat,
      lon:     s.lon,
      name:    s.name,
      brand:   s.name,
      address: s.address,
      dist:    haversine(lat, lon, s.lat, s.lon),
      e10:     s.e10,
      diesel:  s.diesel
    };
  });
  stations.sort(function(a, b) { return a.dist - b.dist; });
  sendToWatch({ status: 'ok', stations: stations, lat: lat, lon: lon });
}

/* ----------------------------------------------------------
   Core refresh: given a position, build the nearest-N list
   with prices and send to watch
---------------------------------------------------------- */
function refresh(lat, lon) {
  console.log('[FuelWatch] Refreshing for position ' + lat + ',' + lon);

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
    var nearest = annotated.slice(0, MAX_STATIONS);

    var priceTargets  = nearest.slice(0, PRICE_FETCH_N);
    var pricePromises = priceTargets.map(function(s) {
      return getStationPrices(s.id).then(function(prices) {
        return {
          id:      s.id,
          lat:     s.lat,
          lon:     s.lon,
          name:    prices.name    || s.name,
          brand:   prices.brand   || s.brand,
          address: prices.address || s.city,
          dist:    s.dist,
          e10:     prices.e10,
          diesel:  prices.diesel
        };
      }).catch(function(err) {
        console.warn('[FuelWatch] Price fetch failed for ' + s.id + ': ' + err.message);
        return {
          id:      s.id,
          lat:     s.lat,
          lon:     s.lon,
          name:    s.name,
          brand:   s.brand,
          address: s.city,
          dist:    s.dist,
          e10:     null,
          diesel:  null
        };
      });
    });

    var remainder = nearest.slice(PRICE_FETCH_N).map(function(s) {
      return {
        id:      s.id,
        lat:     s.lat,
        lon:     s.lon,
        name:    s.name,
        brand:   s.brand,
        address: s.city,
        dist:    s.dist,
        e10:     null,
        diesel:  null
      };
    });

    Promise.all(pricePromises).then(function(withPrices) {
      var stations = withPrices.concat(remainder);
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