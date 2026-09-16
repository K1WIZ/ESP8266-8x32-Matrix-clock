/*
 * ESP8266 NTP Clock is written by John Rogers.
 * john@wizworks.net
 *
 * Permission for use is free to all, with the only condition that this notice is intact within
 * all copies or derivatives of this code. Improvements are welcome by pull request on the github
 * master repo.
 *
 * ESP8266 NTP Clock:  https://github.com/K1WIZ/ESP8266-8x32-Matrix-clock
 */

#include "Arduino.h"
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>

/* ================== MAX7219 PIN DEFINES (MUST BE BEFORE max7219.h) ================== */
#define NUM_MAX 4
#define DIN_PIN 13
#define CS_PIN  12
#define CLK_PIN 14

#include "max7219.h"
#include "fonts.h"

/* ================== CLOCK + DISPLAY STATE ================== */
#define MAX_DIGITS 16
byte dig[MAX_DIGITS]      = {0};
byte digold[MAX_DIGITS]   = {0};
byte digtrans[MAX_DIGITS] = {0};

bool is12HFormat = true;
bool isPM = false;

int updCnt = 0;
int dots = 0;
long dotTime = 0;
long clkTime = 0;
int dx = 0;
int dy = 0;
byte del = 0;

int h, m, s;
time_t epochUTC = 0;              // Always store UTC epoch here
uint32_t localMillisAtUpdate = 0;

int day, month, year, dayOfWeek;

/* ================== USER SETTINGS ================== */
float utcOffset = 0.0;         // hours, can be fractional
float dstExtraHours = 1.0;     // usually +1 hour

// Avoid DST_* symbol collisions with ESP8266 headers.
static const uint8_t CLOCK_DSTMODE_NONE = 0;
static const uint8_t CLOCK_DSTMODE_EU   = 1;
static const uint8_t CLOCK_DSTMODE_US   = 2;

// A single mode controls DST. NONE is the initial effective setting.
uint8_t dstMode = CLOCK_DSTMODE_NONE;

/* Track whether portal actually saved settings (prevents overwriting EEPROM on reboot) */
volatile bool configWasSaved = false;

/* ================== WIFI + NTP ================== */
String clockHostname = "NTP-Clock";
WiFiManager wifiManager;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0); // offset always 0 because we keep UTC

/* ================== EEPROM LAYOUT ================== */
struct Settings {
  float utcOffset;
  bool  observeDST; // Legacy layout: mirrored from dstMode when saving.
  bool  is12H;
  uint8_t dstMode;
  float dstExtraHours;
  uint32_t magic;
};

static const uint32_t SETTINGS_MAGIC = 0x4B315749; // "K1WI"
static const int EEPROM_ADDR = 0;

Settings settings;

/* ================== WiFiManager Parameters ================== */
char bufUtc[12]      = "0";
char buf12h[6]       = "true";
char bufDstMode[10]  = "NONE";
char bufDstExtra[12] = "1.0";

// UTC offset must be STANDARD time: New York=-5, London=0, Berlin=+1.
WiFiManagerParameter pUtc("utcoffset", "Standard UTC offset (hours; excludes DST)", bufUtc, sizeof(bufUtc));
WiFiManagerParameter p12h("is12h", "12 Hour Format? (true/false)", buf12h, sizeof(buf12h));
WiFiManagerParameter pDstMode("dstmode", "Daylight saving: AUTO_US / AUTO_EU / NONE", bufDstMode, sizeof(bufDstMode));
WiFiManagerParameter pDstExtra("dstextra", "DST Extra Hours (US/Europe: leave at 1.0)", bufDstExtra, sizeof(bufDstExtra));

// Enhance the ordinary parameter into a select with the SAME name and id.
// WiFiManager therefore receives it through its normal parameter save path.
// Without JavaScript, the labeled text field remains usable.
const char dstSelectHtml[] = R"HTML(
<script>
document.addEventListener('DOMContentLoaded', function () {
  var input = document.getElementById('dstmode');
  if (!input || input.tagName === 'SELECT') return;
  var select = document.createElement('select');
  select.id = input.id;
  select.name = input.name;
  [['AUTO_US', 'United States'], ['AUTO_EU', 'Europe'], ['NONE', 'None']].forEach(function (item) {
    var option = document.createElement('option');
    option.value = item[0]; option.textContent = item[1];
    select.appendChild(option);
  });
  select.value = input.value;
  input.parentNode.replaceChild(select, input);
});
</script>
<p>Use your standard (winter) UTC offset. US and Europe normally add one hour.
Choose None for locations that do not change clocks.</p>
)HTML";
WiFiManagerParameter pDstSelect(dstSelectHtml);

