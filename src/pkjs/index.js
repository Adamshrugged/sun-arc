// Phone-side code: fetches data and sends it to the watch via AppMessage.
//
// - Ambient Weather station (optional): outdoor temp, an extra sensor
//   (greenhouse, indoor...), rain
// - Open-Meteo: forecast high/low, current conditions, and the outdoor temp
//   whenever there is no station or it is offline/stale
// - Sunrise/sunset calculated from the station's (or phone's) location
// - Coinbase: crypto spot price; open.er-api.com: exchange rate (daily)

var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

var MISSING = -999;
var STATION_STALE_MS = 20 * 60 * 1000;  // station data older than this is ignored
var MIN_REFRESH_GAP_MS = 30 * 1000;     // Ambient allows ~1 request/sec; skip duplicates
var FX_RETRY_MS = 60 * 60 * 1000;       // after a failed exchange-rate fetch
var lastRefresh = 0;

// ---- settings --------------------------------------------------------------

var DEFAULTS = {
  AMBIENT_API_KEY: '',
  AMBIENT_APP_KEY: '',
  STATION: 'auto',
  EXTRA_SENSOR: '1',
  EXTRA_ICON: 'greenhouse',
  TEMP_UNITS: 'F',
  CRYPTO: 'BTC',
  BTC_CURRENCY: 'USD',  // currency the crypto price is shown in
  FX_PAIR: 'EURUSD',
  THEME: 'navy',
  VIBRATE_DISCONNECT: true
};

// Codes shared with the watch (sun_arc.c).
var THEMES = { navy: 0, black: 1, light: 2 };
var EXTRA_ICONS = { greenhouse: 0, home: 1, thermometer: 2, pool: 3 };
var COND = { CLEAR: 0, PARTLY: 1, CLOUDY: 2, FOG: 3, RAIN: 4, SNOW: 5, STORM: 6 };

// Exchange-rate pairs: label shown before the rate.
var FX_PAIRS = {
  USDJPY: { base: 'USD', quote: 'JPY', label: '¥' },
  EURUSD: { base: 'EUR', quote: 'USD', label: '€' },
  GBPUSD: { base: 'GBP', quote: 'USD', label: '£' },
  USDCAD: { base: 'USD', quote: 'CAD', label: 'C$' },
  AUDUSD: { base: 'AUD', quote: 'USD', label: 'A$' },
  USDMXN: { base: 'USD', quote: 'MXN', label: 'MX$' },
  USDINR: { base: 'USD', quote: 'INR', label: 'INR ' },
  USDCNY: { base: 'USD', quote: 'CNY', label: 'CNY ' }
};

function savedSettings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

function settings() {
  var saved = savedSettings();
  var s = {};
  for (var k in DEFAULTS) {
    s[k] = saved[k] !== undefined && saved[k] !== null && saved[k] !== '' ? saved[k] : DEFAULTS[k];
  }
  s.AMBIENT_API_KEY = String(s.AMBIENT_API_KEY).trim();
  s.AMBIENT_APP_KEY = String(s.AMBIENT_APP_KEY).trim();
  return s;
}

// Installs from before the exchange-rate setting existed always showed
// USD/JPY; keep that rather than switching them to the new default.
function migrateSettings() {
  var saved = savedSettings();
  if (Object.keys(saved).length > 0 && saved.FX_PAIR === undefined) {
    clay.setSettings('FX_PAIR', 'USDJPY');
  }
}

// Fahrenheit reading -> display units
function toUnits(f) {
  return settings().TEMP_UNITS === 'C' ? (f - 32) * 5 / 9 : f;
}

// ---- helpers ---------------------------------------------------------------

function getJSON(url, callback) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        callback(null, JSON.parse(xhr.responseText));
      } catch (e) {
        callback('bad JSON');
      }
    } else {
      callback('HTTP ' + xhr.status);
    }
  };
  xhr.onerror = function() { callback('network error'); };
  xhr.timeout = 20000;
  xhr.ontimeout = function() { callback('timeout'); };
  xhr.open('GET', url);
  xhr.send();
}

function num(v) {
  return typeof v === 'number' && isFinite(v) ? v : null;
}

function loadJSON(key) {
  try {
    return JSON.parse(localStorage.getItem(key));
  } catch (e) {
    return null;
  }
}

// ---- sunrise / sunset ------------------------------------------------------

