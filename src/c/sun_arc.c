#include <pebble.h>

// Sun Arc — watchface for emery (Pebble Time 2, 200x228).
//
// Time, date and the sun's position on the arc come from the watch clock.
// Weather, sun times and prices are sent by the phone (src/pkjs) and saved
// with persist storage so the face is populated immediately after a restart.
// Build with -DMOCK_DATA (see prv_load_mock) to design against fixed values.

#define HORIZON_Y   100
#define ARC_R       82
#define GAUGE_SEGS  6

#define REFRESH_MIN       15     // ask the phone for new data this often
#define STALE_SEC         3600   // weather older than this is drawn dimmed
#define MISSING           -999   // phone sends this for unavailable values
#define PERSIST_DATA      1
#define PERSIST_VERSION   2
#define PERSIST_VIBRATE   3
#define PERSIST_THEME     4
#define DATA_VERSION      3

// Current conditions; codes match COND in src/pkjs/index.js.
typedef enum {
  COND_CLEAR, COND_PARTLY, COND_CLOUDY, COND_FOG, COND_RAIN, COND_SNOW, COND_STORM,
} Condition;

// Icon for the extra sensor; codes match EXTRA_ICONS in src/pkjs/index.js.
typedef enum {
  ICON_GREENHOUSE, ICON_HOME, ICON_THERMOMETER, ICON_POOL,
} ExtraIcon;

typedef struct {
  // outdoor weather, in the units chosen in settings
  bool has_weather;
  bool from_station;       // true = Ambient station, false = forecast fallback
  int16_t temp;
  time_t weather_time;     // when the reading was taken (UTC seconds)
  bool has_hilo;
  int16_t hi, lo;
  uint8_t condition;       // Condition
  // extra Ambient sensor (greenhouse, indoor, pool...)
  bool has_extra;
  int16_t extra_temp;
  int16_t extra_humidity;  // MISSING if the sensor has no humidity
  uint8_t extra_icon;      // ExtraIcon
  // rain event (Ambient eventrainin), hundredths of an inch
  int32_t rain_event_x100;
  bool raining;            // rain falling right now (hourly rate > 0)
  // sun times, minutes after local midnight
  bool has_sun;
  int16_t sunrise_min, sunset_min;
  bool south;              // southern hemisphere: moon lit side is mirrored
  // prices, formatted by the phone ("BTC 83.6k", "¥157.30"); empty = hidden
  char price1[16];
  char price2[16];
} Data;

// ---- color themes ----------------------------------------------------------

typedef struct {
  GColor bg, arc_track, arc_day, arc_night;
  GColor text, text_dim, accent;
  GColor sunrise, sunset, extra, station_icon, price1, price2;
  GColor sun, moon_lit, moon_dark;
  GColor cloud, cloud_small, cloud_storm, rain, snow, bolt, fog;
  GColor gauge_on, gauge_off, drop, drop_active;
  GColor icon, batt_ok;
} Theme;

typedef enum { THEME_NAVY, THEME_BLACK, THEME_LIGHT, THEME_COUNT } ThemeId;

static Theme s_themes[THEME_COUNT];