/* ================== FORWARD DECLS ================== */
void saveConfigCallback();
void loadSettings();
void applySettingsToRuntime();
bool writeSettingsFromRuntime();
void refreshPortalValues();

time_t getTimeUTC();
void updateTime(time_t epochUTC, uint32_t localMillisAtUpdate);

bool isDST_EU_UTC(time_t utcNow);
bool isDST_US_Local(time_t utcNow, float baseUtcOffsetHours);
long dstSecondsAt(time_t utcNow);

void setIntensity();
void showAnimClock();
void showDigit(char ch, int col, const uint8_t* data);
void setCol(int col, byte v);
int showChar(char ch, const uint8_t* data);
void printCharWithShift(unsigned char c, int shiftDelay);
void printStringWithShift(const char* s, int shiftDelay);

/* ================== DATE MATH (NO timegm) ================== */
// Day-of-week for Gregorian calendar. Returns 0=Sunday ... 6=Saturday.
static int dow_ymd(int y, int m, int d) {
  // Sakamoto's algorithm
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
  return dow;
}

static bool isLeap(int y) {
  return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
}

static int daysInMonth(int y, int m) {
  switch (m) {
    case 1: case 3: case 5: case 7: case 8: case 10: case 12: return 31;
    case 4: case 6: case 9: case 11: return 30;
    case 2: return isLeap(y) ? 29 : 28;
    default: return 30;
  }
}

static int lastSundayOfMonth(int y, int m) {
  int last = daysInMonth(y, m);
  int w = dow_ymd(y, m, last); // 0=Sun..6=Sat
  return last - w;             // step back to Sunday
}

static int firstSundayOfMonth(int y, int m) {
  int w = dow_ymd(y, m, 1);
  int delta = (7 - w) % 7;
  return 1 + delta;
}

static int secondSundayOfMonth(int y, int m) {
  return firstSundayOfMonth(y, m) + 7;
}

/* ================== EEPROM SETTINGS ================== */
void loadSettings() {
  EEPROM.get(EEPROM_ADDR, settings);

  // Keep the original structure and magic so existing EEPROM remains readable.
  if (settings.magic != SETTINGS_MAGIC) {
    settings = {};
    settings.utcOffset = 0.0;
    settings.observeDST = false;
    settings.is12H = true;
    settings.dstMode = CLOCK_DSTMODE_NONE;
    settings.dstExtraHours = 1.0;
    settings.magic = SETTINGS_MAGIC;
    // Defaults stay in RAM until the user saves; ordinary boots never write.
  }
  // Repair only invalid fields, preserving all other stored preferences.
  if (!isfinite(settings.utcOffset) || settings.utcOffset < -12.0 || settings.utcOffset > 14.0)
    settings.utcOffset = 0.0;
  if (!isfinite(settings.dstExtraHours) || settings.dstExtraHours < 0.0 || settings.dstExtraHours > 3.0)
    settings.dstExtraHours = 1.0;
  if (settings.dstMode > CLOCK_DSTMODE_US) settings.dstMode = CLOCK_DSTMODE_NONE;
}

void applySettingsToRuntime() {
  utcOffset = settings.utcOffset;
  is12HFormat = settings.is12H;
  // Preserve the EFFECTIVE selection from old firmware: disabled means NONE.
  dstMode = settings.observeDST ? settings.dstMode : CLOCK_DSTMODE_NONE;
  dstExtraHours = settings.dstExtraHours;
}

void refreshPortalValues() {
  dtostrf(utcOffset, 0, 4, bufUtc);
  strcpy(buf12h, is12HFormat ? "true" : "false");
  strcpy(bufDstMode, dstMode == CLOCK_DSTMODE_US ? "AUTO_US" :
         (dstMode == CLOCK_DSTMODE_EU ? "AUTO_EU" : "NONE"));
  dtostrf(dstExtraHours, 0, 4, bufDstExtra);
  // WiFiManager copies constructor buffers; changing those buffers is not enough.
  pUtc.setValue(bufUtc, sizeof(bufUtc));
  p12h.setValue(buf12h, sizeof(buf12h));
  pDstMode.setValue(bufDstMode, sizeof(bufDstMode));
  pDstExtra.setValue(bufDstExtra, sizeof(bufDstExtra));
}