// Sunrise equation (NOAA-style approximation, ~1 min accuracy). Returns UTC
// seconds for the sunrise and sunset of the solar day nearest now, or null
// for polar day/night. Times are sent as UTC because the phone's JS time zone
// can't be trusted (it was off by an hour on Android); the watch converts.
function sunTimes(lat, lon) {
  var rad = Math.PI / 180;
  var jdNow = Date.now() / 86400000 + 2440587.5;
  var n = Math.round(jdNow - 2451545 + lon / 360);

  var jStar = n - lon / 360;
  var M = (357.5291 + 0.98560028 * jStar) % 360;
  var C = 1.9148 * Math.sin(M * rad) + 0.02 * Math.sin(2 * M * rad) + 0.0003 * Math.sin(3 * M * rad);
  var lambda = (M + C + 180 + 102.9372) % 360;
  var jTransit = 2451545 + jStar + 0.0053 * Math.sin(M * rad) - 0.0069 * Math.sin(2 * lambda * rad);
  var sinDec = Math.sin(lambda * rad) * Math.sin(23.4397 * rad);
  var cosDec = Math.cos(Math.asin(sinDec));
  var cosW = (Math.sin(-0.833 * rad) - Math.sin(lat * rad) * sinDec) / (Math.cos(lat * rad) * cosDec);
  if (cosW < -1 || cosW > 1) {
    return null;
  }
  var w = Math.acos(cosW) / rad;

  function toUtcSeconds(j) {
    return Math.round((j - 2440587.5) * 86400);
  }
  return {
    rise: toUtcSeconds(jTransit - w / 360),
    set: toUtcSeconds(jTransit + w / 360)
  };
}

// ---- location ----------------------------------------------------------------

// Station coordinates are preferred (fixed, no GPS needed); phone location is
// the fallback. The last known location is cached for offline use.
function stationCoords(device) {
  var info = device.info || {};
  var c = info.coords || {};
  if (c.coords && num(c.coords.lat) !== null && num(c.coords.lon) !== null) {
    return { lat: c.coords.lat, lon: c.coords.lon };
  }
  if (c.geo && c.geo.coordinates && c.geo.coordinates.length === 2) {
    return { lat: c.geo.coordinates[1], lon: c.geo.coordinates[0] };
  }
  return null;
}

function withLocation(preferred, callback) {
  if (preferred) {
    localStorage.setItem('coords', JSON.stringify(preferred));
    return callback(preferred);
  }
  navigator.geolocation.getCurrentPosition(function(pos) {
    var c = { lat: pos.coords.latitude, lon: pos.coords.longitude };
    localStorage.setItem('coords', JSON.stringify(c));
    callback(c);
  }, function() {
    callback(loadJSON('coords'));
  }, { timeout: 15000, maximumAge: 60 * 60 * 1000 });
}

// ---- Ambient Weather -------------------------------------------------------

// The chosen station, or the one that reported most recently ('auto').
function pickDevice(devices, mac) {
  var best = null;
  for (var i = 0; i < devices.length; i++) {
    var d = devices[i];
    if (!d.lastData) continue;
    if (mac !== 'auto' && d.macAddress === mac) return d;
    if (!best || d.lastData.dateutc > best.lastData.dateutc) best = d;
  }
  return best;
}

// Remember the account's stations so the settings page can list them.
function rememberDevices(devices) {
  var list = [];
  for (var i = 0; i < devices.length; i++) {
    var d = devices[i];
    if (d.macAddress) {
      list.push({ mac: d.macAddress, name: (d.info && d.info.name) || d.macAddress });
    }
  }
  localStorage.setItem('devices', JSON.stringify(list));
}

function fetchStation(callback) {
  var s = settings();
  if (!s.AMBIENT_API_KEY || !s.AMBIENT_APP_KEY) {
    localStorage.removeItem('station');
    return callback('no Ambient keys');
  }
  var url = 'https://rt.ambientweather.net/v1/devices' +
    '?apiKey=' + encodeURIComponent(s.AMBIENT_API_KEY) +
    '&applicationKey=' + encodeURIComponent(s.AMBIENT_APP_KEY);
  getJSON(url, function(err, devices) {
    if (!err && devices && devices.length) {
      rememberDevices(devices);
    }
    var device = err ? null : pickDevice(devices || [], s.STATION);
    if (device) {
      localStorage.setItem('station', JSON.stringify(device));
      return callback(null, device);
    }
    // On an error (e.g. HTTP 429 rate limit), reuse the last good reading;
    // the freshness check in refresh() decides whether it's still usable.
    var cached = loadJSON('station');
    if (cached) {
      console.log('Station: ' + (err || 'no data') + ', using cached reading');
      return callback(null, cached);
    }
    callback(err || 'no station data');
  });
}

// Extra sensor fields: 'in' = the console's indoor sensor, '1'..'8' = channels.
function extraReading(d, channel) {
  if (channel === 'none') return null;
  var t = channel === 'in' ? d.tempinf : d['temp' + channel + 'f'];
  var h = channel === 'in' ? d.humidityin : d['humidity' + channel];
  if (num(t) === null) return null;
  return { temp: t, humidity: num(h) };
}