static void prv_init_themes(void) {
  Theme navy = {
    .bg = GColorOxfordBlue, .arc_track = GColorCobaltBlue,
    .arc_day = GColorChromeYellow, .arc_night = GColorLightGray,
    .text = GColorWhite, .text_dim = GColorDarkGray, .accent = GColorPictonBlue,
    .sunrise = GColorChromeYellow, .sunset = GColorMelon, .extra = GColorMintGreen,
    .station_icon = GColorMintGreen, .price1 = GColorChromeYellow, .price2 = GColorMintGreen,
    .sun = GColorYellow, .moon_lit = GColorPastelYellow, .moon_dark = GColorDarkGray,
    .cloud = GColorLightGray, .cloud_small = GColorWhite, .cloud_storm = GColorDarkGray,
    .rain = GColorVividCerulean, .snow = GColorWhite, .bolt = GColorYellow, .fog = GColorLightGray,
    .gauge_on = GColorVividCerulean, .gauge_off = GColorCobaltBlue,
    .drop = GColorPictonBlue, .drop_active = GColorCyan,
    .icon = GColorLightGray, .batt_ok = GColorGreen,
  };
  Theme black = navy;
  black.bg = GColorBlack;
  black.arc_track = GColorDarkGray;
  black.gauge_off = GColorDarkGray;

  Theme light = {
    .bg = GColorWhite, .arc_track = GColorLightGray,
    .arc_day = GColorOrange, .arc_night = GColorDarkGray,
    .text = GColorBlack, .text_dim = GColorLightGray, .accent = GColorCobaltBlue,
    .sunrise = GColorOrange, .sunset = GColorPurple, .extra = GColorIslamicGreen,
    .station_icon = GColorIslamicGreen, .price1 = GColorOrange, .price2 = GColorIslamicGreen,
    .sun = GColorOrange, .moon_lit = GColorChromeYellow, .moon_dark = GColorDarkGray,
    .cloud = GColorLightGray, .cloud_small = GColorLightGray, .cloud_storm = GColorDarkGray,
    .rain = GColorBlue, .snow = GColorPictonBlue, .bolt = GColorOrange, .fog = GColorDarkGray,
    .gauge_on = GColorVividCerulean, .gauge_off = GColorLightGray,
    .drop = GColorCobaltBlue, .drop_active = GColorBlue,
    .icon = GColorDarkGray, .batt_ok = GColorIslamicGreen,
  };
  s_themes[THEME_NAVY] = navy;
  s_themes[THEME_BLACK] = black;
  s_themes[THEME_LIGHT] = light;
}

static Window *s_window;
static Layer *s_canvas;
static Data s_data;
static struct tm s_now;
static BatteryChargeState s_batt;
static bool s_connected = true;
static bool s_vibrate_disconnect = true;
static const Theme *T;  // current theme

static GFont s_font_time, s_font_temp, s_font_date, s_font_prices, s_font_info;

#ifdef MOCK_DATA
#ifndef MOCK_CONDITION
#define MOCK_CONDITION COND_CLEAR
#endif
#ifndef MOCK_ICON
#define MOCK_ICON ICON_GREENHOUSE
#endif
#ifndef MOCK_RAIN
#define MOCK_RAIN 209
#endif
#ifndef MOCK_SOUTH
#define MOCK_SOUTH false
#endif
#ifndef MOCK_PRICE1
#define MOCK_PRICE1 "BTC 104.2k"
#endif
#ifndef MOCK_PRICE2
#define MOCK_PRICE2 "¥148.62"
#endif
static void prv_load_mock(void) {
  s_data = (Data) {
    .has_weather = true, .from_station = true, .weather_time = time(NULL),
    .temp = 77, .has_hilo = true, .hi = 81, .lo = 58, .condition = MOCK_CONDITION,
    .has_extra = true, .extra_temp = 76, .extra_humidity = 74, .extra_icon = MOCK_ICON,
    .rain_event_x100 = MOCK_RAIN, .raining = false,
    .has_sun = true, .sunrise_min = 6 * 60 + 58, .sunset_min = 18 * 60 + 49, .south = MOCK_SOUTH,
    .price1 = MOCK_PRICE1, .price2 = MOCK_PRICE2,
  };
}
#endif

static void prv_load_saved(void) {
  if (persist_exists(PERSIST_VERSION) && persist_read_int(PERSIST_VERSION) == DATA_VERSION &&
      persist_get_size(PERSIST_DATA) == (int)sizeof(Data)) {
    persist_read_data(PERSIST_DATA, &s_data, sizeof(Data));
  }
}

static void prv_save(void) {
  persist_write_int(PERSIST_VERSION, DATA_VERSION);
  persist_write_data(PERSIST_DATA, &s_data, sizeof(Data));
}

static void prv_set_theme(int id) {
  if (id < 0 || id >= THEME_COUNT) id = THEME_NAVY;
  T = &s_themes[id];
}

// ---- small helpers -------------------------------------------------------

static void prv_text(GContext *ctx, const char *s, GFont font, GRect r, GTextAlignment a, GColor c) {
  graphics_context_set_text_color(ctx, c);
  graphics_draw_text(ctx, s, font, r, GTextOverflowModeTrailingEllipsis, a, NULL);
}

static int prv_text_width(const char *s, GFont font) {
  return graphics_text_layout_get_content_size(s, font, GRect(0, 0, 200, 40),
                                               GTextOverflowModeTrailingEllipsis,
                                               GTextAlignmentLeft).w;
}

