/* =============================================================
   FuelWatch – PebbleKit JS layer
   Fetches nearby fuel station prices from ANWB Onderweg API
   Single bounding box call returns stations + prices for NL + BE
   API: https://api.anwb.nl/routing/points-of-interest/v3/all
   ============================================================= */

/* ----------------------------------------------------------
   Clay + message keys
---------------------------------------------------------- */
var Clay       = require('@rebble/clay');
var clayConfig = require('./config');
var messageKeys = require('message_keys');
var clay = new Clay(clayConfig);

/* ----------------------------------------------------------
   Constants
---------------------------------------------------------- */
var API_ANWB     = 'https://api.anwb.nl/routing/points-of-interest/v3/all';
var API_OVERPASS = 'https://overpass-api.de/api/interpreter';
var API_TYPE     = 'FUEL_STATION';

var BBOX_RADIUS     = 0.12;           // degrees (~13km) for station list
var MAP_BBOX_RADIUS = 0.025;          // degrees (~2.5km) for road map
var CACHE_TTL_MS    = 15 * 60 * 1000; // 15 min for station data
var ROAD_CACHE_TTL  = 24 * 60 * 60 * 1000; // 24h for road geometry
var MAX_STATIONS    = 10;
var MAX_ROAD_SEGS   = 75;             // stay under AppMessage limit
var DRIFT_KM        = 2.0;

/* Road type categories */
var ROAD_MAJOR  = 0;
var ROAD_MEDIUM = 1;
var ROAD_MINOR  = 2;

var ROAD_TYPE_MAP = {
  'motorway': ROAD_MAJOR,  'motorway_link': ROAD_MAJOR,
  'trunk':    ROAD_MAJOR,  'trunk_link':    ROAD_MAJOR,
  'primary':  ROAD_MAJOR,  'primary_link':  ROAD_MAJOR,
  'secondary':  ROAD_MEDIUM, 'secondary_link':  ROAD_MEDIUM,
  'tertiary':   ROAD_MEDIUM, 'tertiary_link':   ROAD_MEDIUM,
  'residential':   ROAD_MINOR,
  'unclassified':  ROAD_MINOR,
  'living_street': ROAD_MINOR,
  'service':       ROAD_MINOR
};

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
    if (options && options.headers) {
      Object.keys(options.headers).forEach(function(k) {
        xhr.setRequestHeader(k, options.headers[k]);
      });
    }
    var timer = setTimeout(function() { xhr.abort(); reject(new Error('Timeout')); }, timeoutMs);
    xhr.onload = function() {
      clearTimeout(timer);
      var text = xhr.responseText;
      resolve({
        ok: xhr.status >= 200 && xhr.status < 300,
        status: xhr.status,
        text: function() { return Promise.resolve(text); },
        json: function() {
          return new Promise(function(res, rej) {
            try { res(JSON.parse(text)); } catch(e) { rej(new Error('JSON parse error')); }
          });
        }
      });
    };
    xhr.onerror = function() { clearTimeout(timer); reject(new Error('Network error')); };
    xhr.send(options && options.body ? options.body : null);
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
  var addr = raw.address || {};
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
   Overpass road fetch + geometry processing
---------------------------------------------------------- */
function buildOverpassQuery(lat, lon) {
  var r = MAP_BBOX_RADIUS;
  var bbox = (lat - r).toFixed(6) + ',' + (lon - r).toFixed(6) + ',' +
             (lat + r).toFixed(6) + ',' + (lon + r).toFixed(6);
  return '[out:json][timeout:10];' +
    'way[highway~"^(motorway|motorway_link|trunk|trunk_link|primary|primary_link|' +
    'secondary|secondary_link|tertiary|tertiary_link|residential|unclassified|' +
    'living_street|service)$"](' + bbox + ');' +
    '(._;>;);out body;';
}

function fetchRoads(lat, lon) {
  var query = buildOverpassQuery(lat, lon);
  console.log('[FuelWatch] Fetching road data from Overpass');
  return fetchWithTimeout(API_OVERPASS, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'data=' + encodeURIComponent(query)
  }, 20000).then(function(r) {
    if (!r.ok) throw new Error('Overpass HTTP ' + r.status);
    return r.json();
  });
}

/* Douglas-Peucker line simplification */
function perpendicularDist(pt, lineStart, lineEnd) {
  var dx = lineEnd.lon - lineStart.lon;
  var dy = lineEnd.lat - lineStart.lat;
  var mag = Math.sqrt(dx * dx + dy * dy);
  if (mag === 0) return 0;
  return Math.abs(dy * pt.lon - dx * pt.lat + lineEnd.lon * lineStart.lat - lineEnd.lat * lineStart.lon) / mag;
}

