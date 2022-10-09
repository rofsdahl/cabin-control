#define DEBUG

#ifdef DEBUG
#define DEBUG_BEGIN(...)  Serial.begin(__VA_ARGS__)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_BEGIN(...)  // Blank line - no code
#define DEBUG_PRINTF(...) // Blank line - no code
#endif

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

#define LEFT_BUTTON_PIN    0
#define RIGHT_BUTTON_PIN  35
#define SENSOR_PIN        25
#define TX_PIN            26

#define DELAY_STARTUP          4000
#define DELAY_NEXT_DISPLAY     1000
#define DELAY_NEXA_TRANSMIT    5000
#define DELAY_REVERSE_SYNC     5000
#define TIMEOUT_WIFI_CONNECT  10000
#define TIMEOUT_HTTP          20000

Preferences preferences;
TFT_eSPI tft = TFT_eSPI();
OneWire oneWire(SENSOR_PIN);
DallasTemperature dallas(&oneWire);
NexaTx nexaTx = NexaTx(TX_PIN);
Backlight backlight;

Activity activity = NONE;
byte setMode;
byte setTemp;
byte prevTemp;
byte minTemp[] = { 5, 16};
byte maxTemp[] = {15, 25};
int syncInterval = 60;
bool reverseSyncPending = false;
bool savePrefsPending = false;
unsigned long tNextNexaUpdate = 0;
unsigned long tNextSync = 0;
unsigned long tNextTokenRefresh = 0;
unsigned long tNextTimeAdjustment = 0;
unsigned long tLastSync = 0;
unsigned long tNextDisplay = 0;
String accessToken = "";
int errCount = 0;
int numZones = sizeof(zones) / sizeof(zones[0]);

////////////////////////////////////////////////////////////////////////////////
void enableWiFi() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  delay(200);

  DEBUG_PRINTF("Connecting to WiFi...\n");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long tTimeout = millis() + TIMEOUT_WIFI_CONNECT;
  while (millis() < tTimeout) {
    if (WiFi.status() == WL_CONNECTED) {
      DEBUG_PRINTF("-> IP address: %s\n", WiFi.localIP().toString());
      return;
    }
    delay(100);
  }
  DEBUG_PRINTF("-> FAILED!\n");
}

void disableWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  DEBUG_PRINTF("WiFi disconnected!\n");
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

time_t lastSundayOf(int yearOffset, int month) {
  struct tm timeinfo;
  timeinfo.tm_year = yearOffset;
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
  DEBUG_PRINTF("NTP sync...\n");
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
    if (zones[i].type == AUTO || zones[i].type == SENSOR) {
      numTempZones++;
    }
  }
  return numTempZones;
}

int getTempZone(int n) {
  int t = 0;
  for (int i = 0; i < numZones; i++) {
    if (zones[i].type == AUTO || zones[i].type == SENSOR) {
      if (n == t) return i;
      else t++;
    }
  }
  return -1;
}

void restorePreferences() {
  DEBUG_PRINTF("Restore preferences...\n");
  // TODO: Check for legal values (temp 5-25, manual 0-1)?
  for (int i = 0; i < numZones; i++) {
    zones[i].value = preferences.getUChar(zones[i].name, 0);
    DEBUG_PRINTF("<- Zone %s: %d\n", zones[i].name, zones[i].value);
  }
  prevTemp = preferences.getUChar("prevTemp", 0);
  DEBUG_PRINTF("<- prevTemp: %d\n", prevTemp);

  setTemp = zones[0].value;
  setMode = setTemp >= minTemp[1] ? 1 : 0;
}

void savePreferences() {
  DEBUG_PRINTF("Save preferences...\n");
  for (int i = 0; i < numZones; i++) {
    DEBUG_PRINTF("-> Zone %s: %d\n", zones[i].name, zones[i].value);
    preferences.putUChar(zones[i].name, zones[i].value);
  }
  DEBUG_PRINTF("-> prevTemp: %d\n", prevTemp);
  preferences.putUChar("prevTemp", prevTemp);
}

////////////////////////////////////////////////////////////////////////////////

void readTemperatures() {
  dallas.requestTemperatures();
  for (int i = 0; i < numZones; i++) {
    if (zones[i].type == AUTO || zones[i].type == SENSOR) {
      zones[i].temp = dallas.getTempC(zones[i].sensorAddr);
    }
  }
}