bool writeSettingsFromRuntime() {
  Settings next = settings;
  next.utcOffset = utcOffset;
  next.observeDST = dstMode != CLOCK_DSTMODE_NONE;
  next.is12H = is12HFormat;
  next.dstMode = dstMode;
  next.dstExtraHours = dstExtraHours;
  next.magic = SETTINGS_MAGIC;

  // ESP8266 EEPROM commits flash only when the stored bytes change.
  EEPROM.put(EEPROM_ADDR, next);
  if (!EEPROM.commit()) {
    EEPROM.put(EEPROM_ADDR, settings); // Restore the RAM buffer after a failure.
    Serial.println("ERROR: EEPROM save failed; retaining previous settings.");
    return false;
  }
  settings = next;
  return true;
}

static bool parseHours(const char* value, float minimum, float maximum, float& result) {
  char* end;
  float parsed = strtof(value, &end);
  if (end == value) return false;
  while (isspace(static_cast<unsigned char>(*end))) ++end;
  if (*end != '\0' || !isfinite(parsed) || parsed < minimum || parsed > maximum) return false;
  result = parsed;
  return true;
}

/* ================== WIFI MANAGER SAVE ================== */
void saveConfigCallback() {
  float newUtc, newExtra;
  String mode(pDstMode.getValue());
  mode.trim(); mode.toUpperCase();
  String format(p12h.getValue());
  format.trim(); format.toLowerCase();
  if (!parseHours(pUtc.getValue(), -12.0, 14.0, newUtc) ||
      !parseHours(pDstExtra.getValue(), 0.0, 3.0, newExtra) ||
      (mode != "AUTO_US" && mode != "AUTO_EU" && mode != "NONE") ||
      (format != "true" && format != "false")) {
    Serial.println("Invalid clock settings: nothing saved. Check UTC offset, DST mode, extra hours and true/false.");
    refreshPortalValues();
    return;
  }

  utcOffset = newUtc;
  dstExtraHours = newExtra;
  is12HFormat = format == "true";
  dstMode = mode == "AUTO_US" ? CLOCK_DSTMODE_US :
            (mode == "AUTO_EU" ? CLOCK_DSTMODE_EU : CLOCK_DSTMODE_NONE);
  if (writeSettingsFromRuntime()) {
    configWasSaved = true;
    Serial.println("Clock settings saved to EEPROM.");
  } else {
    applySettingsToRuntime();
  }
  refreshPortalValues();
}

/* ================== SETUP ================== */
void setup() {
  Serial.begin(115200);
  EEPROM.begin(sizeof(Settings));

  // Load EEPROM first, and apply to runtime so a reboot keeps settings
  loadSettings();
  applySettingsToRuntime();
  refreshPortalValues();

  wifiManager.setSaveParamsCallback(saveConfigCallback);
  wifiManager.addParameter(&pUtc);
  wifiManager.addParameter(&p12h);
  wifiManager.addParameter(&pDstMode);
  wifiManager.addParameter(&pDstExtra);
  wifiManager.addParameter(&pDstSelect);

  WiFi.mode(WIFI_STA);
  WiFi.hostname(clockHostname.c_str());

  initMAX7219();
  sendCmdAll(CMD_SHUTDOWN, 1);
  sendCmdAll(CMD_INTENSITY, 0);

  Serial.print("Connecting WiFi ");

  configWasSaved = false;

  if (!wifiManager.autoConnect("NTP Clock Setup")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
    delay(5000);
  }

  // CRITICAL: do NOT overwrite EEPROM unless the callback fired
  if (configWasSaved) {
    Serial.println("Config saved via portal; EEPROM updated.");
  } else {
    Serial.println("Using stored EEPROM settings (portal not saved).");
    // Ensure runtime stays aligned to stored settings (not portal defaults)
    applySettingsToRuntime();
  }

  // Optional: show values
  Serial.println("");
  Serial.print("MyIP: ");
  Serial.println(WiFi.localIP());
  Serial.print("UTC Offset: ");
  Serial.println(utcOffset);
  Serial.print("DST Mode: ");
  Serial.println(dstMode == CLOCK_DSTMODE_US ? "AUTO_US" : (dstMode == CLOCK_DSTMODE_EU ? "AUTO_EU" : "NONE"));
  Serial.print("DST Extra Hours: ");
  Serial.println(dstExtraHours);

  printStringWithShift("Connecting", 15);
  printStringWithShift((String("  MyIP: ") + WiFi.localIP().toString()).c_str(), 15);
  delay(1000);

  Serial.println("Send C over Serial at 115200 baud to reopen NTP Clock Setup and change settings.");
  timeClient.begin();
  epochUTC = getTimeUTC();
}

