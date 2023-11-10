/*
 * ESP8266 NTP Clock is written by John Rogers.
 * john@wizworks.net
 * 
 * Permission for use is free to all, with the only condition that this notice is intact within
 * all copies or derivatives of this code.  Improvements are welcome by pull request on the github
 * master repo.  
 * 
 * ESP8266 NTP Clock:  https://github.com/K1WIZ/ESP8266-8x32-Matrix-clock
 * 
 */
#include "Arduino.h"
#include <ESP8266WiFi.h>  //ESP8266 Core WiFi Library (you most likely already have this in your sketch)
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>  // For NTP Client
// =============================DEFINE VARS==============================
#define MAX_DIGITS 16
byte dig[MAX_DIGITS] = { 0 };
byte digold[MAX_DIGITS] = { 0 };
byte digtrans[MAX_DIGITS] = { 0 };
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
float utcOffset;  // UTC - Now set via WiFi Manager!
long epoch;
long localMillisAtUpdate;
int day, month, year, dayOfWeek;
int adjustedHour;
String clockHostname = "NTP-Clock";
const int utcOffsetInSeconds = utcOffset * 3600;  
WiFiManagerParameter custom_utc_offset("utcoffset", "UTC Offset", String(utcOffset).c_str(), 10);
WiFiManagerParameter custom_is_12h("is12h", "12 Hour Format", "true", 6);
WiFiManager wifiManager;

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

#define NUM_MAX 4

// for NodeMCU 1.0
#define DIN_PIN 13  // D7
#define CS_PIN 12   // D6
#define CLK_PIN 14  // D5
#include "max7219.h"
#include "fonts.h"
#define UTC_OFFSET_ADDRESS 0

void setup() {
  Serial.begin(115200);
  // Initialize EEPROM
  EEPROM.begin(sizeof(float));
  wifiManager.setSaveParamsCallback(saveConfigCallback);
  wifiManager.addParameter(&custom_utc_offset);
  wifiManager.addParameter(&custom_is_12h);
  WiFi.mode(WIFI_STA);
  initMAX7219();
  sendCmdAll(CMD_SHUTDOWN, 1);
  sendCmdAll(CMD_INTENSITY, 0);
  Serial.print("Connecting WiFi ");
  WiFi.hostname(clockHostname.c_str());

  if (!wifiManager.autoConnect("NTP Clock Setup")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
    delay(5000);
  }

  utcOffset = atof(custom_utc_offset.getValue());

  printStringWithShift("Connecting", 15);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // Read the utcOffset value from EEPROM
  EEPROM.get(UTC_OFFSET_ADDRESS, utcOffset);

  Serial.println("");
  Serial.print("MyIP: ");
  Serial.println(WiFi.localIP());
  Serial.print("UTC Offset: ");
  Serial.println(utcOffset);
  printStringWithShift((String("  MyIP: ") + WiFi.localIP().toString()).c_str(), 15);
  delay(1500);
  // Start NTP Client
  timeClient.begin();
}

// =======================================================================

void saveConfigCallback() {
  utcOffset = atof(custom_utc_offset.getValue());
  is12HFormat = String(custom_is_12h.getValue()) != "false";

  // Store the utcOffset value in EEPROM
  EEPROM.put(UTC_OFFSET_ADDRESS, utcOffset);
  EEPROM.commit();
}

// =======================================================================
void loop() {
  if (updCnt <= 0) {  // every 10 scrolls, ~450s=7.5m
    updCnt = 60;
    Serial.println("Getting data ...");
    printStringWithShift("   Setting Time...", 15);
    getTime();
    Serial.println("Data loaded");
    clkTime = millis();
  }

  if (millis() - clkTime > 20000 && !del && dots) {  // clock for 15s, then scrolls for about 30s
    updCnt--;
    clkTime = millis();
  }
  if (millis() - dotTime > 500) {
    dotTime = millis();
    dots = !dots;
  }
  updateTime(epoch, localMillisAtUpdate);
  setIntensity();
  showAnimClock();
}

// =======================================================================
void setIntensity() {  // Removed the 'int h' parameter because it's not used
  // Convert back to 24-hour format for the comparison
  int intensityHour = is12HFormat && isPM ? adjustedHour + 12 : adjustedHour;
  if (intensityHour > 12) {  // Correct for the midnight case
    intensityHour -= 24;
  }

  if (intensityHour >= 22 || intensityHour <= 6) {
    sendCmdAll(CMD_INTENSITY, 0);
  }
  else if ((intensityHour >= 7 && intensityHour <= 10) || (intensityHour >= 16 && intensityHour < 18)) {
    sendCmdAll(CMD_INTENSITY, 3);
  }
  else if (intensityHour >= 11 && intensityHour <= 15) {
    sendCmdAll(CMD_INTENSITY, 5);
  }
  else if (intensityHour >= 19 && intensityHour <= 22) {
    sendCmdAll(CMD_INTENSITY, 2);
  }
}