static void prv_format_clock_min(char *buf, size_t len, int minutes) {
  int h = minutes / 60, m = minutes % 60;
  if (clock_is_24h_style()) {
    snprintf(buf, len, "%02d:%02d", h, m);
  } else {
    int h12 = h % 12 == 0 ? 12 : h % 12;
    snprintf(buf, len, "%d:%02d%c", h12, m, h < 12 ? 'a' : 'p');
  }
}

// ---- icons ---------------------------------------------------------------

static void prv_sun(GContext *ctx, GPoint c, int r, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  graphics_context_set_stroke_color(ctx, col);
  graphics_fill_circle(ctx, c, r);
  for (int i = 0; i < 8; i++) {
    int32_t a = TRIG_MAX_ANGLE * i / 8;
    int dx = sin_lookup(a) * 100 / TRIG_MAX_RATIO, dy = -cos_lookup(a) * 100 / TRIG_MAX_RATIO;
    graphics_draw_line(ctx, GPoint(c.x + dx * (r + 3) / 100, c.y + dy * (r + 3) / 100),
                       GPoint(c.x + dx * (r + 6) / 100, c.y + dy * (r + 6) / 100));
  }
}

// Outline house: marks readings that come from the user's own station.
static void prv_house(GContext *ctx, int x, int y, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_draw_rect(ctx, GRect(x + 1, y + 5, 9, 6));
  graphics_draw_line(ctx, GPoint(x, y + 5), GPoint(x + 5, y));
  graphics_draw_line(ctx, GPoint(x + 5, y), GPoint(x + 10, y + 5));
}

// Extra-sensor icons, each about 13x12 with the top-left at (x, y).
static void prv_extra_icon(GContext *ctx, int x, int y, ExtraIcon icon, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_fill_color(ctx, col);
  switch (icon) {
    case ICON_HOME:  // house with a door
      graphics_draw_rect(ctx, GRect(x + 1, y + 5, 11, 7));
      graphics_draw_line(ctx, GPoint(x, y + 5), GPoint(x + 6, y - 1));
      graphics_draw_line(ctx, GPoint(x + 6, y - 1), GPoint(x + 12, y + 5));
      graphics_fill_rect(ctx, GRect(x + 5, y + 8, 3, 4), 0, GCornerNone);
      break;
    case ICON_THERMOMETER:
      graphics_draw_round_rect(ctx, GRect(x + 4, y - 1, 5, 10), 2);
      graphics_fill_circle(ctx, GPoint(x + 6, y + 10), 3);
      graphics_draw_line(ctx, GPoint(x + 6, y + 3), GPoint(x + 6, y + 9));
      break;
    case ICON_POOL:  // two waves
      for (int row = 0; row < 2; row++) {
        int wy = y + 3 + row * 5;
        for (int i = 0; i < 3; i++) {
          graphics_draw_line(ctx, GPoint(x + i * 4, wy + 1), GPoint(x + i * 4 + 2, wy - 1));
          graphics_draw_line(ctx, GPoint(x + i * 4 + 2, wy - 1), GPoint(x + i * 4 + 4, wy + 1));
        }
      }
      break;
    case ICON_GREENHOUSE:
    default:
      graphics_draw_rect(ctx, GRect(x, y + 5, 13, 7));
      graphics_draw_line(ctx, GPoint(x, y + 5), GPoint(x + 6, y));
      graphics_draw_line(ctx, GPoint(x + 6, y), GPoint(x + 12, y + 5));
      graphics_draw_line(ctx, GPoint(x + 4, y + 5), GPoint(x + 4, y + 11));
      graphics_draw_line(ctx, GPoint(x + 8, y + 5), GPoint(x + 8, y + 11));
      break;
  }
}

static void prv_raindrop(GContext *ctx, int x, int y, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_circle(ctx, GPoint(x + 4, y + 8), 4);
  for (int i = 0; i < 5; i++) {
    graphics_fill_rect(ctx, GRect(x + 4 - i * 3 / 4, y + i, 1 + i * 3 / 2, 1), 0, GCornerNone);
  }
}

// Moon phase as 0..TRIG_MAX_ANGLE (0 = new, half = full), from a reference new
// moon (2000-01-06 18:14 UTC) and the mean synodic month.
static int32_t prv_moon_phase_angle(time_t utc) {
  const int64_t ref_new_moon = 947182440;
  const int64_t synodic_ms = 2551442877LL;  // 29.530588853 days
  int64_t ms = ((int64_t)utc - ref_new_moon) * 1000 % synodic_ms;
  if (ms < 0) ms += synodic_ms;
  return (int32_t)(ms * TRIG_MAX_ANGLE / synodic_ms);
}

