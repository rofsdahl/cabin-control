#include <Preferences.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HTTPClient.h>
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

Preferences preferences;
TFT_eSPI tft = TFT_eSPI();
OneWire oneWire(SENSOR_PIN);
DallasTemperature dallas(&oneWire);
NexaTx nexaTx = NexaTx(TX_PIN);
Backlight backlight;

#define ACTIVITY_NONE 0
#define ACTIVITY_NEXA 1
#define ACTIVITY_WIFI 2
#define ACTIVITY_TIME 3
#define ACTIVITY_SYNC 4

byte activity = ACTIVITY_NONE;
byte setMode;
byte setTemp;
byte minTemp[] = { 5, 16};
byte maxTemp[] = {15, 25};
int syncInterval = 60;
bool reverseSyncPending = false;
unsigned long tNextNexaUpdate = 0;
unsigned long tNextSync = 0;
unsigned long tNextTokenRefresh = 0;
unsigned long tNextTimeAdjustment = 0;
unsigned long tLastSync = 0;
unsigned long tNextDisplay = 0;
String accessToken = "";
int errCount = 0;

////////////////////////////////////////////////////////////////////////////////
void enableWiFi(){
  WiFi.disconnect(false);  // Reconnect the network
  WiFi.mode(WIFI_STA);    // Switch WiFi off
  delay(200);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long tTimeout = millis() + 10000;
  do {
    delay(1000);
  } while (WiFi.status() != WL_CONNECTED && millis() < tTimeout);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected - IP address: ");
    Serial.println(WiFi.localIP());
  }
  else {
    Serial.println("Connecting to WiFi failed!");
  }
}

void disableWiFi(){
  WiFi.disconnect(true);  // Disconnect from the network
  WiFi.mode(WIFI_OFF);    // Switch WiFi off
  Serial.println("WiFi disconnected!");
}

////////////////////////////////////////////////////////////////////////////////

unsigned long getUptimeMillis() {
  return (int64_t)esp_timer_get_time() / 1000;
}

String toDurationStr(unsigned long ms) {
  unsigned long t = ms/1000;
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
  //Calculate day of week Sun-Sat as an integer 0-6
  //1970-01-01 was a Thursday (4)
  return ((unixTime / (24*60*60)) + 4) % 7;
}

time_t lastSundayOf(int yearOffset, int month) {
  struct tm timeinfo;
  timeinfo.tm_year = yearOffset;
  timeinfo.tm_mon = month-1;
  timeinfo.tm_mday = 31;
  timeinfo.tm_hour = 2;
  timeinfo.tm_sec = 0;
  timeinfo.tm_min = 0;
  time_t lastDayOfMonth = mktime(&timeinfo);
  timeinfo.tm_mday=31-weekday(lastDayOfMonth);
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
  static int dstOffset = 0;
  int lastDstOffset;
  do {
    const int timezone = 1;
    Serial.print("Configure time with DST ");
    Serial.println(dstOffset);
    configTime(timezone*60*60, dstOffset*60*60, "pool.ntp.org", "time.nist.gov");
    lastDstOffset = dstOffset;
    dstOffset = calculateDstOffset();

  } while (dstOffset != lastDstOffset);
}

////////////////////////////////////////////////////////////////////////////////

void readTemperatures() {
  dallas.requestTemperatures();
  int numSensors = sizeof(sensor)/sizeof(sensor[0]);
  for (int s=0; s<numSensors; s++) {
    sensor[s].temp = dallas.getTempC(sensor[s].addr);
  }
}

void updateNexas() {
  int numNexas = sizeof(nexa)/sizeof(nexa[0]);
  for (int i=0; i<numNexas; i++) {

    // Nexa linked to sensor?
    if (nexa[i].linkedSensor != -1) {
      float tempThreshold = nexa[i].state ? setTemp + 0.1 : setTemp - 0.1;
      nexa[i].state = sensor[nexa[i].linkedSensor].temp < tempThreshold;
    }

    // Nexa linked to mode?
    if (nexa[i].activeInMode != -1) {
      nexa[i].state = nexa[i].activeInMode == setMode;
    }

    nexaTx.transmit(nexa[i].type, nexa[i].id, nexa[i].state);
  }
}

