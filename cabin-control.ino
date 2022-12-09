#include <Preferences.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HTTPClient.h>
#include <UrlEncode.h>
#include <Arduino_JSON.h>
#include "Button2.h"
#include "time.h"

#include "config.h"
#include "typedef.h"
#include "nexa-tx.h"
#include "backlight.h"

#define PIN_LEFT_BUTTON    0
#define PIN_RIGHT_BUTTON  35
#define PIN_SENSOR        25
#define PIN_TX            26

#ifdef DEBUG
#define DEBUG_BEGIN(...)   Serial.begin(__VA_ARGS__)
#define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_BEGIN(...)   // Blank line - no code
#define DEBUG_PRINTF(...)  // Blank line - no code
#define DEBUG_PRINTLN(...) // Blank line - no code
#endif

#define SEC               (1000UL * 1)
#define MIN               (1000UL * 60)
#define HOUR              (1000UL * 60 * 60)

Preferences preferences;
TFT_eSPI tft = TFT_eSPI();
OneWire oneWire(PIN_SENSOR);
DallasTemperature dallas(&oneWire);
NexaTx nexaTx = NexaTx(PIN_TX);
Backlight backlight;

enum Activity { NONE, NEXA, WIFI, TIME, SYNC };
Activity activity = NONE;
byte setMode;
byte setTemp;
byte prevTemp;
int syncIntervalMin = 60;
bool doReverseSync = false;
bool savePrefsPending = false;
bool wifiSuccess = true;
int errCount = 0;
unsigned long tLastTempRead = 0;
unsigned long tLastNexaUpdate = 0;
unsigned long tLastNtpSync = 0;
unsigned long tLastSync = 0;
unsigned long tLastReport = 0;
unsigned long tLastDisplayUpdate = 0;
unsigned long tLastWifiFailure = 0;

const byte minTemp[] = { 5, 16};
const byte maxTemp[] = {15, 25};
const int numZones = sizeof(zones) / sizeof(zones[0]);
const unsigned long displayUpdateInterval = 1 * SEC;
const unsigned long tempReadInterval = 5 * SEC;
const unsigned long nexaUpdateInterval = 5 * MIN;
const unsigned long ntpSyncInterval = 24 * HOUR;
const unsigned long displayDimTimeout = 1 * MIN;
const unsigned long keypressTimeout = 5 * SEC;
const unsigned long wifiConnectTimeout = 10 * SEC;
const unsigned long wifiRetryTimeout = 1 * MIN;
const unsigned long httpResponseTimeout = 20 * SEC;

////////////////////////////////////////////////////////////////////////////////
void enableWiFi() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  delay(200);

  DEBUG_PRINTLN("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long tStart = millis();
  while (millis() - tStart < wifiConnectTimeout) {
    if (WiFi.status() == WL_CONNECTED) {
      DEBUG_PRINTF("-> IP address: %s\n", WiFi.localIP().toString());
      tLastWifiFailure = millis() - wifiRetryTimeout;
      wifiSuccess = true;
      return;
    }
    delay(100);
  }
  DEBUG_PRINTLN("-> FAILED!");
  tLastWifiFailure = millis();
  wifiSuccess = false;
}

void disableWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  DEBUG_PRINTLN("WiFi disconnected!");
}

////////////////////////////////////////////////////////////////////////////////

unsigned long getUptimeMillis() {
  return (int64_t)esp_timer_get_time() / 1000;
}

String toDurationStr(unsigned long ms) {
  unsigned long t = ms / 1000;
  if (t < 60)
    return (String)t + "s";
  t /= 60;
  if (t < 60)
    return (String)t + "m";
  t /= 60;
  if (t < 24)
    return (String)t + "h";
  t /= 24;
  return (String)t + "d";
}

uint8_t weekday(time_t unixTime) {
  // Calculate day of week Sun-Sat as an integer 0-6
  // 1970-01-01 was a Thursday (4)
  return ((unixTime / (24 * 60 * 60)) + 4) % 7;
}