static int prv_isqrt(int n) {
  int r = 0;
  while ((r + 1) * (r + 1) <= n) r++;
  return r;
}

// Waxing moons are lit on the right in the northern hemisphere and on the
// left in the southern hemisphere (and the reverse while waning).
static void prv_moon(GContext *ctx, GPoint c, int r, int32_t phase) {
  graphics_context_set_fill_color(ctx, T->moon_dark);
  graphics_fill_circle(ctx, c, r);
  int32_t cosp = cos_lookup(phase);
  bool waxing = phase < TRIG_MAX_ANGLE / 2;
  graphics_context_set_fill_color(ctx, T->moon_lit);
  for (int dy = -r; dy <= r; dy++) {
    int w = prv_isqrt(r * r - dy * dy);
    int term = w * cosp / TRIG_MAX_RATIO;
    // northern hemisphere: waxing lit from the terminator to the right edge
    int x0 = waxing ? term : -w;
    int x1 = waxing ? w : -term;
    if (s_data.south) {
      int t = x0;
      x0 = -x1;
      x1 = -t;
    }
    if (x1 > x0) {
      graphics_fill_rect(ctx, GRect(c.x + x0, c.y + dy, x1 - x0 + 1, 1), 0, GCornerNone);
    }
  }
}

static void prv_battery_icon(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, T->icon);
  graphics_draw_rect(ctx, GRect(x, y, 20, 10));
  graphics_context_set_fill_color(ctx, T->icon);
  graphics_fill_rect(ctx, GRect(x + 20, y + 3, 2, 4), 0, GCornerNone);

  GColor fill = s_batt.is_charging         ? GColorVividCerulean
              : s_batt.charge_percent <= 20 ? GColorRed
              : s_batt.charge_percent <= 40 ? GColorChromeYellow
                                            : T->batt_ok;
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, GRect(x + 2, y + 2, 16 * s_batt.charge_percent / 100, 6), 0, GCornerNone);
}

// Cloud centered on c; scale 1 = small (partly cloudy), 2 = full size.
static void prv_cloud(GContext *ctx, GPoint c, int scale, GColor col) {
  // halo in the background color so the cloud separates from the sun/moon/arc
  for (int pass = 0; pass < 2; pass++) {
    int g = pass == 0 ? 1 : 0;
    graphics_context_set_fill_color(ctx, pass == 0 ? T->bg : col);
    graphics_fill_circle(ctx, GPoint(c.x - 3 * scale, c.y + scale), 2 * scale + 1 + g);
    graphics_fill_circle(ctx, GPoint(c.x + scale, c.y - scale), 3 * scale + g);
    graphics_fill_circle(ctx, GPoint(c.x + 4 * scale, c.y + scale), 2 * scale + g);
    graphics_fill_rect(ctx, GRect(c.x - 3 * scale - g, c.y + scale - g, 7 * scale + 2 * g,
                                  2 * scale + 2 + 2 * g), 0, GCornerNone);
  }
}

// Precipitation details under a full-size cloud at c.
static void prv_cloud_extras(GContext *ctx, GPoint c, Condition cond) {
  int y = c.y + 7;
  switch (cond) {
    case COND_RAIN:
      graphics_context_set_stroke_color(ctx, T->rain);
      for (int i = -1; i <= 1; i++) {
        graphics_draw_line(ctx, GPoint(c.x + i * 5 + 1, y), GPoint(c.x + i * 5 - 1, y + 4));
      }
      break;
    case COND_SNOW:
      graphics_context_set_fill_color(ctx, T->snow);
      for (int i = -1; i <= 1; i++) {
        graphics_fill_rect(ctx, GRect(c.x + i * 5, y + 1 + (i & 1) * 2, 2, 2), 0, GCornerNone);
      }
      break;
    case COND_STORM:
      graphics_context_set_stroke_color(ctx, T->bolt);
      graphics_draw_line(ctx, GPoint(c.x + 2, y - 1), GPoint(c.x - 1, y + 3));
      graphics_draw_line(ctx, GPoint(c.x - 1, y + 3), GPoint(c.x + 2, y + 3));
      graphics_draw_line(ctx, GPoint(c.x + 2, y + 3), GPoint(c.x - 1, y + 7));
      break;
    default:
      break;
  }
}