function douglasPeucker(points, epsilon) {
  if (points.length < 3) return points;
  var maxDist = 0, maxIdx = 0;
  for (var i = 1; i < points.length - 1; i++) {
    var d = perpendicularDist(points[i], points[0], points[points.length - 1]);
    if (d > maxDist) { maxDist = d; maxIdx = i; }
  }
  if (maxDist > epsilon) {
    var left  = douglasPeucker(points.slice(0, maxIdx + 1), epsilon);
    var right = douglasPeucker(points.slice(maxIdx), epsilon);
    return left.slice(0, left.length - 1).concat(right);
  }
  return [points[0], points[points.length - 1]];
}

/* Project lat/lon to screen pixel — mirrors C-side projection */
function projectToScreen(lat, lon, bbox, screenW, screenH) {
  var padTop  = 16;
  var padBot  = 2;
  var padSide = 6;
  var mapX0 = padSide;
  var mapY0 = padTop;
  var mapW  = screenW - padSide * 2;
  var mapH  = screenH - padTop - padBot;

  var x = mapX0 + (lon - bbox.minLon) / (bbox.maxLon - bbox.minLon) * mapW;
  var y = mapY0 + (1 - (lat - bbox.minLat) / (bbox.maxLat - bbox.minLat)) * mapH;
  return { x: Math.round(x), y: Math.round(y) };
}

/* Build bounding box for projection from station coords + own pos */
function buildProjectionBbox(stations, sel, ownLat, ownLon) {
  var pts = [
    { lat: ownLat, lon: ownLon },
    { lat: stations[sel].lat, lon: stations[sel].lon }
  ];
  if (sel > 0)
    pts.push({ lat: stations[sel - 1].lat, lon: stations[sel - 1].lon });
  if (sel < stations.length - 1)
    pts.push({ lat: stations[sel + 1].lat, lon: stations[sel + 1].lon });

  var minLat = pts[0].lat, maxLat = pts[0].lat;
  var minLon = pts[0].lon, maxLon = pts[0].lon;
  pts.forEach(function(p) {
    if (p.lat < minLat) minLat = p.lat;
    if (p.lat > maxLat) maxLat = p.lat;
    if (p.lon < minLon) minLon = p.lon;
    if (p.lon > maxLon) maxLon = p.lon;
  });

  var latSpan = maxLat - minLat;
  var lonSpan = maxLon - minLon;
  if (latSpan < 0.005) { var ml = (minLat + maxLat) / 2; minLat = ml - 0.0025; maxLat = ml + 0.0025; }
  if (lonSpan < 0.005) { var mlo = (minLon + maxLon) / 2; minLon = mlo - 0.0025; maxLon = mlo + 0.0025; }

  var pad = 0.20;
  return {
    minLat: minLat - latSpan * pad,
    maxLat: maxLat + latSpan * pad,
    minLon: minLon - lonSpan * pad,
    maxLon: maxLon + lonSpan * pad
  };
}

function processRoads(overpassData, stations, sel, ownLat, ownLon, screenW, screenH) {
  var bbox     = buildProjectionBbox(stations, sel, ownLat, ownLon);
  var nodeMap  = {};
  var segments = [];

  // Build node lookup
  overpassData.elements.forEach(function(el) {
    if (el.type === 'node') nodeMap[el.id] = { lat: el.lat, lon: el.lon };
  });

  // Process ways into segments
  overpassData.elements.forEach(function(el) {
    if (el.type !== 'way' || !el.tags || !el.tags.highway) return;
    var roadType = ROAD_TYPE_MAP[el.tags.highway];
    if (roadType === undefined) return;

    // Build point array for this way
    var points = [];
    el.nodes.forEach(function(nid) {
      if (nodeMap[nid]) points.push(nodeMap[nid]);
    });
    if (points.length < 2) return;

    // Simplify — epsilon varies by road type
    var epsilon = roadType === ROAD_MINOR ? 0.0002 : 0.0001;
    var simplified = douglasPeucker(points, epsilon);

    // Convert to screen segments
    for (var i = 0; i < simplified.length - 1; i++) {
      var p1 = projectToScreen(simplified[i].lat,     simplified[i].lon,     bbox, screenW, screenH);
      var p2 = projectToScreen(simplified[i + 1].lat, simplified[i + 1].lon, bbox, screenW, screenH);
      // Skip degenerate segments (same pixel)
      if (p1.x === p2.x && p1.y === p2.y) continue;
      segments.push({ x1: p1.x, y1: p1.y, x2: p2.x, y2: p2.y, t: roadType });
    }
  });

  // Sort by road type (major first so they draw over minor)
  // then limit total count
  segments.sort(function(a, b) { return a.t - b.t; });
  return segments.slice(0, MAX_ROAD_SEGS);
}

