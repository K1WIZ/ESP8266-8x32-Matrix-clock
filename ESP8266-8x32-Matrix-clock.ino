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
#ifdef ESP8266
  #include <ESP8266WiFi.h>  //ESP8266 Core WiFi Library (you most likely already have this in your sketch)
  #include <ESP8266WebServer.h>
#endif
#ifdef ESP32
  #include <soc/spi_pins.h>
#endif
#include <DNSServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h> 

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
float utcOffset; // UTC - Now set via WiFi Manager!
long epoch;
long localMillisAtUpdate;
int day, month, year, dayOfWeek;
int adjustedHour;
String clockHostname = "NTP-Clock";
WiFiManagerParameter custom_utc_offset("utcoffset", "UTC Offset", String(utcOffset).c_str(), 10);
WiFiManagerParameter custom_is_12h("is12h", "12 Hour Format", "true", 6);
WiFiManager wifiManager;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0); // utcOffsetInSeconds will be dynamically adjusted

#define NUM_MAX 4

#ifdef ESP8266
  // for NodeMCU 1.0
  #define DIN_PIN 13  // D7
  #define CS_PIN  12  // D6
  #define CLK_PIN 14  // D5
#else
#ifdef ESP32
  // https://lastminuteengineers.com/esp32-pinout-reference/
  // pins are defined in spi_pins.h
  #define DIN_PIN SPI2_IOMUX_PIN_NUM_MISO  // ESP32: GPIO12, ESP32-S2: GPIO13
  #define CS_PIN  SPI2_IOMUX_PIN_NUM_CS    // ESP32: GPIO15, ESP32-S2: GPIO10
  #define CLK_PIN SPI2_IOMUX_PIN_NUM_CLK   // ESP32: GPIO14, ESP32-S2: GPIO12
#else
  #error board undefined
#endif
#endif

#include "max7219.h"
#include "fonts.h"
#define UTC_OFFSET_ADDRESS 0
#define IS_12_HOUR_FORMAT  1

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
  EEPROM.get(IS_12_HOUR_FORMAT, is12HFormat);

  Serial.println("");
  Serial.print("MyIP: ");
  Serial.println(WiFi.localIP());
  Serial.print("UTC Offset: ");
  Serial.println(utcOffset);
  Serial.print("is 12 hour format: ");
  Serial.println(is12HFormat);
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
  EEPROM.put(IS_12_HOUR_FORMAT, is12HFormat);
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
void setIntensity() {
  int intensityHour = h; // Always use the hour in 24-hour format for intensity decisions

  // No need to adjust intensityHour based on is12HFormat

  // Set intensity based on the 24-hour format hour
  if (intensityHour >= 22 || intensityHour < 6) {
    sendCmdAll(CMD_INTENSITY, 0);  // Low intensity for night time
  } else if ((intensityHour >= 7 && intensityHour <= 10) || (intensityHour >= 16 && intensityHour < 18)) {
    sendCmdAll(CMD_INTENSITY, 3);  // Medium intensity for morning and late afternoon
  } else if (intensityHour >= 11 && intensityHour <= 15) {
    sendCmdAll(CMD_INTENSITY, 5);  // High intensity for midday
  } else if (intensityHour >= 19 && intensityHour <= 22) {
    sendCmdAll(CMD_INTENSITY, 2);  // Lower intensity for evening
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

    int displayHour = h; // Use a temporary variable for the hour to be displayed
    
    // Adjust for 12H format if needed
    if (is12HFormat) {
      isPM = displayHour >= 12; // Determine if it's PM for potential AM/PM display
      displayHour %= 12;
      displayHour = displayHour ? displayHour : 12; // Converts 0 to 12 for 12-hour format display
    }

    // Prepare the digits based on the displayHour
    dig[0] = displayHour / 10 ? displayHour / 10 : (is12HFormat ? 10 : 0); // Leading 0 for 24H format, blank for 12H
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
      dy = digHt - digtrans[i];
      showDigit(digold[i], digPos[i], dig6x8);
      dy = -digtrans[i];
      showDigit(dig[i], digPos[i], dig6x8);
      digtrans[i]--;
    }
  }
  dy = 0;
  // Handle the display of dots for seconds or AM/PM indicator
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
  timeClient.update(); // Update time with NTP server

  // Apply the utcOffset to adjust for the local timezone
  epoch = timeClient.getEpochTime() + (long)(utcOffset * 3600);
  localMillisAtUpdate = millis(); // Store the current millis() to calculate the drift

  // Convert the adjusted epoch time to tm struct for local time
  time_t rawTime = epoch;
  struct tm *ptm = gmtime(&rawTime); // Convert epoch to struct tm as UTC time

  // Update global time variables
  h = ptm->tm_hour; // 24-hour format
  m = ptm->tm_min;
  s = ptm->tm_sec;

  // Update global date variables
  year = ptm->tm_year + 1900; // Year since 1900
  month = ptm->tm_mon + 1;    // tm_mon is months since January (0-11)
  day = ptm->tm_mday;         // Day of the month

  // Log the epoch time for debugging
  Serial.println(epoch);

  return epoch;
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
  // Assuming utcOffset is correctly set from the WiFi Manager as -4
  // and considering the need to correctly apply this offset

  // Adjust epoch for the UTC offset
  //epoch += utcOffset * 3600; // This should subtract 4 hours worth of seconds from the epoch

  // Adjust for any time passed since the last NTP update to maintain accuracy
  epoch += (millis() - localMillisAtUpdate) / 1000;

  // Convert the adjusted epoch time to a tm structure for easier manipulation
  time_t adjustedEpoch = static_cast<time_t>(epoch);
  struct tm *timeinfo = gmtime(&adjustedEpoch); // Use gmtime here to work with the adjusted epoch directly

  // Update the global time variables based on the adjusted time
  h = timeinfo->tm_hour; // Should reflect the correct local time considering the utcOffset
  m = timeinfo->tm_min;
  s = timeinfo->tm_sec;

  // With is12HFormat = false, no need to convert h to 12-hour format, it should display directly as 24-hour format
  // Ensure that wherever the time is displayed, it uses h, m, s directly without further conversion

  // Debug print to verify the time after adjustment
  Serial.print("Adjusted Time: ");
  Serial.print(h);
  Serial.print(":");
  Serial.print(m);
  Serial.println(" (24-hour format)");
}