void updateNexas() {
  DEBUG_PRINTF("Update Nexas...\n");
  for (int i = 0; i < numZones; i++) {
    bool newState = zones[i].state;
    if (zones[i].type == AUTO) {
      float tempOffset = zones[i].state ? 0.1 : -0.1;
      newState = zones[i].temp < (zones[i].value + tempOffset);
    }
    if (zones[i].type == MANUAL) {
      newState = zones[i].value > 0;
    }

    // Start / stop on-timer
    if (!zones[i].state && newState) {
      zones[i].tOn = millis();
      DEBUG_PRINTF(".. Zone %s ON at %d, accumulated = %d\n", zones[i].name, zones[i].tOn, zones[i].tAccu);
    }
    if (zones[i].state && !newState) {
      unsigned long tPeriod = millis() - zones[i].tOn;
      zones[i].tAccu += tPeriod;
      DEBUG_PRINTF(".. Zone %s OFF at %d, period = %d, accumulated = %d\n", zones[i].name, millis(), tPeriod, zones[i].tAccu);
    }

    // Update state
    zones[i].state = newState;

    // Transmit current zone state for all Nexa power plugs
    for (int j = 0; j < NEXAS_PER_ZONE; j++) {
      if (zones[i].nexas[j].type != 0) {
        DEBUG_PRINTF("-> Zone %s (%c): %d\n", zones[i].name, zones[i].nexas[j].type, zones[i].state);
        nexaTx.transmit(zones[i].nexas[j].type, zones[i].nexas[j].id, zones[i].state);
      }
    }
  }
}

void synchronizeWithRemote(bool reverseSync) {
  DEBUG_PRINTF("Synchronize with remote...\n");

  unsigned long tNow = millis();

  //TODO: Support using dev script also (is it always valid?)
  String url = "https://script.google.com/macros/s/" APPS_SCRIPT_ID "/exec?uptime=";
  url += getUptimeMillis() / 1000;

  url += "&errorCount=";
  url += errCount;

  for (int i = 0; i < numZones; i++) {

    // Accumulate on-timer, calculate and report on-ratio, reset on-timer
    if (zones[i].state) {
      unsigned long tPeriod = tNow - zones[i].tOn;
      zones[i].tAccu += tPeriod;
      zones[i].tOn = tNow;
    }
    float dutyCycle = (float)zones[i].tAccu / (tNow - tLastSync);

    char zoneBuf[40];
    snprintf(zoneBuf, sizeof(zoneBuf), "%s;%c;%.1f;%d;%.2f",
             zones[i].name,
             zones[i].type,
             zones[i].temp,
             // Currently reverse syncing only zone 0
             (reverseSync && i == 0) ? zones[i].value : -1,
             dutyCycle);
    url += "&zone=";
    url += urlEncode(zoneBuf);
    DEBUG_PRINTF("-> Zone %s (%d/(%d-%d))\n", zoneBuf, zones[i].tAccu, tNow, tLastSync);

    // Reset on-timer should ideally be done AFTER successful sync
    zones[i].tAccu = 0;
  }

  tLastSync = tNow;

  int responseCode = 0;
  int maxRedirects = 3;

  do {
    HTTPClient http;
    http.begin(url.c_str());
    DEBUG_PRINTF("HTTP request: %s\n", url.c_str());
    const char * headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(char*));
    http.setTimeout(TIMEOUT_HTTP);
    responseCode = http.GET();
    DEBUG_PRINTF("HTTP response code: %d\n", responseCode);

    if (responseCode == 200) {
      String payload = http.getString();
      DEBUG_PRINTF("HTTP response: %s\n", payload.c_str());
      http.end();

      JSONVar json = JSON.parse(payload);
      if (JSON.typeof(json) == "object") {

        for (int i = 0; i < numZones; i++) {
          if (zones[i].type != SENSOR) {
            // TODO: Check for legal values (temp 5-25, manual 0-1)?
            // TODO: Fix invalid values, and sync back (set reverseSync=true)
            int value = (int)json[String("zone.") + zones[i].name];
            DEBUG_PRINTF("<- Zone %s: %d (was: %d)\n", zones[i].name, value, zones[i].value);
            if (value != zones[i].value) {
              zones[i].value = value;
              savePrefsPending = true;
            }
            if (i == 0) {
              setTemp = zones[0].value;
              setMode = setTemp >= minTemp[1] ? 1 : 0;
            }
          }
        }

        int value = (int)json["syncInterval"];
        DEBUG_PRINTF("<- Sync Interval: %d\n", value);
        if (value > 0) {
          syncInterval = value;
        }
      }
    }

    if (responseCode == 302) {
      url = http.header("Location");
    }

    if (responseCode < 0) {
      errCount++;
    }

    http.end();

  } while (responseCode == 302 && maxRedirects-- > 0);
}