void synchronizeWithRemote(bool reverseSync) {
  String url = "https://script.google.com/macros/s/" APPS_SCRIPT_ID "/exec?uptime=";
  url += getUptimeMillis()/1000;

  url += "&errCount=";
  url += errCount;

  if (reverseSync) {
    url += "&setTemp=";
    url += setTemp;
  }

  int numSensors = sizeof(sensor)/sizeof(sensor[0]);
  for (int i=0; i<numSensors; i++) {
    url += "&temp=";
    url += sensor[i].name;
    url += ":";
    url += String(sensor[i].temp, 1);
  }

  int numNexas = sizeof(nexa)/sizeof(nexa[0]);
  for (int i=0; i<numNexas; i++) {
    if (nexa[i].linkedSensor == -1 && nexa[i].activeInMode == -1) {
      url += "&output=";
      url += nexa[i].name;
    }
  }
  tLastSync = millis();

  int responseCode = 0;
  int maxRedirects = 3;

  do {
    HTTPClient http;
    http.begin(url.c_str());
    const char * headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, sizeof(headerKeys)/sizeof(char*));
    http.setTimeout(20000);
    responseCode = http.GET();
    
    Serial.print("HTTP response code: ");
    Serial.println(responseCode);
    
    if (responseCode == 200) {
      String payload = http.getString();
      http.end();

      JSONVar json = JSON.parse(payload);
      if (JSON.typeof(json) == "object") {
        int value = (int)json["setTemp"];
        if (value >= minTemp[0] && value <= maxTemp[1]) {
          setTemp = value;
          setMode = setTemp >= minTemp[1] ? 1 : 0;
          preferences.putUChar("setMode", setMode);
          preferences.putUChar(String(setMode).c_str(), setTemp);
        }
        value = (int)json["syncInterval"];
        if (value > 0) {
          syncInterval = value;
        }

        for (int i=0; i<numNexas; i++) {
          if (nexa[i].linkedSensor == -1 && nexa[i].activeInMode == -1) {
            value = (int)json[String("output.")+nexa[i].name];
            nexa[i].state = value > 0;
          }
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
  Serial.print("controlTask() running on core ");
  Serial.println(xPortGetCoreID());
  int dayMinDone = -1;

  while (true) {
    readTemperatures();

    if (millis() > tNextNexaUpdate) {
      Serial.println("Update Nexas...");
      activity = ACTIVITY_NEXA;
      updateNexas();
      tNextNexaUpdate = millis() + 5*60*1000; // 5 min
      activity = ACTIVITY_NONE;
      Serial.println("Update Nexas done!");
    }

    struct tm timeinfo;
    getLocalTime(&timeinfo, 100);
    int dayMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    bool doScheduledSync = dayMin % syncInterval == 0 && dayMin != dayMinDone;
    if (doScheduledSync || millis() > tNextSync) {

      activity = ACTIVITY_WIFI;
      enableWiFi();
      activity = ACTIVITY_NONE;

      if (WiFi.status() == WL_CONNECTED) {

        if (dayMin == 3*60 || millis() > tNextTimeAdjustment) {
          Serial.println("NTP sync...");
          activity = ACTIVITY_TIME;
          adjustTime();
          tNextTimeAdjustment = millis() + 24*60*60*1000; // 24 hours
          activity = ACTIVITY_NONE;
          Serial.println("NTP sync done!");
        }

        Serial.println("Synchronize with remote...");
        activity = ACTIVITY_SYNC;
        synchronizeWithRemote(reverseSyncPending);
        reverseSyncPending = false;
        dayMinDone = dayMin;
        tNextSync = millis() + syncInterval*60*1000;
        Serial.println("Synchronize with remote done!");

        tNextNexaUpdate = millis() + 5000; //Avoid too frequent Nexa updates (pilot signal confusion)
        activity = ACTIVITY_NONE;
      }

      disableWiFi();
    }

    vTaskDelay(10);
  }
}

////////////////////////////////////////////////////////////////////////////////

void triggerReverseSync() {
  reverseSyncPending = true;
  tNextNexaUpdate = millis() + 5000; //Avoid too rapid Nexa updates (pilot signal confusion)
  tNextSync = millis() + 5000;
}

void toggleMode(Button2& btn) {
  if (!backlight.isBright()) {
    backlight.setBright();
  } else {
    setMode = (setMode + 1) % 2;
    preferences.putUChar("setMode", setMode);
    setTemp = preferences.getUChar(String(setMode).c_str(), minTemp[setMode]);
    tNextDisplay = 0;
    triggerReverseSync();
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
    preferences.putUChar(String(setMode).c_str(), setTemp);
    tNextDisplay = 0;
    triggerReverseSync();
  }
}

void buttonTask(void *params) {
  Serial.print("buttonTask() running on core ");
  Serial.println(xPortGetCoreID());

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
  Serial.print("displayTask() running on core ");
  Serial.println(xPortGetCoreID());

  tft.fillScreen(TFT_BLACK);
  backlight.begin(TFT_BL, 8, 20, 255, 60*1000); // 60 sec

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
      tft.drawLine(0, y+27, 135, y+27, TFT_LIGHTGREY);

      // Main temp
      y = 82;
      int numSensors = sizeof(sensor)/sizeof(sensor[0]);
      if (numSensors >= 1) {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        int x = tft.drawFloat(sensor[0].temp, 1, 67, y, 6);
        tft.fillRect(0,      y, 67-x/2, 48, TFT_BLACK);
        tft.fillRect(67+x/2, y, 68-x/2, 48, TFT_BLACK);
      }

      // Extra temp
      y = 130;
      if (numSensors >= 2) {
        int i = 1 + ((millis()/10000) % (numSensors-1));
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        int x1 = tft.drawString(sensor[i].name, 0, y, 4);
        x1 += tft.drawString(":", x1, y, 4);
        tft.setTextDatum(TR_DATUM);
        int x2 = 135 - tft.drawFloat(sensor[i].temp, 1, 135, y, 4);
        tft.fillRect(x1, y, x2-x1, 26, TFT_BLACK);
      }

      // Set temp
      y = 170;
      static byte lastDisplayedTemp = 0;
      if (setTemp != lastDisplayedTemp) {
        uint16_t col = setMode == 1 ? TFT_RED : TFT_BLUE;
        tft.setTextColor(TFT_YELLOW, col);
        tft.setTextDatum(TC_DATUM);
        
        int x = tft.drawNumber(setTemp, 67+28, y, 6);
        tft.fillRect(0,         y,    67+28-x/2, 48, col);
        tft.fillRect(67+28+x/2, y,    68-28-x/2, 48, col);
        tft.fillRect(0,         y-13, 135,       13, col);
        tft.fillRect(0,         y+48, 135,        3, col);
        lastDisplayedTemp = setTemp;

        if (setMode == 1) {
          // Sun
          tft.drawLine  (30-20, y+18,    30+20, y+18,    TFT_YELLOW);
          tft.drawLine  (30,    y+18-20, 30,    y+18+20, TFT_YELLOW);
          tft.drawLine  (30-13, y+18-13, 30+13, y+18+13, TFT_YELLOW);
          tft.drawLine  (30-13, y+18+13, 30+13, y+18-13, TFT_YELLOW);
          tft.fillCircle(30,    y+18,    14,             col);
          tft.fillCircle(30,    y+18,    12,             TFT_YELLOW);
        }
        else {
          // Moon
          tft.fillCircle(30,    y+18,    12,             TFT_YELLOW);
          tft.fillCircle(30-12, y+18-3,  16,             col);
        }
      }

      // Nexa state
      y = 223;
      tft.setTextDatum(TL_DATUM);

      int numNexas = sizeof(nexa)/sizeof(nexa[0]);
      for (int i=0; i<numNexas; i++) {
        if (nexa[i].state != nexa[i].lastDisplayedState || tNextDisplay == 0) {
          if (nexa[i].state) {
            tft.fillCircle(i*20+8, y+8, 8, TFT_YELLOW);
            tft.setTextColor(TFT_BLACK);
            tft.drawChar(nexa[i].name[0], i*20+5, y, 2);
          }
          else {
            tft.fillCircle(i*20+8, y+8, 8, TFT_BLACK);
            tft.drawCircle(i*20+8, y+8, 8, TFT_YELLOW);
            tft.setTextColor(TFT_YELLOW);
            tft.drawChar(nexa[i].name[0], i*20+5, y, 2);
          }
          nexa[i].lastDisplayedState = nexa[i].state;
        }
      }

      // Activity / uptime
      y = 223;
      String text = toDurationStr(getUptimeMillis());
      if      (activity == ACTIVITY_NEXA) text = "Nexa";
      else if (activity == ACTIVITY_WIFI) text = "WiFi";
      else if (activity == ACTIVITY_TIME) text = "Time";
      else if (activity == ACTIVITY_SYNC) text = "Sync";

      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setTextDatum(TR_DATUM);
      int x = tft.drawString(text, 135, y, 2);
      tft.fillRect(100, y, 35-x, 16, TFT_BLACK);

      tNextDisplay = millis() + 1000;
    }

    vTaskDelay(1);
  }
}

////////////////////////////////////////////////////////////////////////////////

const char* menuItems[] = {"Select Nexa", "Turn on", "Turn off", "Turn on/off", "Sensors", "Exit"};
int selectedMenuItem = 0;
int selectedNexa = 0;

void displayMenuItems() {
  int numMenuItems = sizeof(menuItems)/sizeof(menuItems[0]);
  for (int i=0; i<numMenuItems; i++) {
    uint16_t fgCol = selectedMenuItem==i ? TFT_BLACK : TFT_LIGHTGREY;
    uint16_t bgCol = selectedMenuItem==i ? TFT_LIGHTGREY : TFT_BLACK;
    tft.setTextColor(fgCol, bgCol);
    int x = tft.drawString(menuItems[i], 0, 32+i*26, 4);
    tft.fillRect(x, 32+i*26, 135-x, 26, bgCol);
  }
}

void displaySelectedNexa() {
  tft.fillRect(0, 0, 135, 26, TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);

  int ch;
  if (nexa[selectedNexa].type == He35)     ch='H';
  if (nexa[selectedNexa].type == Simple)   ch='S';
  if (nexa[selectedNexa].type == Learning) ch='L';
  tft.drawChar(ch, 0, -3, 2);

  int x = 10;
  int y = -3;
  for (int i=7; i>=0; i--) {
    ch = (nexa[selectedNexa].id>>i*4 & 0x0000000F);
    x += tft.drawChar(ch + (ch>9?'A'-10:'0'), x, y, 2);
    if (i==4) {
      x = 10;
      y += 13;
    }
  }

  tft.drawString(nexa[selectedNexa].name, 45, 0, 4);
  tft.drawLine(0, 26, 135, 26, TFT_YELLOW);
}

void displaySensors() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Sensors:", 0, 0, 4);
  tft.drawLine(0, 26, 135, 26, TFT_YELLOW);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);

  dallas.begin();
  int numSensors = dallas.getDeviceCount();

  for (int i=0; i<numSensors; i++) {
    DeviceAddress address;
    dallas.getAddress(address, i);

    Serial.print("Sensor ");
    Serial.print(i+1);
    Serial.print(": ");

    int x = 0;
    for (int j=0; j<8; j++) {
      char buf[5];
      snprintf(buf, sizeof(buf), "%02X", address[j]);
      x += tft.drawString(buf, x, 32+i*16, 2);
      Serial.print("0x");
      Serial.print(buf);
      if (j<7) Serial.print(", ");
    }
    Serial.println();
  }
}