// The marker riding the arc: sun or moon, dressed for current conditions.
static void prv_sky_marker(GContext *ctx, GPoint p, bool day, int32_t moon_phase) {
  Condition cond = s_data.has_weather ? (Condition)s_data.condition : COND_CLEAR;
  graphics_context_set_fill_color(ctx, T->bg);
  graphics_fill_circle(ctx, p, 12);

  if (cond == COND_FOG) {
    graphics_context_set_stroke_color(ctx, T->fog);
    for (int i = -1; i <= 1; i++) {
      int inset = i == 0 ? 0 : 2;
      graphics_draw_line(ctx, GPoint(p.x - 8 + inset, p.y + i * 4), GPoint(p.x + 8 - inset, p.y + i * 4));
    }
    return;
  }
  bool body_visible = cond == COND_CLEAR || cond == COND_PARTLY;
  if (body_visible) {
    GPoint b = cond == COND_PARTLY ? GPoint(p.x - 3, p.y - 3) : p;
    if (day) {
      prv_sun(ctx, b, 5, T->sun);
    } else {
      prv_moon(ctx, b, 7, moon_phase);
    }
  }
  if (cond == COND_PARTLY) {
    prv_cloud(ctx, GPoint(p.x + 3, p.y + 4), 1, T->cloud_small);
  } else if (!body_visible) {
    prv_cloud(ctx, GPoint(p.x, p.y - 2), 2, cond == COND_STORM ? T->cloud_storm : T->cloud);
    prv_cloud_extras(ctx, GPoint(p.x, p.y - 2), cond);
  }
}

// Bluetooth rune with a small red X; shown only while disconnected.
static void prv_bt_off_icon(GContext *ctx, int x, int y) {
  graphics_context_set_stroke_color(ctx, T->icon);
  graphics_draw_line(ctx, GPoint(x + 5, y), GPoint(x + 5, y + 16));
  graphics_draw_line(ctx, GPoint(x + 5, y), GPoint(x + 9, y + 4));
  graphics_draw_line(ctx, GPoint(x + 9, y + 4), GPoint(x + 1, y + 12));
  graphics_draw_line(ctx, GPoint(x + 1, y + 4), GPoint(x + 9, y + 12));
  graphics_draw_line(ctx, GPoint(x + 9, y + 12), GPoint(x + 5, y + 16));
  graphics_context_set_stroke_color(ctx, GColorRed);
  for (int d = 0; d < 2; d++) {  // 2px-wide strokes without the round caps
    graphics_draw_line(ctx, GPoint(x + 13 + d, y + 9), GPoint(x + 18 + d, y + 14));
    graphics_draw_line(ctx, GPoint(x + 18 + d, y + 9), GPoint(x + 13 + d, y + 14));
  }
}

// ---- sections ------------------------------------------------------------

static bool prv_is_daytime(void) {
  int now_min = s_now.tm_hour * 60 + s_now.tm_min;
  return now_min >= s_data.sunrise_min && now_min < s_data.sunset_min;
}

static void prv_draw_arc(GContext *ctx, int hy) {
  GRect arc = GRect(100 - ARC_R, hy - ARC_R, ARC_R * 2, ARC_R * 2);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_context_set_stroke_color(ctx, T->arc_track);
  graphics_draw_arc(ctx, arc, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(-90), DEG_TO_TRIGANGLE(90));

  if (!s_data.has_sun) {
    graphics_context_set_stroke_width(ctx, 1);
    return;
  }

  // Day: sunrise -> sunset. Night: sunset -> next sunrise (today's times as
  // an approximation for tomorrow's).
  int now_min = s_now.tm_hour * 60 + s_now.tm_min;
  bool day = prv_is_daytime();
  int elapsed, span;
  if (day) {
    elapsed = now_min - s_data.sunrise_min;
    span = s_data.sunset_min - s_data.sunrise_min;
  } else {
    elapsed = (now_min - s_data.sunset_min + 1440) % 1440;
    span = 1440 - s_data.sunset_min + s_data.sunrise_min;
  }
  int32_t angle = DEG_TO_TRIGANGLE(-90) + DEG_TO_TRIGANGLE(180) * elapsed / span;

  graphics_context_set_stroke_color(ctx, day ? T->arc_day : T->arc_night);
  graphics_draw_arc(ctx, arc, GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(-90), angle);
  graphics_context_set_stroke_width(ctx, 1);

  GPoint p = gpoint_from_polar(arc, GOvalScaleModeFitCircle, angle);
  prv_sky_marker(ctx, p, day, prv_moon_phase_angle(time(NULL)));
}