void controlTask(void *params) {
  int dayMinDone = -1;

  while (true) {
    if (savePrefsPending) {
      savePreferences();
      savePrefsPending = false;
    }

    readTemperatures();

    if (millis() > tNextNexaUpdate) {
      activity = NEXA;
      updateNexas();
      tNextNexaUpdate = millis() + 5000 * 60 * 1000; // 5 min
      activity = NONE;
    }

    struct tm timeinfo;
    getLocalTime(&timeinfo, 100);
    int dayMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    bool doScheduledSync = dayMin % syncInterval == 0 && dayMin != dayMinDone;
    if (doScheduledSync || millis() > tNextSync) {

      activity = WIFI;
      enableWiFi();
      activity = NONE;

      if (WiFi.status() == WL_CONNECTED) {

        // NTP sync at 03:00
        if (dayMin == 3 * 60 || millis() > tNextTimeAdjustment) {
          activity = TIME;
          adjustTime();
          tNextTimeAdjustment = millis() + 24 * 60 * 60 * 1000; // 24 hours
          activity = NONE;
        }

        activity = SYNC;
        synchronizeWithRemote(reverseSyncPending);
        reverseSyncPending = false;
        dayMinDone = dayMin;
        tNextSync = millis() + syncInterval * 60 * 1000;

        // Avoid too frequent Nexa updates (pilot signal confusion)
        tNextNexaUpdate = millis() + DELAY_NEXA_TRANSMIT;
        activity = NONE;
      }

      disableWiFi();
    }

    vTaskDelay(10);
  }
}

////////////////////////////////////////////////////////////////////////////////

void triggerReverseSync() {
  reverseSyncPending = true;
  // Avoid too rapid Nexa updates (pilot signal confusion)
  tNextNexaUpdate = millis() + DELAY_REVERSE_SYNC;
  tNextSync = millis() + DELAY_REVERSE_SYNC;
}

void toggleMode(Button2& btn) {
  if (!backlight.isBright()) {
    backlight.setBright();
  } else {
    setMode = (setMode + 1) % 2;
    setTemp = prevTemp;
    prevTemp = zones[0].value;
    zones[0].value = setTemp;

    tNextDisplay = 0;
    triggerReverseSync();
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

    tNextDisplay = 0;
    triggerReverseSync();
    savePrefsPending = true;
  }
}

void buttonTask(void *params) {
  Button2 leftBtn = Button2(LEFT_BUTTON_PIN);
  leftBtn.setPressedHandler(toggleMode);

  Button2 rightBtn = Button2(RIGHT_BUTTON_PIN);
  rightBtn.setPressedHandler(increaseTemp);

  while (true) {
    backlight.loop();
    leftBtn.loop();
    rightBtn.loop();
    vTaskDelay(1);
  }
}

////////////////////////////////////////////////////////////////////////////////