void menuSystem() {
  tft.fillScreen(TFT_BLACK);
  displayMenuItems();
  displaySelectedNexa();
  while (digitalRead(LEFT_BUTTON_PIN) == LOW || digitalRead(RIGHT_BUTTON_PIN) == LOW);
  delay(50); // Debounce delay

  while (true) {
    if (digitalRead(LEFT_BUTTON_PIN) == LOW) {
      delay(50); // Debounce delay
      int numMenuItems = sizeof(menuItems)/sizeof(menuItems[0]);
      selectedMenuItem = (selectedMenuItem+1)%numMenuItems;
      displayMenuItems();
      while (digitalRead(LEFT_BUTTON_PIN) == LOW);
      delay(50); // Debounce delay
    }

    if (digitalRead(RIGHT_BUTTON_PIN) == LOW) {
      delay(50); // Debounce delay

      switch(selectedMenuItem) {
        case 0: {
          selectedNexa = (selectedNexa+1)%(sizeof(nexa)/sizeof(nexa[0]));
          displaySelectedNexa();
          while (digitalRead(RIGHT_BUTTON_PIN) == LOW);
          break;
        }
        case 1:
        case 2:
        case 3: {
          while (digitalRead(RIGHT_BUTTON_PIN) == LOW) {
            bool activation;
            if (selectedMenuItem==3) activation = !activation;
            else activation = selectedMenuItem==1;

            if (activation) {
              tft.fillCircle(126, 10, 8, TFT_YELLOW);
            }
            else {
              tft.fillCircle(126, 10, 8, TFT_BLACK);
              tft.drawCircle(126, 10, 8, TFT_YELLOW);
            }
            nexaTx.transmit(nexa[selectedNexa].type, nexa[selectedNexa].id, activation, 2);
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
  Serial.begin(115200);
  Serial.print("setup() running on core ");
  Serial.println(xPortGetCoreID());

  dallas.begin();
  tft.init();
  tft.setRotation(0);
  tft.setTextSize(1);

  btStop();
  Serial.println("Bluetooth stopped!");
  disableWiFi();
  Serial.println("Wifi disabled!");

  pinMode(LEFT_BUTTON_PIN, INPUT);
  pinMode(RIGHT_BUTTON_PIN, INPUT);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("ESP32 Temp Control", 0, 0*13, 2);
  tft.drawString(__DATE__,             0, 1*13, 2);
  tft.drawString(__TIME__,             0, 2*13, 2);
  tft.drawString("Press for menu...",  0, 4*13, 2);

  unsigned long timeout = millis() + 4000;
  while (millis() < timeout) {
    tft.drawNumber((timeout-millis())/1000, 0, 5*13, 2);
    if (digitalRead(LEFT_BUTTON_PIN) == LOW || digitalRead(RIGHT_BUTTON_PIN) == LOW) {
      menuSystem();
    }
  }

  preferences.begin("temp", false);
  setMode = preferences.getUChar("setMode", 0);
  setTemp = preferences.getUChar(String(setMode).c_str(), minTemp[setMode]);

  xTaskCreatePinnedToCore(displayTask, "displayTask", 10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask,  "buttonTask",  10000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(controlTask, "controlTask", 10000, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelay(1);
}