// Drop + segmented bar + storm total, centered horizontally at cx.
static void prv_draw_rain_gauge(GContext *ctx, int cx, int y) {
  int32_t rain = s_data.rain_event_x100;
  int32_t scale = rain <= 100 ? 100 : rain <= 200 ? 200 : rain <= 400 ? 400 : 800;
  int filled = (GAUGE_SEGS * rain + scale - 1) / scale;  // round up so any rain shows

  static char buf[24];
  snprintf(buf, sizeof(buf), "%d.%02d\"", (int)(rain / 100), (int)(rain % 100));
  const int seg_w = 8;
  int w = 12 + GAUGE_SEGS * seg_w + 2 + prv_text_width(buf, s_font_info);
  int x = cx - w / 2;

  prv_raindrop(ctx, x, y + 6, s_data.raining ? T->drop_active : T->drop);
  for (int i = 0; i < GAUGE_SEGS; i++) {
    graphics_context_set_fill_color(ctx, i < filled ? T->gauge_on : T->gauge_off);
    graphics_fill_rect(ctx, GRect(x + 12 + i * seg_w, y + 8, seg_w - 2, 10), 1, GCornersAll);
  }
  prv_text(ctx, buf, s_font_info, GRect(x + 14 + GAUGE_SEGS * seg_w, y, 60, 24),
           GTextAlignmentLeft, T->accent);
}

// Inside the arc: outdoor temp (+ station icon), hi/lo, rain gauge during a storm.
static void prv_draw_weather(GContext *ctx, int hy) {
  bool rain = s_data.rain_event_x100 > 0;
  int ty = rain ? hy - 78 : hy - 68;

  if (!s_data.has_weather) {
    prv_text(ctx, "--°", s_font_temp, GRect(40, ty, 120, 34), GTextAlignmentCenter, T->text_dim);
  } else {
    bool stale = time(NULL) - s_data.weather_time > STALE_SEC;
    static char temp[16];
    snprintf(temp, sizeof(temp), "%d°", s_data.temp);
    prv_text(ctx, temp, s_font_temp, GRect(40, ty, 120, 34), GTextAlignmentCenter,
             stale ? T->text_dim : T->text);
    if (s_data.from_station && !stale) {
      prv_house(ctx, 100 + prv_text_width(temp, s_font_temp) / 2 + 4, ty + 10, T->station_icon);
    }
  }
  if (s_data.has_hilo) {
    static char hilo[32];
    snprintf(hilo, sizeof(hilo), "%d / %d", s_data.hi, s_data.lo);
    prv_text(ctx, hilo, s_font_info, GRect(40, ty + 30, 120, 24), GTextAlignmentCenter, T->accent);
  }
  if (rain) {
    prv_draw_rain_gauge(ctx, 100, hy - 27);
  }
}

// Under the horizon: sunrise / extra sensor / sunset.
static void prv_draw_horizon_row(GContext *ctx, int hy) {
  graphics_context_set_stroke_color(ctx, T->accent);
  graphics_draw_line(ctx, GPoint(4, hy), GPoint(195, hy));

  if (s_data.has_sun) {
    static char rise[16], set[16];
    prv_format_clock_min(rise, sizeof(rise), s_data.sunrise_min);
    prv_format_clock_min(set, sizeof(set), s_data.sunset_min);
    // left label = where the arc starts (sunrise by day, sunset by night)
    bool day = prv_is_daytime();
    prv_text(ctx, day ? rise : set, s_font_info, GRect(3, hy + 1, 60, 24), GTextAlignmentLeft,
             day ? T->sunrise : T->sunset);
    prv_text(ctx, day ? set : rise, s_font_info, GRect(137, hy + 1, 60, 24), GTextAlignmentRight,
             day ? T->sunset : T->sunrise);
  }
  if (s_data.has_extra) {
    static char buf[32];
    if (s_data.extra_humidity == MISSING) {
      snprintf(buf, sizeof(buf), "%d°", s_data.extra_temp);
    } else {
      snprintf(buf, sizeof(buf), "%d°  %d%%", s_data.extra_temp, s_data.extra_humidity);
    }
    int w = 17 + prv_text_width(buf, s_font_info);
    int x = 100 - w / 2;
    prv_extra_icon(ctx, x, hy + 7, (ExtraIcon)s_data.extra_icon, T->extra);
    prv_text(ctx, buf, s_font_info, GRect(x + 17, hy + 1, 90, 24), GTextAlignmentLeft, T->extra);
  }
}