void displayTask(void *params) {
  tft.fillScreen(TFT_BLACK);
  backlight.begin(TFT_BL, 8, 20, 255, 60 * 1000); // 60 sec

  // TODO: Display UTF-8 characters?
  while (true) {
    if (millis() > tNextDisplay) {

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
        if (zones[i].type == AUTO || zones[i].type == MANUAL) {
          if (zones[i].state != zones[i].lastDisplayedState || tNextDisplay == 0) {
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

      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setTextDatum(TR_DATUM);
      x = tft.drawString(text, 135, y, 2);
      tft.fillRect(100, y, 35 - x, 16, TFT_BLACK);

      tNextDisplay = millis() + DELAY_NEXT_DISPLAY;
    }

    vTaskDelay(1);
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

    DEBUG_PRINTF("Sensor %d: 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X\n",
                 i + 1,
                 address[0], address[1], address[2], address[3],
                 address[4], address[5], address[6], address[7]);
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
  tft.drawChar(zones[selectedZone].nexas[selectedNexa].type, 0, 0, 4);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  char buf[5];
  if (zones[selectedZone].nexas[selectedNexa].type == LEARN) {
    snprintf(buf, sizeof(buf), "%04X", (zones[selectedZone].nexas[selectedNexa].id >> 4*4) & 0x0000FFFF);
    tft.drawString(buf, 20, -3, 2);
    snprintf(buf, sizeof(buf), "%04X", (zones[selectedZone].nexas[selectedNexa].id >> 0*4) & 0x0000FFFF);
    tft.drawString(buf, 20, 10, 2);
  }
  else {
    snprintf(buf, sizeof(buf), "%02X", zones[selectedZone].nexas[selectedNexa].id & 0x000000FF);
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
  while (digitalRead(LEFT_BUTTON_PIN) == LOW || digitalRead(RIGHT_BUTTON_PIN) == LOW);
  delay(50); // Debounce delay

  int numMenuItems = sizeof(menuItems) / sizeof(menuItems[0]);
  int numNexas = 0;
  for (int i = 0; i < numZones; i++) {
    for (int j = 0; j < NEXAS_PER_ZONE; j++) {
      if (zones[i].nexas[j].type != 0) {
        numNexas++;
      }
    }
  }
  DEBUG_PRINTF("Selected Nexa: %s/%d\n", zones[selectedZone].name, selectedNexa);

  while (true) {
    if (digitalRead(LEFT_BUTTON_PIN) == LOW) {
      delay(50); // Debounce delay
      selectedMenuItem = (selectedMenuItem + 1) % numMenuItems;
      displayMenuItems();
      while (digitalRead(LEFT_BUTTON_PIN) == LOW);
      delay(50); // Debounce delay
    }

    if (digitalRead(RIGHT_BUTTON_PIN) == LOW) {
      delay(50); // Debounce delay

      switch (selectedMenuItem) {
        case 0: {
            selectedNexa++;
            while (selectedNexa >= NEXAS_PER_ZONE || zones[selectedZone].nexas[selectedNexa].type == 0) {
              selectedNexa = 0;
              selectedZone = (selectedZone + 1) % numZones;
            }
            DEBUG_PRINTF("Selected Nexa: %s/%d\n", zones[selectedZone].name, selectedNexa);
            displaySelectedNexa();
            while (digitalRead(RIGHT_BUTTON_PIN) == LOW);
            break;
          }
        case 1:
        case 2:
        case 3: {
            while (digitalRead(RIGHT_BUTTON_PIN) == LOW) {
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
              nexaTx.transmit(zones[selectedZone].nexas[selectedNexa].type,
                              zones[selectedZone].nexas[selectedNexa].id,
                              activation, 2);
            }
            displaySelectedNexa();
            break;
          }
        case 4: {
            displaySensors();
            while (digitalRead(RIGHT_BUTTON_PIN) == LOW);
            tft.fillScreen(TFT_BLACK);
            displayMenuItems();
            displaySelectedNexa();
            break;
          }
        default: {
            while (digitalRead(RIGHT_BUTTON_PIN) == LOW);
            return;
          }
      }

      while (digitalRead(RIGHT_BUTTON_PIN) == LOW);
      delay(50); // Debounce delay
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void setup() {
  DEBUG_BEGIN(115200);

  dallas.begin();
  tft.init();
  tft.setRotation(0);
  tft.setTextSize(1);

  btStop();
  disableWiFi();

  pinMode(LEFT_BUTTON_PIN, INPUT);
  pinMode(RIGHT_BUTTON_PIN, INPUT);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("ESP32 Temp Control", 0, 0 * 13, 2);
  tft.drawString(__DATE__, 0, 1 * 13, 2);
  tft.drawString(__TIME__, 0, 2 * 13, 2);
  tft.drawString("Press for menu...", 0, 4 * 13, 2);

  unsigned long timeout = millis() + DELAY_STARTUP;
  while (millis() < timeout) {
    tft.drawNumber((timeout - millis()) / 1000, 0, 5 * 13, 2);
    if (digitalRead(LEFT_BUTTON_PIN) == LOW || digitalRead(RIGHT_BUTTON_PIN) == LOW) {
      menuSystem();
    }
  }

  preferences.begin("temp");
  restorePreferences();

  DEBUG_PRINTF("Setup finished - creating tasks...\n");
  xTaskCreatePinnedToCore(displayTask, "displayTask", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask, "buttonTask", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(controlTask, "controlTask", 10000, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelay(1);
}