time_t lastSundayOf(int year, int month) {
  struct tm timeinfo;
  timeinfo.tm_year = year;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = 31;
  timeinfo.tm_hour = 2;
  timeinfo.tm_sec = 0;
  timeinfo.tm_min = 0;
  time_t lastDayOfMonth = mktime(&timeinfo);
  timeinfo.tm_mday = 31 - weekday(lastDayOfMonth);
  return mktime(&timeinfo);
}

int calculateDstOffset() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  time_t now = mktime(&timeinfo);
  time_t dstBegin = lastSundayOf(timeinfo.tm_year, 3);
  time_t dstEnd = lastSundayOf(timeinfo.tm_year, 10);
  return (now > dstBegin && now < dstEnd) ? 1 : 0;
}

void adjustTime() {
  DEBUG_PRINTLN("NTP sync...");
  static int dstOffset = 0;
  int lastDstOffset;
  do {
    const int timezone = 1;
    DEBUG_PRINTF("-> DST %d\n", dstOffset);
    configTime(timezone * 60 * 60, dstOffset * 60 * 60, "pool.ntp.org", "time.nist.gov");
    lastDstOffset = dstOffset;
    dstOffset = calculateDstOffset();

  } while (dstOffset != lastDstOffset);
}

////////////////////////////////////////////////////////////////////////////////

int getNumTempZones() {
  int numTempZones = 0;
  for (int i = 0; i < numZones; i++) {
    if (zones[i].sensorId != 0) {
      numTempZones++;
    }
  }
  return numTempZones;
}

int getTempZone(int n) {
  int t = 0;
  for (int i = 0; i < numZones; i++) {
    if (zones[i].sensorId != 0) {
      if (n == t) return i;
      else t++;
    }
  }
  return -1;
}

byte normalizeValue(byte value, byte minValue, byte maxValue) {
  if (value < minValue)
    return minValue;
  else if (value > maxValue)
    return maxValue;
  else
    return value;
}

byte normalizeValue(byte value, int zone) {
  byte minValue = zones[zone].sensorId != 0 ? minTemp[0] : 0;
  byte maxValue = zones[zone].sensorId != 0 ? maxTemp[1] : 1;
  return normalizeValue(value, minValue, maxValue);
}

void restorePreferences() {
  DEBUG_PRINTLN("Restore preferences...");

  for (int i = 0; i < numZones; i++) {
    if (zones[i].nexas[0] != 0) {
      byte rawValue = preferences.getUChar(zones[i].name, 0);
      zones[i].value = normalizeValue(rawValue, i);
      if (zones[i].value != rawValue) {
        savePrefsPending = true;
      }
      DEBUG_PRINTF("<- Zone %s: %d (raw: %d)\n", zones[i].name, zones[i].value, rawValue);
    }
  }

  setTemp = zones[0].value;
  setMode = setTemp >= minTemp[1] ? 1 : 0;

  byte prevMode = (setMode + 1) % 2;
  byte rawValue = preferences.getUChar("prevTemp", 0);
  prevTemp = normalizeValue(rawValue, minTemp[prevMode], maxTemp[prevMode]);
  if (prevTemp != rawValue) {
    savePrefsPending = true;
  }
  DEBUG_PRINTF("<- prevTemp: %d (raw: %d)\n", prevTemp, rawValue);
}

void savePreferences() {
  DEBUG_PRINTLN("Save preferences...");
  for (int i = 0; i < numZones; i++) {
    if (zones[i].nexas[0] != 0) {
      DEBUG_PRINTF("-> Zone %s: %d\n", zones[i].name, zones[i].value);
      preferences.putUChar(zones[i].name, zones[i].value);
    }
  }
  DEBUG_PRINTF("-> prevTemp: %d\n", prevTemp);
  preferences.putUChar("prevTemp", prevTemp);
}