function packRoads(segments) {
  return segments.map(function(s) {
    return s.x1 + '|' + s.y1 + '|' + s.x2 + '|' + s.y2 + '|' + s.t;
  }).join('\n');
}

/* ----------------------------------------------------------
   AppMessage packing
---------------------------------------------------------- */
/* AppMessage keys and status codes via messageKeys module */
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
    msg[messageKeys.STATUS]             = STATUS_OK;
    msg[messageKeys.STATION]            = result.stations.map(packStation).join('\n');
    msg[messageKeys.OWN_LAT]            = Math.round(result.lat * 1e6);
    msg[messageKeys.OWN_LON]            = Math.round(result.lon * 1e6);
    msg[messageKeys.FuelType]    = getFuelTypeSetting();
  } else {
    msg[messageKeys.STATUS]             = STATUS_ERROR;
    msg[messageKeys.FuelType]    = getFuelTypeSetting();
  }
  Pebble.sendAppMessage(msg,
    function() { console.log('[FuelWatch] AppMessage sent OK'); },
    function(e) { console.error('[FuelWatch] AppMessage failed: ' + e.error.message); });
}

function sendRoadsToWatch(segments) {
  var msg = {};
  msg[messageKeys.Roads] = packRoads(segments);
  Pebble.sendAppMessage(msg,
    function() { console.log('[FuelWatch] Roads sent: ' + segments.length + ' segs'); },
    function(e) { console.error('[FuelWatch] Roads send failed: ' + e.error.message); });
}

/* ----------------------------------------------------------
   Core refresh — station list
---------------------------------------------------------- */
// Store last known station list + own position for map requests
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
   Fetches Overpass road data and sends pre-projected segments
---------------------------------------------------------- */
function handleMapRequest(selectedIndex, screenW, screenH) {
  if (!s_lastStations || s_lastStations.length === 0) {
    console.warn('[FuelWatch] Map request but no station data');
    sendRoadsToWatch([]);
    return;
  }

  var lat = s_lastLat;
  var lon = s_lastLon;
  var sel = Math.min(selectedIndex, s_lastStations.length - 1);

  // Cache roads by position + selected station
  var roadCacheKey = 'fw_roads_' +
    Math.round(lat * 1000) + '_' + Math.round(lon * 1000) + '_' + sel;
  var cached = cacheGet(roadCacheKey);
  var now    = Date.now();

  if (cached && cached.ts && (now - cached.ts) < ROAD_CACHE_TTL) {
    console.log('[FuelWatch] Using cached road data');
    sendRoadsToWatch(cached.segments);
    return;
  }

  fetchRoads(lat, lon).then(function(data) {
    var segments = processRoads(data, s_lastStations, sel,
                                lat, lon, screenW, screenH);
    console.log('[FuelWatch] Processed ' + segments.length + ' road segments');
    cacheSet(roadCacheKey, { segments: segments, ts: now });
    sendRoadsToWatch(segments);
  }).catch(function(err) {
    console.error('[FuelWatch] Overpass failed: ' + err.message);
    // Send empty roads — map still shows dots
    sendRoadsToWatch([]);
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
        console.log('[FuelWatch] Drift: ' + drift.toFixed(2) + 'km');
        if (drift > DRIFT_KM) {
          cacheSet('fw_last_lat', null);
          cacheSet('fw_last_lon', null);
          try {
            var keys = Object.keys(localStorage);
            keys.forEach(function(k) {
              if (k.indexOf('fw_anwb_') === 0 || k.indexOf('fw_roads_') === 0)
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
  getLocation();
});

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('[FuelWatch] appmessage received');

  // Map request from watch
  if (payload[messageKeys.MapRequest]) {
    var sel     = payload[messageKeys.MapSelected] || 0;
    var screenW = payload[messageKeys.MapScreenW]  || 144;
    var screenH = payload[messageKeys.MapScreenH]  || 168;
    console.log('[FuelWatch] Map request: sel=' + sel +
                ' screen=' + screenW + 'x' + screenH);
    handleMapRequest(sel, screenW, screenH);
    return;
  }

  // Generic refresh request
  console.log('[FuelWatch] Refresh requested');
  getLocation();
});