// =======================================================================

void showAnimClock() {
  byte digPos[6] = { 0, 8, 17, 25, 34, 42 };
  int digHt = 12;
  int num = 6;
  int i;
  if (del == 0) {
    del = digHt;
    for (i = 0; i < num; i++) digold[i] = dig[i];

    if (is12HFormat) {
      dig[0] = h / 10 ? h / 10 : 10;  //12H Mode
    } else {
      dig[0] = h / 10;
    }
    dig[1] = h % 10;
    dig[2] = m / 10;
    dig[3] = m % 10;
    dig[4] = s / 10;
    dig[5] = s % 10;
    for (i = 0; i < num; i++) digtrans[i] = (dig[i] == digold[i]) ? 0 : digHt;
  } else
    del--;

  clr();
  for (i = 0; i < num; i++) {
    if (digtrans[i] == 0) {
      dy = 0;
      showDigit(dig[i], digPos[i], dig6x8);
    } else {
      dy = digHt - digtrans[i];
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

// =======================================================================

void showDigit(char ch, int col, const uint8_t* data) {
  if (dy<-8 | dy> 8) return;
  int len = pgm_read_byte(data);
  int w = pgm_read_byte(data + 1 + ch * len);
  col += dx;
  for (int i = 0; i < w; i++)
    if (col + i >= 0 && col + i < 8 * NUM_MAX) {
      byte v = pgm_read_byte(data + 1 + ch * len + 1 + i);
      if (!dy) scr[col + i] = v;
      else scr[col + i] |= dy > 0 ? v >> dy : v << -dy;
    }
}

// =======================================================================

void setCol(int col, byte v) {
  if (dy<-8 | dy> 8) return;
  col += dx;
  if (col >= 0 && col < 8 * NUM_MAX)
    if (!dy) scr[col] = v;
    else scr[col] |= dy > 0 ? v >> dy : v << -dy;
}

// =======================================================================

int showChar(char ch, const uint8_t* data) {
  int len = pgm_read_byte(data);
  int i, w = pgm_read_byte(data + 1 + ch * len);
  for (i = 0; i < w; i++)
    scr[NUM_MAX * 8 + i] = pgm_read_byte(data + 1 + ch * len + 1 + i);
  scr[NUM_MAX * 8 + i] = 0;
  return w;
}


// =======================================================================

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

// =======================================================================

void printStringWithShift(const char* s, int shiftDelay) {
  while (*s) {
    printCharWithShift(*s, shiftDelay);
    s++;
  }
}
// =======================================================================

long getTime() {
  timeClient.update();

  epoch = timeClient.getEpochTime();
  Serial.println(epoch);
  h = ((epoch % 86400L) / 3600) % 24; // Keep this for 24-hour calculations
  m = (epoch % 3600) / 60;
  s = epoch % 60;
  localMillisAtUpdate = millis();

  // Convert to 12-hour format if IS_12H is true
  isPM = h >= 12;
  if (is12HFormat) {
    if (h == 0) { // Midnight case
      h = 12;
    } else if (h > 12) { // Afternoon case
      h -= 12;
    }
    // You could also set an AM/PM indicator here
  }

  return epoch, localMillisAtUpdate;
}

// =======================================================================

// Function to check if the current time is within daylight saving time
bool isDST(int day, int month, int year) {
  if (month > 3 && month < 10) {
    // DST is in effect between the second Sunday in March and the first Sunday in November
    if (month == 3 && day >= 8 - ((5 * year / 4 + 1) % 7)) return true;
    if (month == 11 && day <= 1 - ((5 * year / 4 + 1) % 7)) return true;
    if (month > 3 && month < 11) return true;
  }
  return false;
}

void updateTime(long epoch, long localMillisAtUpdate) {
  epoch = epoch + (long)(utcOffset * 3600);

  // Adjust the utcOffset if DST is in effect
  if (isDST(day, month, year)) {
    epoch -= 3600; // Subtract 1 hour if DST is in effect
  }

  epoch += ((millis() - localMillisAtUpdate) / 1000);
  h = ((epoch % 86400L) / 3600) % 24; // Keep this for 24-hour calculations
  m = (epoch % 3600) / 60;
  s = epoch % 60;

  // Convert to 12-hour format if IS_12H is true
  isPM = h >= 12;
  if (is12HFormat) {
    if (h == 0) { // Midnight case
      h = 12;
    } else if (h > 12) { // Afternoon case
      h -= 12;
    }
    // You could also set an AM/PM indicator here
  }

  adjustedHour = h; // Make sure this is the hour you want to use for display purposes
}