/* ================== LOOP ================== */
void loop() {
  // Reconfigure without erasing WiFi credentials or EEPROM.
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'C' || command == 'c') {
      refreshPortalValues();
      wifiManager.setConfigPortalTimeout(180);
      wifiManager.startConfigPortal("NTP Clock Setup");
      epochUTC = getTimeUTC();
    }
  }
  if (updCnt <= 0) {
    updCnt = 60;
    Serial.println("Getting data ...");
    printStringWithShift("   Setting Time...", 15);
    epochUTC = getTimeUTC();
    Serial.println("Data loaded");
    clkTime = millis();
  }

  if (millis() - clkTime > 20000 && !del && dots) {
    updCnt--;
    clkTime = millis();
  }

  if (millis() - dotTime > 500) {
    dotTime = millis();
    dots = !dots;
  }

  updateTime(epochUTC, localMillisAtUpdate);
  setIntensity();
  showAnimClock();
}

/* ================== NTP ================== */
time_t getTimeUTC() {
  timeClient.update();
  epochUTC = timeClient.getEpochTime();   // UTC only
  localMillisAtUpdate = millis();
  Serial.print("UTC epoch: ");
  Serial.println(static_cast<unsigned long>(epochUTC));
  return epochUTC;
}

/* ================== DST RULES ================== */
bool isDST_EU_UTC(time_t utcNow) {
  struct tm u;
  gmtime_r(&utcNow, &u);

  int y = u.tm_year + 1900;
  int mon = u.tm_mon + 1;
  int d = u.tm_mday;
  int hr = u.tm_hour;

  if (mon < 3 || mon > 10) return false;
  if (mon > 3 && mon < 10) return true;

  if (mon == 3) {
    int lastSun = lastSundayOfMonth(y, 3);
    if (d > lastSun) return true;
    if (d < lastSun) return false;
    return (hr >= 1); // starts 01:00 UTC
  }

  // mon == 10
  int lastSun = lastSundayOfMonth(y, 10);
  if (d < lastSun) return true;
  if (d > lastSun) return false;
  return (hr < 1); // ends 01:00 UTC
}

bool isDST_US_Local(time_t utcNow, float baseUtcOffsetHours) {
  // March starts at 02:00 STANDARD time. November ends at 02:00 DAYLIGHT
  // time, which is 01:00 standard for the normal +1-hour adjustment.
  time_t localStd = utcNow + lroundf(baseUtcOffsetHours * 3600.0f);
  struct tm lt;
  gmtime_r(&localStd, &lt);
  int y = lt.tm_year + 1900;

  // Obtain Jan 1 from the current standard-time date without mktime/timezone state.
  time_t jan1 = localStd - static_cast<time_t>(lt.tm_yday) * 86400 -
                lt.tm_hour * 3600 - lt.tm_min * 60 - lt.tm_sec;
  int marchDays = 31 + daysInMonth(y, 2);
  int novemberDays = marchDays;
  for (int mon = 3; mon < 11; ++mon) novemberDays += daysInMonth(y, mon);
  time_t start = jan1 + static_cast<time_t>(marchDays + secondSundayOfMonth(y, 3) - 1) * 86400 + 7200;
  time_t end = jan1 + static_cast<time_t>(novemberDays + firstSundayOfMonth(y, 11) - 1) * 86400 +
               7200 - lroundf(dstExtraHours * 3600.0f);
  return localStd >= start && localStd < end;
}

long dstSecondsAt(time_t utcNow) {
  bool active = dstMode == CLOCK_DSTMODE_US ? isDST_US_Local(utcNow, utcOffset) :
                (dstMode == CLOCK_DSTMODE_EU ? isDST_EU_UTC(utcNow) : false);
  return active ? lroundf(dstExtraHours * 3600.0f) : 0;
}

