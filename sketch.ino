#define ARDUINOJSON_USE_LONG_LONG 1
#define _TASK_SLEEP_ON_IDLE_RUN
#define _TASK_STATUS_REQUEST
#define _TASK_PRIORITY

#include <FS.h>
#include <TaskScheduler.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "RTClib.h"
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW 
#define MAX_DEVICES 4
#define CLK_PIN   D5
#define DATA_PIN  D7
#define CS_PIN    D8
#define BUTTON_PIN D2
#define BUZZER_PIN D1

struct CovidData {
  int ts;
  int cases;
  String country;
  bool yesterday;
};

RTC_Millis rtc;
MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
WiFiManager wifiManager;
WiFiClientSecure wifiClient;
HTTPClient http;
CovidData covidData = {0, 0, "UA", false};
Scheduler scheduler;
bool showStats = false;
char message[100] = {"Using existing Wi-Fi settings"};
uint8_t scrollSpeed = 50;
textEffect_t scrollEffect = PA_SCROLL_LEFT;
textEffect_t textEffect = PA_SCROLL_LEFT;
textPosition_t scrollAlign = PA_LEFT;
uint16_t scrollPause = 0;

void updateCovidStats();
void displayStats();
void updateDisplay();
void wifiConfig();

Task updateCovidStatsTask(600000, TASK_FOREVER, &updateCovidStats, &scheduler);
Task displayStatsTask(0, TASK_FOREVER, &displayStats, &scheduler, true);
Task updateDisplayTask(6000, TASK_FOREVER, &updateDisplay, &scheduler);
Task wifiConfigTask(0, TASK_ONCE, &wifiConfig, &scheduler, true);

ICACHE_RAM_ATTR void configModeCallback (WiFiManager *manager) {
  Serial.println("Starting AP mode");
  strcpy(message, "Failed. Use 'COVID19 Counter' AP to setup");
  scrollEffect = textEffect = PA_SCROLL_LEFT;
  scrollAlign = PA_LEFT;
  scrollSpeed = 30;
  P.setTextEffect(scrollEffect, textEffect);
  P.setTextAlignment(scrollAlign);
  P.displayText(message, scrollAlign, scrollSpeed, scrollPause, scrollEffect, textEffect);
  P.displayClear();
  P.displayReset();
  int counter = 0;
  while (counter <= 300) {
    counter++;
    delay(50);
    P.displayAnimate();
  }
  Serial.println(counter);
  P.setTextAlignment(PA_CENTER);
  P.print("AP");
}

ICACHE_RAM_ATTR DynamicJsonDocument loadJson(String path) {
  Serial.println("Mount Controller FS");
  DynamicJsonDocument conf(1024);
  if (SPIFFS.exists(path)) {
    Serial.println("Found Custom Config on a FS");
    File configFile = SPIFFS.open(path, "r");
    if (configFile) {
      Serial.println("Opened Config File");
      size_t size = configFile.size();
      std::unique_ptr<char[]> buf(new char[size]);
      configFile.readBytes(buf.get(), size);
      auto error = deserializeJson(conf, buf.get());
      if (error) {
        conf.clear();
        Serial.println("Failed to load json config");
      }
      configFile.close();
    }
  }
  return conf; 
}

ICACHE_RAM_ATTR bool saveJson(String path, DynamicJsonDocument data) {
  File configFile = SPIFFS.open(path, "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
    return false;
  }
  auto error = serializeJson(data, configFile);
  if (error) {
    return false;
  }
  configFile.close();
  return true;
}

ICACHE_RAM_ATTR DynamicJsonDocument getRequest(String host, String path, DynamicJsonDocument filter, bool reuse) {
  DynamicJsonDocument data(1024);
  String payload = "";
  wifiClient.setInsecure();
  if (http.begin(wifiClient, host, 443, path, true)) {
    int httpCode = http.GET();
    if (httpCode > 0) {
       Serial.printf("[HTTP] GET... code: %d\n", httpCode);
       if (httpCode != HTTP_CODE_OK) {
          http.end();
          DynamicJsonDocument err(200);
          err["status"] = String("HTTP Code: " + String(httpCode) + " != 200");
          serializeJson(err, Serial);
          Serial.println();
          return data;
       }
    } else {
       Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
       http.end();
       return data;
    }
    payload = http.getString();
    http.setReuse(reuse);
    http.end();
  }
  DeserializationError error = deserializeJson(data, payload, DeserializationOption::Filter(filter));
  if (error) {
    Serial.print("Failed to parse payload: ");
    Serial.println(error.c_str());
    return data;
  }
  return data;
}