static void prv_draw_time_date(GContext *ctx, int hy) {
  static char time_buf[8], date_buf[24];
  strftime(time_buf, sizeof(time_buf), clock_is_24h_style() ? "%H:%M" : "%l:%M", &s_now);
  // %l pads single-digit hours with a space; skip it so the time stays centered
  const char *time_str = time_buf[0] == ' ' ? time_buf + 1 : time_buf;
  prv_text(ctx, time_str, s_font_time, GRect(0, hy + 24, 200, 50), GTextAlignmentCenter, T->text);

  strftime(date_buf, sizeof(date_buf), "%a  %b %e", &s_now);
  prv_text(ctx, date_buf, s_font_date, GRect(0, hy + 68, 200, 30), GTextAlignmentCenter, T->accent);
}

// Bottom row: crypto price on the left, exchange rate on the right.
static void prv_draw_prices(GContext *ctx, int y) {
  prv_text(ctx, s_data.price1, s_font_prices, GRect(4, y, 120, 30), GTextAlignmentLeft, T->price1);
  prv_text(ctx, s_data.price2, s_font_prices, GRect(96, y, 100, 30), GTextAlignmentRight, T->price2);
}

static void prv_canvas_update(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, T->bg);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);

  prv_draw_arc(ctx, HORIZON_Y);
  prv_draw_weather(ctx, HORIZON_Y);
  prv_draw_horizon_row(ctx, HORIZON_Y);
  prv_draw_time_date(ctx, HORIZON_Y);
  prv_draw_prices(ctx, HORIZON_Y + 96);
  prv_battery_icon(ctx, 174, 6);
  if (!s_connected) {
    prv_bt_off_icon(ctx, 6, 5);
  }
}

// ---- phone communication -------------------------------------------------

static void prv_request_update(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) == APP_MSG_OK) {
    dict_write_uint8(out, MESSAGE_KEY_REQUEST, 1);
    app_message_outbox_send();
  }
}

// UTC seconds -> minutes after local midnight, using the watch's time zone.
static int16_t prv_local_minutes(time_t utc) {
  struct tm *t = localtime(&utc);
  return t->tm_hour * 60 + t->tm_min;
}

// Returns the int value for key, or MISSING if absent / unavailable.
static int32_t prv_int(DictionaryIterator *iter, uint32_t key, bool *present) {
  Tuple *t = dict_find(iter, key);
  *present = t != NULL;
  return t ? t->value->int32 : MISSING;
}

static void prv_copy_string(DictionaryIterator *iter, uint32_t key, char *dst, size_t len) {
  Tuple *t = dict_find(iter, key);
  if (t && t->type == TUPLE_CSTRING) {
    strncpy(dst, t->value->cstring, len - 1);
    dst[len - 1] = '\0';
  }
}