////////////////////////////////////////////////////////////////////////////////

void readTemperatures() {
  //DEBUG_PRINTLN("Read temperatures...");
  dallas.requestTemperatures();
  for (int i = 0; i < numZones; i++) {
    if (zones[i].sensorId != 0) {
      DeviceAddress deviceAddr;
      uint64_t sensorId = zones[i].sensorId;
      for (int j = 7; j >= 0; j--) {
        deviceAddr[j] = sensorId & 0x00000000000000FF;
        sensorId >>= 8;
      }
      zones[i].temp = dallas.getTempC(deviceAddr);
      //DEBUG_PRINTF("<- %s (0x%016llX): %.2f\n", zones[i].name, zones[i].sensorId, zones[i].temp);
    }
  }
}

void updateNexas() {
  DEBUG_PRINTLN("Update Nexas...");
  for (int i = 0; i < numZones; i++) {
    bool newState = zones[i].state;
    if (zones[i].sensorId != 0) {
      float tempOffset = zones[i].state ? 0.1 : -0.1;
      newState = zones[i].temp < (zones[i].value + tempOffset);
    }
    else {
      newState = zones[i].value > 0;
    }

    // Start / stop on-timer
    unsigned long tNow = millis();
    if (!zones[i].state && newState) {
      zones[i].tOn = tNow;
      DEBUG_PRINTF(".. Zone %s ON at %d, accumulated = %d\n", zones[i].name, zones[i].tOn, zones[i].tAccu);
    }
    if (zones[i].state && !newState) {
      unsigned long tPeriod = tNow - zones[i].tOn;
      zones[i].tAccu += tPeriod;
      DEBUG_PRINTF(".. Zone %s OFF at %d, period = %d, accumulated = %d\n", zones[i].name, tNow, tPeriod, zones[i].tAccu);
    }

    // Update state
    zones[i].state = newState;

    // Transmit current zone state for all Nexa power plugs
    for (int j = 0; j < NEXAS_PER_ZONE; j++) {
      if (zones[i].nexas[j] != 0) {
        DEBUG_PRINTF("-> Zone %s (Nexa %d): %d\n", zones[i].name, j + 1, zones[i].state);

        NexaType type = SIMPLE;
        if (zones[i].nexas[j] > 0x00000100) type = HE35;
        if (zones[i].nexas[j] > 0x00000200) type = LEARN;
        unsigned long id = zones[i].nexas[j] & (type == LEARN ? 0xFFFFFFFF : 0x000000FF);
        nexaTx.transmit(type, id, zones[i].state);
      }
    }
  }
}