// ---- Open-Meteo --------------------------------------------------------------

// WMO weather code -> condition
function conditionFromWmo(code) {
  if (code <= 1) return COND.CLEAR;
  if (code === 2) return COND.PARTLY;
  if (code === 3) return COND.CLOUDY;
  if (code === 45 || code === 48) return COND.FOG;
  if ((code >= 71 && code <= 77) || code === 85 || code === 86) return COND.SNOW;
  if (code >= 95) return COND.STORM;
  return COND.RAIN;  // drizzle 51-57, rain 61-67, showers 80-82
}

function fetchForecast(c, callback) {
  var url = 'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + c.lat.toFixed(3) + '&longitude=' + c.lon.toFixed(3) +
    '&current=temperature_2m,weather_code' +
    '&daily=temperature_2m_max,temperature_2m_min' +
    '&temperature_unit=' + (settings().TEMP_UNITS === 'C' ? 'celsius' : 'fahrenheit') +
    '&timezone=auto&forecast_days=1';
  getJSON(url, callback);
}

// ---- prices ------------------------------------------------------------------

// 104230 -> "104.2k", 3456.7 -> "3.5k", 13088400 -> "13.09M", 152.3 -> "152.30"
function compactPrice(p) {
  if (p >= 1e6) return (p / 1e6).toFixed(2) + 'M';
  if (p >= 1000) return (p / 1000).toFixed(1) + 'k';
  return p.toFixed(2);
}

function fxDecimals(rate) {
  return rate < 10 ? 3 : rate < 1000 ? 2 : 0;
}

function fetchCrypto(s, callback) {
  if (s.CRYPTO === 'none') return callback('');
  var url = 'https://api.coinbase.com/v2/prices/' + s.CRYPTO + '-' + s.BTC_CURRENCY + '/spot';
  getJSON(url, function(err, r) {
    var price = !err && r && r.data ? parseFloat(r.data.amount) : NaN;
    if (!isFinite(price)) {
      console.log('Crypto: ' + (err || 'bad response'));
      return callback(null);  // leave the watch's last price in place
    }
    callback(s.CRYPTO + ' ' + compactPrice(price));
  });
}

// open.er-api.com publishes once a day and asks clients not to poll more
// often, so rates are cached until the next scheduled update.
function fetchFx(s, callback) {
  var pair = FX_PAIRS[s.FX_PAIR];
  if (!pair) return callback('');
  function format(rates) {
    var rate = rates && num(rates[pair.quote]);
    return rate === null ? null : pair.label + rate.toFixed(fxDecimals(rate));
  }

  var fx = loadJSON('fx');
  if (fx && fx.base === pair.base && Date.now() < fx.next) {
    return callback(format(fx.rates));
  }
  getJSON('https://open.er-api.com/v6/latest/' + pair.base, function(err, r) {
    if (!err && r && r.result === 'success' && r.rates) {
      fx = { base: pair.base, rates: r.rates, next: (r.time_next_update_unix || 0) * 1000 + 60000 };
      localStorage.setItem('fx', JSON.stringify(fx));
    } else {
      console.log('FX: ' + (err || 'bad response'));
      if (fx && fx.base === pair.base) {
        fx.next = Date.now() + FX_RETRY_MS;
        localStorage.setItem('fx', JSON.stringify(fx));
      }
    }
    callback(fx && fx.base === pair.base ? format(fx.rates) : null);
  });
}

// Adds PRICE1 / PRICE2 to msg. '' hides a slot; null leaves the watch's value.
function fetchPrices(msg, done) {
  var s = settings();
  var pending = 2;
  function finish() {
    if (--pending === 0) done();
  }
  fetchCrypto(s, function(text) {
    if (text !== null) msg.PRICE1 = text;
    finish();
  });
  fetchFx(s, function(text) {
    if (text !== null) msg.PRICE2 = text;
    finish();
  });
}

// ---- refresh ---------------------------------------------------------------

function sendToWatch(msg) {
  Pebble.sendAppMessage(msg, function() {
    console.log('Sent to watch: ' + JSON.stringify(msg));
  }, function(e) {
    console.log('Send failed: ' + JSON.stringify(e));
  });
}