static void prv_inbox_received(DictionaryIterator *iter, void *context) {
#ifdef MOCK_DATA
  return;  // keep the fixed design values
#endif
  bool present;
  int32_t v;

  // settings
  v = prv_int(iter, MESSAGE_KEY_VIBRATE_DISCONNECT, &present);
  if (present) {
    s_vibrate_disconnect = v != 0;
    persist_write_bool(PERSIST_VIBRATE, s_vibrate_disconnect);
  }
  v = prv_int(iter, MESSAGE_KEY_THEME, &present);
  if (present) {
    prv_set_theme(v);
    persist_write_int(PERSIST_THEME, v);
  }

  // weather
  v = prv_int(iter, MESSAGE_KEY_TEMP, &present);
  if (present) {
    s_data.has_weather = v != MISSING;
    s_data.temp = v;
    s_data.from_station = prv_int(iter, MESSAGE_KEY_FROM_STATION, &present) == 1;
    s_data.weather_time = prv_int(iter, MESSAGE_KEY_WEATHER_TIME, &present);
  }
  v = prv_int(iter, MESSAGE_KEY_HI, &present);
  if (present) {
    int32_t lo = prv_int(iter, MESSAGE_KEY_LO, &present);
    s_data.has_hilo = v != MISSING && lo != MISSING;
    s_data.hi = v;
    s_data.lo = lo;
  }
  v = prv_int(iter, MESSAGE_KEY_CONDITION, &present);
  if (present && v >= COND_CLEAR && v <= COND_STORM) {
    s_data.condition = v;
  }
  v = prv_int(iter, MESSAGE_KEY_EXTRA_TEMP, &present);
  if (present) {
    s_data.has_extra = v != MISSING;
    s_data.extra_temp = v;
    s_data.extra_humidity = prv_int(iter, MESSAGE_KEY_EXTRA_HUMIDITY, &present);
    s_data.extra_icon = prv_int(iter, MESSAGE_KEY_EXTRA_ICON, &present);
  }
  v = prv_int(iter, MESSAGE_KEY_RAIN_EVENT_X100, &present);
  if (present) {
    s_data.rain_event_x100 = v == MISSING ? 0 : v;
    s_data.raining = prv_int(iter, MESSAGE_KEY_RAINING, &present) == 1;
  }

  // sun
  v = prv_int(iter, MESSAGE_KEY_SUNRISE_UTC, &present);
  if (present) {
    int32_t set = prv_int(iter, MESSAGE_KEY_SUNSET_UTC, &present);
    s_data.has_sun = v != MISSING && set != MISSING;
    if (s_data.has_sun) {
      s_data.sunrise_min = prv_local_minutes(v);
      s_data.sunset_min = prv_local_minutes(set);
    }
  }
  v = prv_int(iter, MESSAGE_KEY_SOUTH, &present);
  if (present) {
    s_data.south = v == 1;
  }

  // prices (an empty string hides the slot)
  prv_copy_string(iter, MESSAGE_KEY_PRICE1, s_data.price1, sizeof(s_data.price1));
  prv_copy_string(iter, MESSAGE_KEY_PRICE2, s_data.price2, sizeof(s_data.price2));

  prv_save();
  layer_mark_dirty(s_canvas);
}

// ---- lifecycle -----------------------------------------------------------

static void prv_tick_handler(struct tm *t, TimeUnits changed) {
  s_now = *t;
  if (t->tm_min % REFRESH_MIN == 0) {
    prv_request_update();
  }
  layer_mark_dirty(s_canvas);
}

static void prv_request_after_reconnect(void *context) {
  prv_request_update();
}

static void prv_connection_handler(bool connected) {
  if (connected == s_connected) {
    return;
  }
  s_connected = connected;
  if (!connected && s_vibrate_disconnect && !quiet_time_is_active()) {
    vibes_double_pulse();
  }
  if (connected) {
    // give the phone-side JS a moment to start before asking for data
    app_timer_register(5000, prv_request_after_reconnect, NULL);
  }
  layer_mark_dirty(s_canvas);
}

static void prv_battery_handler(BatteryChargeState state) {
  s_batt = state;
  layer_mark_dirty(s_canvas);
}

static void prv_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, prv_canvas_update);
  layer_add_child(root, s_canvas);
}

static void prv_window_unload(Window *window) {
  layer_destroy(s_canvas);
}

static void prv_init(void) {
  s_font_time = fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS);
  s_font_temp = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  s_font_date = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  s_font_prices = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  s_font_info = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  prv_init_themes();
#ifdef MOCK_THEME
  prv_set_theme(MOCK_THEME);
#else
  prv_set_theme(persist_exists(PERSIST_THEME) ? persist_read_int(PERSIST_THEME) : THEME_NAVY);
#endif
  if (persist_exists(PERSIST_VIBRATE)) {
    s_vibrate_disconnect = persist_read_bool(PERSIST_VIBRATE);
  }
#ifdef MOCK_DATA
  prv_load_mock();
#else
  prv_load_saved();
#endif
  time_t now = time(NULL);
  s_now = *localtime(&now);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  s_batt = battery_state_service_peek();
  battery_state_service_subscribe(prv_battery_handler);
  s_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = prv_connection_handler,
  });

  app_message_register_inbox_received(prv_inbox_received);
  app_message_open(384, 64);
}

static void prv_deinit(void) {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