void synchronizeWithRemote() {
  DEBUG_PRINTLN("Synchronize with remote...");
  unsigned long tNow = millis();

  String url = "https://script.google.com/macros/s/" APPS_SCRIPT_ID "/exec?uptime=";
  url += getUptimeMillis() / 1000;
  url += "&errorCount=";
  url += errCount;

  for (int i = 0; i < numZones; i++) {

    // TODO: Calculate duty cycle for all zones with sensor and Nexas
    int dutyCyclePercent;
    if (i == 0) {
      // Accumulate on-timer, calculate and report duty cycle, reset on-timer
      if (zones[i].state) {
        unsigned long tPeriod = tNow - zones[i].tOn;
        zones[i].tAccu += tPeriod;
        zones[i].tOn = tNow;
      }
      // TODO: Integer division with rounding (995/1000 should be 100%)
      dutyCyclePercent = zones[i].tAccu * 100 / (tNow - tLastReport);
      DEBUG_PRINTF(".. Zone %s duty cycle = %d %% (%d*100/(%d-%d))\n", zones[i].name, dutyCyclePercent, zones[i].tAccu, tNow, tLastReport);
      zones[i].tAccu = 0;
    }

    // Zone:<name>;<value|blank|NA>;<temp|blank>;<duty cycle|blank>
    String zoneParam = urlEncode(zones[i].name) + ";";

    // Reverse sync value, blank if no reverse sync or "NA" for zone without Nexas
    if (zones[i].nexas[0] == 0)
      zoneParam += "NA";
    // TODO: Support reverse sync of all zones? (may overwrite cell formulas)
    else if (doReverseSync && i == 0)
      zoneParam += zones[i].value;
    zoneParam += ";";

    if (zones[i].sensorId != 0)
      zoneParam += String(zones[i].temp, 1);
    zoneParam += ";";

    // TODO: Report duty cycle for all zones with sensor and Nexas
    if (i == 0)
      zoneParam += dutyCyclePercent;

    DEBUG_PRINTF("-> Zone %s\n", zoneParam.c_str());
    url += "&zone=" + zoneParam;
  }

  tLastReport = tNow;
  doReverseSync = false;

  int responseCode = 0;
  int maxRedirects = 3;

  do {
    HTTPClient http;
    http.begin(url.c_str());
    DEBUG_PRINTF("HTTP request: %s\n", url.c_str());
    const char * headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(char*));
    http.setTimeout(httpResponseTimeout);
    responseCode = http.GET();
    DEBUG_PRINTF("HTTP response code: %d\n", responseCode);

    if (responseCode == 200) {
      String payload = http.getString();
      DEBUG_PRINTF("HTTP response: %s\n", payload.c_str());
      http.end();

      JSONVar json = JSON.parse(payload);
      if (JSON.typeof(json) == "object") {

        for (int i = 0; i < numZones; i++) {
          // Skip zones without Nexas
          if (zones[i].nexas[0] == 0)
            continue;
          if (json[String("zone.") + zones[i].name] == null)
            continue;

          int rawValue = (int)json[String("zone.") + zones[i].name];
          int value = normalizeValue(rawValue, i);
          if (rawValue != value) {
            // Value normalized - trigger another sync to update sheet
            tLastSync = millis() - syncIntervalMin * MIN;
            doReverseSync = true;
          }

          DEBUG_PRINTF("<- Zone %s: %d (raw: %d, prev: %d)\n", zones[i].name, value, rawValue, zones[i].value);

          // Set new value, save prefs and trigger update of Nexas
          if (value != zones[i].value) {
            zones[i].value = value;
            savePrefsPending = true;
            tLastNexaUpdate = millis() - nexaUpdateInterval;
          }
          if (i == 0) {
            byte currTemp = setTemp;
            byte currMode = setMode;
            setTemp = zones[0].value;
            setMode = setTemp >= minTemp[1] ? 1 : 0;
            if (setMode != currMode) {
              prevTemp = currTemp;
            }
          }
        }

        int value = (int)json["syncInterval"];
        DEBUG_PRINTF("<- Sync Interval: %d\n", value);
        if (value > 0) {
          syncIntervalMin = value;
        }
      }
    }

    if (responseCode == 302) {
      url = http.header("Location");
    }

    // TODO: Improve error handling. Have seen -1 and -11, meaning what? Timeout?
    // TODO: 4xx from Google should also be handled
    // TODO: Display error status on device
    // TODO: Add reference to HTTPClient doc
    if (responseCode < 0) {
      errCount++;
    }

    http.end();

  } while (responseCode == 302 && maxRedirects-- > 0);
}