/* ================== APPLY OFFSET + DST + UPDATE GLOBALS ================== */
void updateTime(time_t epochUtcAtSync, uint32_t millisAtSync) {
  // Unsigned subtraction handles the 32-bit millis rollover between NTP updates.
  uint32_t elapsed = static_cast<uint32_t>(millis()) - millisAtSync;
  time_t utcNow = epochUtcAtSync + elapsed / 1000;
  time_t local = utcNow + lroundf(utcOffset * 3600.0f) + dstSecondsAt(utcNow);
  struct tm t;
  gmtime_r(&local, &t);
  h = t.tm_hour;
  m = t.tm_min;
  s = t.tm_sec;
  year = t.tm_year + 1900;
  month = t.tm_mon + 1;
  day = t.tm_mday;
  dayOfWeek = t.tm_wday;
}

/* ================== INTENSITY ================== */
void setIntensity() {
  int intensityHour = h;
  if (intensityHour >= 22 || intensityHour < 6) {
    sendCmdAll(CMD_INTENSITY, 0);
  } else if ((intensityHour >= 7 && intensityHour <= 10) || (intensityHour >= 16 && intensityHour < 18)) {
    sendCmdAll(CMD_INTENSITY, 3);
  } else if (intensityHour >= 11 && intensityHour <= 15) {
    sendCmdAll(CMD_INTENSITY, 5);
  } else if (intensityHour >= 19 && intensityHour <= 22) {
    sendCmdAll(CMD_INTENSITY, 2);
  }
}

/* ================== DISPLAY (YOUR ORIGINAL ANIMATION) ================== */
void showAnimClock() {
  byte digPos[6] = { 0, 8, 17, 25, 34, 42 };
  int digHt = 12;
  int num = 6;
  int i;

  if (del == 0) {
    del = digHt;
    for (i = 0; i < num; i++) digold[i] = dig[i];

    int displayHour = h;

    if (is12HFormat) {
      isPM = displayHour >= 12;
      displayHour %= 12;
      displayHour = displayHour ? displayHour : 12;
    }

    dig[0] = displayHour / 10 ? displayHour / 10 : (is12HFormat ? 10 : 0);
    dig[1] = displayHour % 10;
    dig[2] = m / 10;
    dig[3] = m % 10;
    dig[4] = s / 10;
    dig[5] = s % 10;

    for (i = 0; i < num; i++) digtrans[i] = (dig[i] == digold[i]) ? 0 : digHt;
  } else {
    del--;
  }

  clr();
  for (i = 0; i < num; i++) {
    if (digtrans[i] == 0) {
      dy = 0;
      showDigit(dig[i], digPos[i], dig6x8);
    } else {
      dy = 12 - digtrans[i];
      showDigit(digold[i], digPos[i], dig6x8);
      dy = -digtrans[i];
      showDigit(dig[i], digPos[i], dig6x8);
      digtrans[i]--;
    }
  }
  dy = 0;

  setCol(15, dots ? B00100100 : 0);
  setCol(32, dots ? B00100100 : 0);
  refreshAll();
  delay(30);
}

void showDigit(char ch, int col, const uint8_t* data) {
  if (dy < -8 || dy > 8) return;
  int len = pgm_read_byte(data);
  int w = pgm_read_byte(data + 1 + ch * len);
  col += dx;
  for (int i = 0; i < w; i++) {
    if (col + i >= 0 && col + i < 8 * NUM_MAX) {
      byte v = pgm_read_byte(data + 1 + ch * len + 1 + i);
      if (!dy) scr[col + i] = v;
      else scr[col + i] |= dy > 0 ? v >> dy : v << -dy;
    }
  }
}

void setCol(int col, byte v) {
  if (dy < -8 || dy > 8) return;
  col += dx;
  if (col >= 0 && col < 8 * NUM_MAX) {
    if (!dy) scr[col] = v;
    else scr[col] |= dy > 0 ? v >> dy : v << -dy;
  }
}

int showChar(char ch, const uint8_t* data) {
  int len = pgm_read_byte(data);
  int i, w = pgm_read_byte(data + 1 + ch * len);
  for (i = 0; i < w; i++)
    scr[NUM_MAX * 8 + i] = pgm_read_byte(data + 1 + ch * len + 1 + i);
  scr[NUM_MAX * 8 + i] = 0;
  return w;
}

void printCharWithShift(unsigned char c, int shiftDelay) {
  if (c < ' ' || c > '~' + 25) return;
  c -= 32;
  int w = showChar(c, font);
  for (int i = 0; i < w + 1; i++) {
    delay(shiftDelay);
    scrollLeft();
    refreshAll();
  }
}

void printStringWithShift(const char* s, int shiftDelay) {
  while (*s) {
    printCharWithShift(*s, shiftDelay);
    s++;
  }
}