ICACHE_RAM_ATTR void wifiConfig() {
  if (covidData.cases == 0) {
    P.setTextAlignment(PA_CENTER);
    P.print("loading");
  }
  if (!wifiManager.autoConnect("COVID19 Counter")) {
    Serial.println("Failed to connect to Wi-Fi...");
    ESP.reset();
    delay(1000);
  }
  rtc.begin(DateTime(F(__DATE__), F(__TIME__)));
  if (covidData.cases == 0) {
    Serial.println("Invalid data. Resetting config");
    strcpy(message, "Done! Waiting for data to be retrieved");
    P.displayClear();
    P.displayReset();
    P.print("Success");
  }
  Serial.println("Country: " + covidData.country);
  while(covidData.country == "") {
    Serial.println("Retrieved country code is invalid. Requesting a new one...");
    delay(3000);
    DynamicJsonDocument filter(64);
    filter["country_code2"] = true;
    DynamicJsonDocument data = getRequest("api.ipgeolocation.io", "/ipgeo?apiKey=ddcfc88cb0a74c929894528c3afc5bd0", filter, false);
    if (!data.isNull()) {
      covidData.country = data["country_code2"].as<String>();
      Serial.println("Country code: " + covidData.country);
      break;
    }
  }
  updateCovidStatsTask.enable();
}

ICACHE_RAM_ATTR void displayStats() {
  P.displayAnimate();
}

ICACHE_RAM_ATTR void updateDisplay() {
  String result = "";
  if (showStats) {
    displayStatsTask.disable();
    scrollEffect = PA_PRINT;
    textEffect = PA_NO_EFFECT;
    scrollAlign = PA_CENTER;
    result = covidData.cases;
  } else {
    scrollEffect = textEffect = PA_SCROLL_LEFT;
    scrollAlign = PA_LEFT;
    DateTime dt = DateTime(covidData.ts);
    if (covidData.yesterday) {
      dt = dt - TimeSpan(1, 0, 0, 0);
    }
    char dtFormat[] = "DD MMM YYYY";
    String date = dt.toString(dtFormat);
    Serial.println("Setting data for " + date); 
    result = date + " (" + covidData.country + ")";
  }
  P.setTextEffect(scrollEffect, textEffect);
  P.setTextAlignment(scrollAlign);
  strcpy(message, String(result).c_str());
  P.displayClear();
  P.displayReset();
  if (showStats) {
    P.print(message);
  } else {
    displayStatsTask.enable();
    P.displayText(message, scrollAlign, scrollSpeed, scrollPause, scrollEffect, textEffect);
  }
  showStats = !showStats;
}

ICACHE_RAM_ATTR void updateCovidStats() {
  DynamicJsonDocument filter(64);
  filter["todayCases"] = true;
  filter["updated"] = true;
  DynamicJsonDocument data(1024);
  bool yesterday = false;
  for (int i = 0; i < 2; i++) {
    while (data.isNull()) {
      delay(3000);
      Serial.println("Requesting COVID19 Data...");
      String path = "/v2/countries/" + covidData.country + "?yesterday=" + (i != 0 ? "true" : "false") + "&strict=true&query";
      data = getRequest("corona.lmao.ninja", path, filter, true);
    }
    Serial.println("Got data successfully");
    yesterday = (bool)i;
    if (data["todayCases"].as<int>() == 0) {
      Serial.println("New statistics is not yet available");
      data.clear();
    } else {
      break;
    }
  }
  int ts = data["updated"].as<int64_t>()/1000;
  if (ts > covidData.ts) {
    covidData.yesterday = yesterday;
    Serial.println("Old timestamp: " + String(covidData.ts));
    Serial.println("New timestamp: " + String(ts));
    Serial.println("New Covid19 stats received. Updating existing data...");
    covidData.ts = ts;
    int newCases = data["todayCases"].as<int>();
    if (covidData.cases != newCases) {
      tone(BUZZER_PIN, 523);
      delay(100);
      noTone(BUZZER_PIN);
    }
    covidData.cases = newCases;
    DynamicJsonDocument upload(1024);
    upload["ts"] = covidData.ts;
    upload["cases"] = covidData.cases;
    upload["country"] = covidData.country;
    upload["yesterday"] = covidData.yesterday;
    saveJson("/config.json", upload);
    Serial.println("Commited new data FS");
  }
  updateDisplayTask.enableIfNot();
  Serial.println("Free Heap Size: " + String(ESP.getFreeHeap()));
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  P.begin();
  wifiManager.setAPCallback(configModeCallback);
  if (SPIFFS.begin()) {
    DynamicJsonDocument json = loadJson("/config.json");
    if (!json.isNull()) {
      covidData.ts = json["ts"].as<int>();
      covidData.cases = json["cases"].as<int>();
      covidData.country = json["country"].as<String>();
      covidData.yesterday = json["yesterday"].as<bool>();
      Serial.println("Config loaded. Starting Rendering");
      updateDisplayTask.enable();
    }
  } else {
    Serial.println("Failed to mount FS");
    ESP.reset();
    delay(1000);
  }
  Serial.println("cases: " + String(covidData.cases));
  if (covidData.cases == 0) {
    P.displayText(message, scrollAlign, 38, scrollPause, scrollEffect, textEffect);
    P.displayClear();
    P.displayReset();
  }
  wifiConfigTask.delay(7000);
}

void loop() {
  scheduler.execute();
}