void controlTask(void *params) {

  tLastTempRead = millis() - tempReadInterval;
  tLastNexaUpdate = millis() - nexaUpdateInterval;
  tLastNtpSync = millis() - ntpSyncInterval;
  tLastSync = millis() - syncIntervalMin * MIN;
  tLastWifiFailure = millis() - wifiRetryTimeout;

  int dayMinDone = -1;

  while (true) {
    vTaskDelay(10);

    if (savePrefsPending) {
      savePrefsPending = false;
      savePreferences();
    }

    if (millis() - tLastTempRead >= tempReadInterval) {
      tLastTempRead = millis();
      readTemperatures();
    }

    if (millis() - tLastNexaUpdate >= nexaUpdateInterval) {
      activity = NEXA;
      tLastNexaUpdate = millis();
      updateNexas();
      activity = NONE;
    }

    struct tm timeinfo;
    getLocalTime(&timeinfo, 100);
    int dayMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    bool doScheduledSync = dayMin % syncIntervalMin == 0 && dayMin != dayMinDone;

    if (millis() - tLastWifiFailure < wifiRetryTimeout) {
      continue;
    }

    if (doScheduledSync || millis() - tLastSync >= syncIntervalMin * MIN) {

      activity = WIFI;
      enableWiFi();
      activity = NONE;

      if (WiFi.status() == WL_CONNECTED) {

        // NTP sync at 03:00
        if (dayMin == 3 * 60 || millis() - tLastNtpSync >= ntpSyncInterval) {
          activity = TIME;
          tLastNtpSync = millis();
          adjustTime();
          activity = NONE;
        }

        activity = SYNC;
        tLastSync = millis();
        dayMinDone = dayMin;
        synchronizeWithRemote();
        activity = NONE;
      }

      disableWiFi();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void toggleMode(Button2& btn) {
  if (!backlight.isBright()) {
    backlight.setBright();
  } else {
    setMode = (setMode + 1) % 2;
    setTemp = prevTemp;
    prevTemp = zones[0].value;
    zones[0].value = setTemp;

    // Trigger update of display, Nexas and remote sync
    tLastDisplayUpdate = millis() - displayUpdateInterval;
    tLastNexaUpdate = millis() - nexaUpdateInterval + keypressTimeout;
    tLastSync = millis() - syncIntervalMin * MIN + keypressTimeout;
    doReverseSync = true;
    savePrefsPending = true;
  }
}

void increaseTemp(Button2& btn) {
  if (!backlight.isBright()) {
    backlight.setBright();
  } else {
    setTemp++;
    if (setTemp > maxTemp[setMode]) {
      setTemp = minTemp[setMode];
    }
    zones[0].value = setTemp;

    // Trigger update of display, Nexas and remote sync
    tLastDisplayUpdate = millis() - displayUpdateInterval;
    tLastNexaUpdate = millis() - nexaUpdateInterval + keypressTimeout;
    tLastSync = millis() - syncIntervalMin * MIN + keypressTimeout;
    doReverseSync = true;
    savePrefsPending = true;
  }
}

void buttonTask(void *params) {
  Button2 leftBtn = Button2(PIN_LEFT_BUTTON);
  leftBtn.setPressedHandler(toggleMode);

  Button2 rightBtn = Button2(PIN_RIGHT_BUTTON);
  rightBtn.setPressedHandler(increaseTemp);

  while (true) {
    vTaskDelay(1);

    backlight.loop();
    leftBtn.loop();
    rightBtn.loop();
  }
}

////////////////////////////////////////////////////////////////////////////////

void displayTask(void *params) {
  tft.fillScreen(TFT_BLACK);
  backlight.begin(TFT_BL, 8, 20, 255, displayDimTimeout);
  bool isFirstDisplay = true;

  while (true) {
    vTaskDelay(1);

    if (millis() - tLastDisplayUpdate >= displayUpdateInterval) {
      tLastDisplayUpdate = millis();

      struct tm timeinfo;
      getLocalTime(&timeinfo, 100);
      char buf[20];
      strftime(buf, sizeof(buf), "  %H:%M  ", &timeinfo);

      // Time
      int y = 0;
      tft.setTextDatum(TC_DATUM);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString(buf, 67, y, 6);

      // Date
      y = 48;
      strftime(buf, sizeof(buf), "  %d. %b %y  ", &timeinfo);
      tft.drawString(buf, 67, y, 4);
      tft.drawLine(0, y + 27, 135, y + 27, TFT_LIGHTGREY);

      // Main temp
      y = 82;
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      int x = tft.drawFloat(zones[0].temp, 1, 67, y, 6);
      tft.fillRect(0, y, 67 - x / 2, 48, TFT_BLACK);
      tft.fillRect(67 + x / 2, y, 68 - x / 2, 48, TFT_BLACK);

      // Extra temp
      y = 130;
      int numTempZones = getNumTempZones();
      if (numTempZones >= 2) {
        int i = 1 + ((millis() / 10000) % (numTempZones - 1));
        i = getTempZone(i);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        int x1 = tft.drawString(zones[i].name, 0, y, 4);
        x1 += tft.drawString(":", x1, y, 4);
        tft.setTextDatum(TR_DATUM);
        int x2 = 135 - tft.drawFloat(zones[i].temp, 1, 135, y, 4);
        tft.fillRect(x1, y, x2 - x1, 26, TFT_BLACK);
      }

      // Set temp
      y = 170;
      static byte lastDisplayedTemp = -1;
      if (setTemp != lastDisplayedTemp) {
        uint16_t col = setMode == 1 ? TFT_RED : TFT_BLUE;
        tft.setTextColor(TFT_YELLOW, col);
        tft.setTextDatum(TC_DATUM);

        int x = tft.drawNumber(setTemp, 67 + 28, y, 6);
        tft.fillRect(0, y, 67 + 28 - x / 2, 48, col);
        tft.fillRect(67 + 28 + x / 2, y, 68 - 28 - x / 2, 48, col);
        tft.fillRect(0, y - 13, 135, 13, col);
        tft.fillRect(0, y + 48, 135, 3, col);
        lastDisplayedTemp = setTemp;

        if (setMode == 1) {
          // Sun
          tft.drawLine(30 - 20, y + 18, 30 + 20, y + 18, TFT_YELLOW);
          tft.drawLine(30, y + 18 - 20, 30, y + 18 + 20, TFT_YELLOW);
          tft.drawLine(30 - 13, y + 18 - 13, 30 + 13, y + 18 + 13, TFT_YELLOW);
          tft.drawLine(30 - 13, y + 18 + 13, 30 + 13, y + 18 - 13, TFT_YELLOW);
          tft.fillCircle(30, y + 18, 14, col);
          tft.fillCircle(30, y + 18, 12, TFT_YELLOW);
        }
        else {
          // Moon
          tft.fillCircle(30, y + 18, 12, TFT_YELLOW);
          tft.fillCircle(30 - 12, y + 18 - 3, 16, col);
        }
      }

      // Zone state
      y = 223;
      tft.setTextDatum(TL_DATUM);

      int n = 0;
      for (int i = 0; i < numZones; i++) {
        if (zones[i].nexas[0] != 0) {
          // Ensure display of initial Nexa state
          if (zones[i].state != zones[i].lastDisplayedState || isFirstDisplay) {
            if (zones[i].state) {
              tft.fillCircle(n * 20 + 8, y + 8, 8, TFT_YELLOW);
              tft.setTextColor(TFT_BLACK);
              tft.drawChar(zones[i].name[0], n * 20 + 5, y, 2);
            }
            else {
              tft.fillCircle(n * 20 + 8, y + 8, 8, TFT_BLACK);
              tft.drawCircle(n * 20 + 8, y + 8, 8, TFT_YELLOW);
              tft.setTextColor(TFT_YELLOW);
              tft.drawChar(zones[i].name[0], n * 20 + 5, y, 2);
            }
            zones[i].lastDisplayedState = zones[i].state;
          }
          n++; //Next position
        }
      }

      // Activity / uptime
      y = 223;
      String text = toDurationStr(getUptimeMillis());
      if      (activity == NEXA) text = "Nexa";
      else if (activity == WIFI) text = "WiFi";
      else if (activity == TIME) text = "Time";
      else if (activity == SYNC) text = "Sync";

      tft.setTextColor(wifiSuccess ? TFT_GREEN : TFT_RED, TFT_BLACK);
      tft.setTextDatum(TR_DATUM);
      x = tft.drawString(text, 135, y, 2);
      tft.fillRect(100, y, 35 - x, 16, TFT_BLACK);
    }
    isFirstDisplay = false;
  }
}

////////////////////////////////////////////////////////////////////////////////

void displaySensors() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Sensors:", 0, 0, 4);
  tft.drawLine(0, 26, 135, 26, TFT_YELLOW);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);

  dallas.begin();
  int numSensors = dallas.getDeviceCount();

  for (int i = 0; i < numSensors; i++) {
    DeviceAddress address;
    dallas.getAddress(address, i);

    char buf[17];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X%02X%02X",
             address[0], address[1], address[2], address[3],
             address[4], address[5], address[6], address[7]);
    tft.drawString(buf, 0, 32 + i * 16, 2);
    DEBUG_PRINTF("Sensor %d: 0x%s\n", i + 1, buf);
  }
}