function refresh() {
  if (Date.now() - lastRefresh < MIN_REFRESH_GAP_MS) {
    return;
  }
  lastRefresh = Date.now();
  var s = settings();

  // weather and prices are fetched in parallel and sent as one message,
  // along with the watch-side settings
  var msg = {
    THEME: THEMES[s.THEME] || 0,
    VIBRATE_DISCONNECT: s.VIBRATE_DISCONNECT ? 1 : 0
  };
  var pending = 2;
  function send() {
    if (--pending === 0) sendToWatch(msg);
  }
  fetchPrices(msg, send);

  fetchStation(function(err, device) {
    var coords = null;

    if (err) {
      console.log('Station: ' + err);
      msg.EXTRA_TEMP = MISSING;
      msg.RAIN_EVENT_X100 = 0;
      msg.RAINING = 0;
    } else {
      coords = stationCoords(device);
      var d = device.lastData;
      var fresh = Date.now() - d.dateutc < STATION_STALE_MS;
      console.log('Station: ' + (fresh ? 'fresh' : 'stale') + ', ' + d.tempf + 'F');
      if (fresh && num(d.tempf) !== null) {
        msg.TEMP = Math.round(toUnits(d.tempf));
        msg.FROM_STATION = 1;
        msg.WEATHER_TIME = Math.floor(d.dateutc / 1000);
      }
      var extra = fresh ? extraReading(d, s.EXTRA_SENSOR) : null;
      msg.EXTRA_TEMP = extra ? Math.round(toUnits(extra.temp)) : MISSING;
      msg.EXTRA_HUMIDITY = extra && extra.humidity !== null ? Math.round(extra.humidity) : MISSING;
      msg.EXTRA_ICON = EXTRA_ICONS[s.EXTRA_ICON] || 0;
      msg.RAIN_EVENT_X100 = fresh && num(d.eventrainin) !== null ? Math.round(d.eventrainin * 100) : 0;
      msg.RAINING = fresh && d.hourlyrainin > 0 ? 1 : 0;
    }

    withLocation(coords, function(c) {
      if (!c) {
        console.log('No location');
        return send();
      }
      var sun = sunTimes(c.lat, c.lon);
      msg.SUNRISE_UTC = sun ? sun.rise : MISSING;
      msg.SUNSET_UTC = sun ? sun.set : MISSING;
      msg.SOUTH = c.lat < 0 ? 1 : 0;

      fetchForecast(c, function(ferr, f) {
        if (ferr || !f || !f.current || !f.daily) {
          console.log('Open-Meteo: ' + (ferr || 'bad response'));
          return send();
        }
        msg.HI = Math.round(f.daily.temperature_2m_max[0]);
        msg.LO = Math.round(f.daily.temperature_2m_min[0]);
        var cond = conditionFromWmo(f.current.weather_code);
        // the station knows better than the model whether it's raining here
        if (msg.RAINING && cond !== COND.SNOW && cond !== COND.STORM) {
          cond = COND.RAIN;
        }
        msg.CONDITION = cond;
        if (msg.TEMP === undefined && num(f.current.temperature_2m) !== null) {
          msg.TEMP = Math.round(f.current.temperature_2m);
          msg.FROM_STATION = 0;
          msg.WEATHER_TIME = Math.floor(Date.now() / 1000);
        }
        send();
      });
    });
  });
}

// ---- settings page ---------------------------------------------------------

function findItem(items, messageKey) {
  for (var i = 0; i < items.length; i++) {
    if (items[i].messageKey === messageKey) return items[i];
    if (items[i].items) {
      var found = findItem(items[i].items, messageKey);
      if (found) return found;
    }
  }
  return null;
}

// Fill the station list from the stations seen on the last fetch.
function updateStationOptions() {
  var item = findItem(clayConfig, 'STATION');
  var options = [{ label: 'Most recently reporting', value: 'auto' }];
  var devices = loadJSON('devices') || [];
  for (var i = 0; i < devices.length; i++) {
    options.push({ label: devices[i].name, value: devices[i].mac });
  }
  item.options = options;
}

Pebble.addEventListener('ready', function() {
  console.log('Sun Arc pkjs ready');
  migrateSettings();
  refresh();
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload && e.payload.REQUEST) {
    refresh();
  }
});

Pebble.addEventListener('showConfiguration', function() {
  updateStationOptions();
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) {
    return;  // closed without saving
  }
  clay.getSettings(e.response, false);  // saves to localStorage
  var s = settings();
  console.log('Settings saved: theme ' + s.THEME + ', units ' + s.TEMP_UNITS + ', station ' +
    s.STATION + ', extra ' + s.EXTRA_SENSOR + ', ' + s.CRYPTO + '/' + s.BTC_CURRENCY + ', ' +
    s.FX_PAIR + ', keys ' + (s.AMBIENT_API_KEY && s.AMBIENT_APP_KEY ? 'set' : 'missing'));
  localStorage.removeItem('station');  // cached reading may be from another station
  localStorage.removeItem('fx');       // pair may have changed
  lastRefresh = 0;
  refresh();
});