const char* menuItems[] = {"Select Nexa", "Turn on", "Turn off", "Turn on/off", "Sensors", "Exit"};
int selectedMenuItem = 0;
int selectedNexa = 0;
int selectedZone = 0;

void displayMenuItems() {
  int numMenuItems = sizeof(menuItems) / sizeof(menuItems[0]);
  for (int i = 0; i < numMenuItems; i++) {
    uint16_t fgCol = selectedMenuItem == i ? TFT_BLACK : TFT_LIGHTGREY;
    uint16_t bgCol = selectedMenuItem == i ? TFT_LIGHTGREY : TFT_BLACK;
    tft.setTextColor(fgCol, bgCol);
    int x = tft.drawString(menuItems[i], 0, 32 + i * 26, 4);
    tft.fillRect(x, 32 + i * 26, 135 - x, 26, bgCol);
  }
}

void displaySelectedNexa() {
  tft.fillRect(0, 0, 135, 26, TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);

  char type = 'S';
  if (zones[selectedZone].nexas[selectedNexa] > 0x00000100) type = 'H';
  if (zones[selectedZone].nexas[selectedNexa] > 0x00000200) type = 'L';
  tft.drawChar(type, 0, 0, 4);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  char buf[5];
  if (zones[selectedZone].nexas[selectedNexa] > 0x200) {
    snprintf(buf, sizeof(buf), "%04X", (zones[selectedZone].nexas[selectedNexa] >> 4*4) & 0x0000FFFF);
    tft.drawString(buf, 20, -3, 2);
    snprintf(buf, sizeof(buf), "%04X", (zones[selectedZone].nexas[selectedNexa] >> 0*4) & 0x0000FFFF);
    tft.drawString(buf, 20, 10, 2);
  }
  else {
    snprintf(buf, sizeof(buf), "%02X", zones[selectedZone].nexas[selectedNexa] & 0x000000FF);
    tft.drawString(buf, 20, 0, 4);
  }

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(zones[selectedZone].name, 55, 0, 4);
  tft.drawLine(0, 26, 135, 26, TFT_YELLOW);
}

void menuSystem() {
  tft.fillScreen(TFT_BLACK);
  displayMenuItems();
  displaySelectedNexa();
  while (digitalRead(PIN_LEFT_BUTTON) == LOW || digitalRead(PIN_RIGHT_BUTTON) == LOW);
  delay(50); // Debounce delay

  int numMenuItems = sizeof(menuItems) / sizeof(menuItems[0]);
  int numNexas = 0;
  for (int i = 0; i < numZones; i++) {
    for (int j = 0; j < NEXAS_PER_ZONE; j++) {
      if (zones[i].nexas[j] != 0) {
        numNexas++;
      }
    }
  }
  DEBUG_PRINTF("Selected Nexa: %s/%d\n", zones[selectedZone].name, selectedNexa + 1);

  while (true) {
    if (digitalRead(PIN_LEFT_BUTTON) == LOW) {
      delay(50); // Debounce delay
      selectedMenuItem = (selectedMenuItem + 1) % numMenuItems;
      displayMenuItems();
      while (digitalRead(PIN_LEFT_BUTTON) == LOW);
      delay(50); // Debounce delay
    }

    if (digitalRead(PIN_RIGHT_BUTTON) == LOW) {
      delay(50); // Debounce delay

      switch (selectedMenuItem) {
        case 0: {
            selectedNexa++;
            while (selectedNexa >= NEXAS_PER_ZONE || zones[selectedZone].nexas[selectedNexa] == 0) {
              selectedNexa = 0;
              selectedZone = (selectedZone + 1) % numZones;
            }
            DEBUG_PRINTF("Selected Nexa: %s/%d\n", zones[selectedZone].name, selectedNexa + 1);
            displaySelectedNexa();
            while (digitalRead(PIN_RIGHT_BUTTON) == LOW);
            break;
          }
        case 1:
        case 2:
        case 3: {
            while (digitalRead(PIN_RIGHT_BUTTON) == LOW) {
              bool activation;
              if (selectedMenuItem == 3) activation = !activation;
              else activation = selectedMenuItem == 1;

              if (activation) {
                tft.fillCircle(126, 10, 8, TFT_YELLOW);
              }
              else {
                tft.fillCircle(126, 10, 8, TFT_BLACK);
                tft.drawCircle(126, 10, 8, TFT_YELLOW);
              }
              NexaType type = SIMPLE;
              if (zones[selectedZone].nexas[selectedNexa] > 0x100) type = HE35;
              if (zones[selectedZone].nexas[selectedNexa] > 0x200) type = LEARN;
              unsigned long id = zones[selectedZone].nexas[selectedNexa] & (type == LEARN ? 0xFFFFFFFF : 0x000000FF);
              nexaTx.transmit(type, id, activation, 2);
            }
            displaySelectedNexa();
            break;
          }
        case 4: {
            displaySensors();
            while (digitalRead(PIN_RIGHT_BUTTON) == LOW);
            tft.fillScreen(TFT_BLACK);
            displayMenuItems();
            displaySelectedNexa();
            break;
          }
        default: {
            while (digitalRead(PIN_RIGHT_BUTTON) == LOW);
            return;
          }
      }

      while (digitalRead(PIN_RIGHT_BUTTON) == LOW);
      delay(50); // Debounce delay
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void setup() {
  DEBUG_BEGIN(115200);

  btStop();
  disableWiFi();
  dallas.begin();

  pinMode(PIN_LEFT_BUTTON, INPUT);
  pinMode(PIN_RIGHT_BUTTON, INPUT);

  // TODO: Display accented characters (æøå)?
  tft.init();
  tft.setRotation(0);
  tft.setTextSize(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Cabin Ctrl.", 0, 0, 4);
  tft.drawLine(0, 26, 135, 26, TFT_YELLOW);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(__DATE__, 0, 32, 4);
  tft.drawString(__TIME__, 0, 58, 4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("M: Menu...", 0, 110, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);

  unsigned long tStart = millis();
  while (millis() - tStart < keypressTimeout) {
    tft.drawNumber((tStart + keypressTimeout - millis()) / 1000, 0, 136, 4);
    if (digitalRead(PIN_LEFT_BUTTON) == LOW) {
      menuSystem();
    }
  }

  DEBUG_PRINTLN("*******************");
  DEBUG_PRINTLN("*** Cabin Ctrl. ***");
  DEBUG_PRINTLN("*******************");

  preferences.begin("temp");
  restorePreferences();

  DEBUG_PRINTLN("Setup finished - creating tasks...");
  xTaskCreatePinnedToCore(displayTask, "displayTask", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask, "buttonTask", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(controlTask, "controlTask", 10000, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelay(1);
}
