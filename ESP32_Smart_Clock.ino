// ============================================================
//  ESP32 Smart Clock / Watch  -  complete sketch
//  Liquid-glass web UI + OLED clock, weather, stopwatch, timer
//  Paste over your entire sketch. Nothing else to change
//  except your Wi-Fi name / password / API key below.
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <EEPROM.h>

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- WiFi ----------------
const char* ssid = "moto g04";       // your WiFi name
const char* password = "Kroud@123";   // your WiFi password

// ---------------- Weather API ----------------
const char* weatherApiKey = "4ddfe7e0babe498eaf375032262411";
String userCity = "Kolkata";         // default city (overridden by saved city)

// ---------------- EEPROM layout ----------------
#define EEPROM_SIZE 512
#define EE_MODE_ADDR 0
#define EE_CITY_ADDR 10
#define CITY_MAX_LEN 32

// ---------------- Time ----------------
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;    // fallback GMT+5:30 until the weather API gives the city's timezone
const int daylightOffset_sec = 0;

// ---------------- Web server ----------------
WebServer server(80);

// ---------------- Display modes ----------------
enum DisplayMode {
  ANALOG_CLOCK = 0,
  DIGITAL_CLOCK = 1,
  WEATHER_INFO = 2,
  STOPWATCH = 3,
  TIMER = 4
};

DisplayMode currentMode = DIGITAL_CLOCK;

unsigned long lastWeatherAttempt = 0;
unsigned long lastWeatherUpdate = 0;
bool weatherOk = false;
unsigned long lastTimeUpdate = 0;
const unsigned long weatherUpdateInterval = 300000;  // 5 minutes
const unsigned long weatherRetryInterval = 30000;    // retry after 30 s if the last update failed
const unsigned long timeUpdateInterval = 30;         // redraw every 30 ms

bool use24Hour = true;        // false = 12-hour with AM/PM
bool use2HourOffset = false;  // when ON, every clock on the watch shows time + 2 hours

// Stopwatch
unsigned long stopwatchStart = 0;
unsigned long stopwatchElapsed = 0;
bool stopwatchRunning = false;

// Timer
unsigned long timerSetSec = 60;      // chosen length
unsigned long timerRemaining = 60;   // seconds left while stopped / paused
unsigned long timerEndMillis = 0;    // when it will finish while running
bool timerRunning = false;
bool timerDone = false;

// Weather data
struct WeatherData {
  float temperature = 0;
  float feelsLike = 0;
  float humidity = 0;
  String condition = "--";
  String location = "--";
  String date = "";
} weather;

// ---------------- Prototypes ----------------
void readCityFromEEPROM();
void saveCityToEEPROM();
void setupDisplay();
void setupWiFi();
void setupWebServer();
void syncTime();
void applyUtcOffset(long offsetSec);
bool getClockTime(struct tm* t);
bool updateWeather(const String& city);
String urlEncode(const String& s);
String jsonEscape(const String& s);
unsigned long getStopwatchMs();
unsigned long timerRemainingMs();
void drawMinimalAnalogClockFace();
void drawMinimalClockHand(int cx, int cy, int angle, int length, int thickness);
void displayAnalogClock();
void displayDigitalClock();
void displayWeatherInfo();
void displayStopwatch();
void displayTimer();
void handleRoot();
void handleModeChange();
void handleGetStatus();
void handleToggle24Hour();
void handleToggle2HourOffset();
void handleStopwatchStart();
void handleStopwatchStop();
void handleStopwatchReset();
void handleTimerSet();
void handleTimerStart();
void handleTimerStop();
void handleSetCity();

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);

  EEPROM.begin(EEPROM_SIZE);

  // Restore last watch face (0-4)
  uint8_t savedMode = EEPROM.read(EE_MODE_ADDR);
  currentMode = (savedMode <= 4) ? (DisplayMode)savedMode : DIGITAL_CLOCK;

  // Restore saved city BEFORE the first weather request
  readCityFromEEPROM();
  weather.location = userCity;

  setupDisplay();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32 Smart Clock");
  display.println("Initializing...");
  display.display();

  setupWiFi();
  setupWebServer();
  syncTime();
  updateWeather(userCity);   // also sets the correct timezone for the city

  Serial.println("Setup complete!");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  if (now - lastTimeUpdate >= timeUpdateInterval) {
    lastTimeUpdate = now;

    switch (currentMode) {
      case ANALOG_CLOCK:  displayAnalogClock();  break;
      case DIGITAL_CLOCK: displayDigitalClock(); break;
      case WEATHER_INFO:  displayWeatherInfo();  break;
      case STOPWATCH:     displayStopwatch();    break;
      case TIMER:         displayTimer();        break;
    }
  }

  // Refresh weather every 5 minutes (retry sooner if the last try failed)
  unsigned long wait = weatherOk ? weatherUpdateInterval : weatherRetryInterval;
  if (now - lastWeatherAttempt >= wait) {
    updateWeather(userCity);
  }
}

// ============================================================
//  INIT HELPERS
// ============================================================
void setupDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    while (1);
  }
  display.clearDisplay();
  display.display();
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Connecting to WiFi");
  display.display();

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - startAttempt > 30000) {   // give up after 30 s and try again
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("WiFi failed");
      display.println("Restarting...");
      display.display();
      delay(2000);
      ESP.restart();
    }
  }

  Serial.println("");
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi Connected!");
  display.print("IP: ");
  display.println(WiFi.localIP());
  display.display();
  delay(2000);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/mode", handleModeChange);
  server.on("/status", handleGetStatus);
  server.on("/toggle24h", handleToggle24Hour);
  server.on("/toggle2h", handleToggle2HourOffset);
  server.on("/stopwatch/start", handleStopwatchStart);
  server.on("/stopwatch/stop", handleStopwatchStop);
  server.on("/stopwatch/reset", handleStopwatchReset);
  server.on("/timer/set", handleTimerSet);
  server.on("/timer/start", handleTimerStart);
  server.on("/timer/stop", handleTimerStop);
  server.on("/setcity", handleSetCity);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });

  server.begin();
  Serial.println("Web server started");
}

void syncTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println("Time synchronized");
}

// ============================================================
//  EEPROM CITY STORAGE
// ============================================================
void readCityFromEEPROM() {
  char buf[CITY_MAX_LEN + 1] = {0};
  bool valid = true;

  for (int i = 0; i < CITY_MAX_LEN; i++) {
    uint8_t c = EEPROM.read(EE_CITY_ADDR + i);
    if (c == 0) break;
    if (c < 32 || c > 126) { valid = false; break; }   // blank / garbage EEPROM
    buf[i] = (char)c;
  }

  String saved = String(buf);
  saved.trim();
  if (valid && saved.length() > 0) userCity = saved;
  else userCity = "Kolkata";
}

void saveCityToEEPROM() {
  for (int i = 0; i < CITY_MAX_LEN; i++) {
    if (i < (int)userCity.length() && i < CITY_MAX_LEN - 1) EEPROM.write(EE_CITY_ADDR + i, userCity[i]);
    else EEPROM.write(EE_CITY_ADDR + i, 0);
  }
  EEPROM.commit();
}

// ============================================================
//  TIME HELPERS
// ============================================================

// Sets the timezone to a fixed UTC offset, e.g. +19800 -> "<+0530>-5:30"
void applyUtcOffset(long offsetSec) {
  char tz[24];
  long a = labs(offsetSec);
  int hh = a / 3600;
  int mm = (a % 3600) / 60;
  snprintf(tz, sizeof(tz), "<%c%02d%02d>%c%d:%02d",
           offsetSec >= 0 ? '+' : '-', hh, mm,
           offsetSec >= 0 ? '-' : '+', hh, mm);
  setenv("TZ", tz, 1);
  tzset();
}

// Days since 1970-01-01 for a calendar date
static long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// Current time as shown on every screen (applies the optional +2 h offset)
bool getClockTime(struct tm* t) {
  if (!getLocalTime(t, 50)) return false;
  if (use2HourOffset) {
    time_t n = mktime(t) + 7200;
    localtime_r(&n, t);
  }
  return true;
}

// ============================================================
//  WEATHER
// ============================================================
String urlEncode(const String& s) {
  String out;
  char hex[4];
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { snprintf(hex, sizeof(hex), "%%%02X", c); out += hex; }
  }
  return out;
}

String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c >= 32) o += c;
  }
  return o;
}

// Returns true on success. Existing weather data is untouched if it fails.
bool updateWeather(const String& city) {
  lastWeatherAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = "http://api.weatherapi.com/v1/current.json?key=" + String(weatherApiKey) + "&q=" + urlEncode(city);
  http.setTimeout(8000);
  if (!http.begin(url)) return false;

  int httpCode = http.GET();
  bool ok = false;

  if (httpCode == 200) {
    DynamicJsonDocument doc(3072);
    DeserializationError err = deserializeJson(doc, http.getString());

    if (!err && !doc["current"].isNull() && !doc["location"].isNull()) {
      weather.temperature = doc["current"]["temp_c"] | 0.0f;
      weather.feelsLike   = doc["current"]["feelslike_c"] | 0.0f;
      weather.humidity    = doc["current"]["humidity"] | 0.0f;
      weather.condition   = doc["current"]["condition"]["text"].as<String>();
      weather.location    = doc["location"]["name"].as<String>();
      weather.date        = doc["location"]["localtime"].as<String>();

      // Work out the city's UTC offset from its local clock vs the real UTC epoch
      long epoch = doc["location"]["localtime_epoch"] | 0L;
      int y, mo, d, h, mi;
      if (epoch > 0 && sscanf(weather.date.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
        long localSecs = daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L;
        long diff = localSecs - epoch;
        long offset = lround(diff / 900.0) * 900L;   // round to nearest 15 minutes
        applyUtcOffset(offset);
      }

      lastWeatherUpdate = millis();
      ok = true;
      Serial.println("Weather updated");
    } else {
      Serial.println("Weather: bad response");
    }
  } else {
    Serial.print("Weather HTTP error: ");
    Serial.println(httpCode);
  }

  http.end();
  weatherOk = ok;
  return ok;
}

// ============================================================
//  STOPWATCH / TIMER HELPERS
// ============================================================
unsigned long getStopwatchMs() {
  return stopwatchRunning ? (millis() - stopwatchStart) : stopwatchElapsed;
}

// Milliseconds left on the timer; also detects when it reaches zero
unsigned long timerRemainingMs() {
  if (!timerRunning) return timerRemaining * 1000UL;
  long left = (long)(timerEndMillis - millis());
  if (left <= 0) {
    timerRunning = false;
    timerRemaining = 0;
    timerDone = true;
    return 0;
  }
  return (unsigned long)left;
}

// ============================================================
//  OLED SCREENS
// ============================================================
void drawMinimalAnalogClockFace() {
  int cx = 36, cy = 32, rOuter = 28;

  display.drawCircle(cx, cy, rOuter, SSD1306_WHITE);

  for (int i = 0; i < 12; i++) {
    float angle = (i * 30 - 90) * PI / 180;
    int x1 = cx + (rOuter - 7) * cos(angle);
    int y1 = cy + (rOuter - 7) * sin(angle);
    int x2 = cx + rOuter * cos(angle);
    int y2 = cy + rOuter * sin(angle);
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);

    if (i == 0 || i == 3 || i == 6 || i == 9) {
      int numX = cx + (rOuter - 13) * cos(angle) - ((i == 0) ? 6 : 3);
      int numY = cy + (rOuter - 13) * sin(angle) - 4;
      display.setTextSize(1);
      display.setCursor(numX, numY);
      if (i == 0) display.print("12");
      else if (i == 3) display.print("3");
      else if (i == 6) display.print("6");
      else if (i == 9) display.print("9");
    }
  }
}

void drawMinimalClockHand(int cx, int cy, int angle, int length, int thickness) {
  float rad = angle * PI / 180;
  int x2 = cx + length * cos(rad);
  int y2 = cy + length * sin(rad);
  display.drawLine(cx, cy, x2, y2, SSD1306_WHITE);
}

void displayAnalogClock() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  struct tm timeinfo;
  if (!getClockTime(&timeinfo)) {
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.print("No Sync");
    display.display();
    return;
  }

  drawMinimalAnalogClockFace();

  int hours = timeinfo.tm_hour % 12;
  int minutes = timeinfo.tm_min;
  int seconds = timeinfo.tm_sec;
  int hourAngle = (hours * 30) + (minutes * 0.5) - 90;
  int minuteAngle = (minutes * 6) - 90;
  int secondAngle = (seconds * 6) - 90;

  int cx = 36, cy = 32;
  drawMinimalClockHand(cx, cy, hourAngle, 12, 3);
  drawMinimalClockHand(cx, cy, minuteAngle, 19, 2);
  drawMinimalClockHand(cx, cy, secondAngle, 24, 1);
  display.fillCircle(cx, cy, 2, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(76, 15);
  const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  display.printf("%s", days[timeinfo.tm_wday]);

  display.setCursor(76, 25);
  display.printf("%02d %s", timeinfo.tm_mday, months[timeinfo.tm_mon]);
  display.setCursor(76, 35);
  display.printf("%04d", timeinfo.tm_year + 1900);

  display.display();
}

void displayDigitalClock() {
  display.clearDisplay();

  struct tm timeinfo;
  if (!getClockTime(&timeinfo)) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Time sync failed");
    display.display();
    return;
  }

  int hourToShow = timeinfo.tm_hour;
  String ampm = "";
  if (!use24Hour) {
    ampm = (hourToShow >= 12) ? "PM" : "AM";
    hourToShow = hourToShow % 12;
    if (hourToShow == 0) hourToShow = 12;
  }

  // Large time
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(5, 5);
  display.printf("%02d:%02d", hourToShow, timeinfo.tm_min);

  // Seconds
  display.setTextSize(2);
  display.setCursor(100, 15);
  display.printf("%02d", timeinfo.tm_sec);

  // AM/PM
  if (!use24Hour) {
    display.setTextSize(1);
    display.setCursor(100, 5);
    display.print(ampm);
  }

  display.drawLine(0, 37, 127, 37, SSD1306_WHITE);

  // Date
  display.setTextSize(1);
  display.setCursor(13, 42);
  const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  display.printf("%s %s %02d, %02d",
                 days[timeinfo.tm_wday],
                 months[timeinfo.tm_mon],
                 timeinfo.tm_mday,
                 timeinfo.tm_year + 1900);

  // Temperature and humidity
  display.setCursor(1, 56);
  display.printf("Temp: %.1f", weather.temperature);
  display.write(248);
  display.print("C  ");

  display.setCursor(85, 56);
  display.printf("Hum:%d%%", (int)weather.humidity);

  display.display();
}

void displayWeatherInfo() {
  display.clearDisplay();

  // Header: location + WiFi
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 2);
  display.print("LOC:");
  display.print(weather.location.substring(0, 12));

  display.setCursor(73, 2);
  int rssi = WiFi.RSSI();
  display.print("WiFi[");
  if (rssi > -50) display.print("***");
  else if (rssi > -70) display.print("** ");
  else display.print("*  ");
  display.print("]");

  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  // Temperature
  display.setTextSize(3);
  display.setCursor(10, 16);
  display.printf("%2.0f", weather.temperature);

  display.setTextSize(1);
  display.setCursor(47, 23);
  display.write(248);
  display.print("C");

  // Condition code
  display.setTextSize(1);
  display.setCursor(95, 18);
  String condition = weather.condition;
  condition.toLowerCase();
  if (condition.indexOf("sunny") >= 0 || condition.indexOf("clear") >= 0) display.print("SUN");
  else if (condition.indexOf("cloud") >= 0 || condition.indexOf("overcast") >= 0) display.print("CLD");
  else if (condition.indexOf("rain") >= 0 || condition.indexOf("drizzle") >= 0) display.print("RAN");
  else if (condition.indexOf("snow") >= 0) display.print("SNW");
  else if (condition.indexOf("mist") >= 0 || condition.indexOf("fog") >= 0 || condition.indexOf("haze") >= 0) display.print("MST");
  else display.print(" --- ");

  display.setCursor(95, 29);
  String shortCond = weather.condition;
  if (shortCond.length() > 8) shortCond = shortCond.substring(0, 8);
  display.print(shortCond);

  // Humidity + feels like (real value from the API)
  display.drawLine(0, 39, 127, 39, SSD1306_WHITE);
  display.setCursor(2, 41);
  display.print("HUM: ");
  display.printf("%2.0f%%", weather.humidity);

  display.setCursor(60, 41);
  display.print("FEELS ");
  display.printf("%2.0f", weather.feelsLike);
  display.write(248);
  display.print("C");

  // Time + last update
  struct tm timeinfo;
  if (getClockTime(&timeinfo)) {
    display.setCursor(2, 53);
    display.print("TIME:");
    display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    display.setCursor(67, 53);
    display.print("Upd ");
    if (lastWeatherUpdate == 0) display.print("--");
    else {
      unsigned long minutes = (millis() - lastWeatherUpdate) / 60000;
      if (minutes < 1) display.print("now");
      else display.printf("%dm ago", (int)minutes);
    }
  }

  display.display();
}

void displayStopwatch() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(10, 5);
  display.print("STOPWATCH");

  unsigned long elapsed = getStopwatchMs();

  unsigned long cs = (elapsed % 1000) / 10;          // hundredths of a second (2 digits)
  unsigned long totalSeconds = elapsed / 1000;
  unsigned long s = totalSeconds % 60;
  unsigned long m = (totalSeconds / 60) % 60;
  unsigned long h = totalSeconds / 3600;

  display.setTextSize(2);
  display.setCursor(10, 35);
  display.printf("%02lu:%02lu:%02lu", h, m, s);

  display.setTextSize(2);
  display.setCursor(90, 10);
  display.printf("%02lu", cs);

  display.display();
}

void displayTimer() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(10, 5);
  display.print("TIMER");

  unsigned long remainingMs = timerRemainingMs();
  unsigned long remaining = (remainingMs + 999) / 1000;   // round up so it hits 00:00:00 only at the end

  unsigned long s = remaining % 60;
  unsigned long m = (remaining / 60) % 60;
  unsigned long h = remaining / 3600;

  display.setTextSize(2);
  display.setCursor(15, 20);
  display.printf("%02lu:%02lu:%02lu", h, m, s);

  display.setTextSize(1);
  display.setCursor(35, 48);
  if (timerDone) {
    if ((millis() / 500) % 2 == 0) display.print("** TIME UP **");
  } else if (timerRunning) {
    display.print("Running");
  } else {
    display.print("Ready");
  }

  display.display();
}

// ============================================================
//  WEB HANDLERS
// ============================================================
void handleToggle24Hour() {
  use24Hour = !use24Hour;
  server.send(200, "text/plain", use24Hour ? "24-hour time on" : "12-hour time on");
}

void handleToggle2HourOffset() {
  use2HourOffset = !use2HourOffset;
  server.send(200, "text/plain", use2HourOffset ? "2-hour offset on" : "2-hour offset off");
}

void handleStopwatchStart() {
  if (!stopwatchRunning) {
    stopwatchStart = millis() - stopwatchElapsed;   // resume from the stored time
    stopwatchRunning = true;
  }
  server.send(200, "text/plain", "OK");
}

void handleStopwatchStop() {
  if (stopwatchRunning) {
    stopwatchElapsed = millis() - stopwatchStart;
    stopwatchRunning = false;
  }
  server.send(200, "text/plain", "OK");
}

void handleStopwatchReset() {
  stopwatchRunning = false;
  stopwatchElapsed = 0;
  server.send(200, "text/plain", "OK");
}

void handleTimerSet() {
  if (!server.hasArg("seconds")) {
    server.send(400, "text/plain", "Missing seconds");
    return;
  }
  long sec = server.arg("seconds").toInt();
  if (sec < 1) sec = 1;
  if (sec > 359999) sec = 359999;   // 99:59:59

  timerSetSec = sec;
  timerRemaining = sec;
  timerRunning = false;
  timerDone = false;
  server.send(200, "text/plain", "OK");
}

void handleTimerStart() {
  if (!timerRunning) {
    if (timerRemaining == 0) timerRemaining = timerSetSec;   // finished -> restart from the full length
    timerEndMillis = millis() + timerRemaining * 1000UL;
    timerRunning = true;
    timerDone = false;
  }
  server.send(200, "text/plain", "OK");
}

void handleTimerStop() {
  unsigned long ms = timerRemainingMs();
  if (timerRunning) {
    timerRemaining = (ms + 999) / 1000;   // pause: keep what is left
    timerRunning = false;
  }
  server.send(200, "text/plain", "OK");
}

void handleSetCity() {
  if (!server.hasArg("city")) {
    server.send(400, "text/plain", "Missing city parameter");
    return;
  }

  String newCity = server.arg("city");
  newCity.trim();
  if (newCity.length() == 0 || newCity.length() > CITY_MAX_LEN - 1) {
    server.send(400, "text/plain", "City name must be 1 to 31 characters");
    return;
  }

  if (updateWeather(newCity)) {
    userCity = newCity;
    saveCityToEEPROM();
    server.send(200, "text/plain", "City set to " + weather.location);
  } else {
    server.send(404, "text/plain", "Could not find that city");
  }
}

void handleModeChange() {
  if (server.hasArg("m")) {
    int newMode = server.arg("m").toInt();

    if (newMode >= 0 && newMode <= 4) {
      if ((int)currentMode != newMode) {
        currentMode = (DisplayMode)newMode;
        EEPROM.write(EE_MODE_ADDR, (uint8_t)currentMode);
        EEPROM.commit();
      }
      Serial.print("Mode changed to: ");
      Serial.println(newMode);
    }
  }
  server.send(200, "text/plain", "OK");
}

// Live status for the web page (clock, weather, stopwatch, timer, settings)
void handleGetStatus() {
  struct tm t;
  if (!getClockTime(&t)) memset(&t, 0, sizeof(t));

  char dateBuf[32];
  strftime(dateBuf, sizeof(dateBuf), "%A, %d %B", &t);

  unsigned long sw = getStopwatchMs();
  unsigned long remMs = timerRemainingMs();

  String json = "{";
  json += "\"mode\":" + String((int)currentMode) + ",";
  json += "\"temperature\":" + String(weather.temperature, 1) + ",";
  json += "\"humidity\":" + String(weather.humidity, 0) + ",";
  json += "\"condition\":\"" + jsonEscape(weather.condition) + "\",";
  json += "\"location\":\"" + jsonEscape(weather.location) + "\",";
  json += "\"date\":\"" + String(dateBuf) + "\",";
  json += "\"h\":" + String(t.tm_hour) + ",\"m\":" + String(t.tm_min) + ",\"s\":" + String(t.tm_sec) + ",";
  json += "\"h24\":" + String(use24Hour ? "true" : "false") + ",";
  json += "\"off\":" + String(use2HourOffset ? "true" : "false") + ",";
  json += "\"sw\":" + String(sw) + ",\"swRun\":" + String(stopwatchRunning ? "true" : "false") + ",";
  json += "\"tSet\":" + String(timerSetSec) + ",\"tRemMs\":" + String(remMs) + ",";
  json += "\"tRun\":" + String(timerRunning ? "true" : "false") + ",\"tDone\":" + String(timerDone ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
}

// ============================================================
//  WEB PAGE  (Liquid Glass UI, served straight from flash)
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#030816">
<title>ESP32 Smart Watch</title>
<style>
:root{--a:#38e1ff;--b:#2f6bff;--ink:#eaf4ff;--mut:rgba(234,244,255,.62);--line:rgba(255,255,255,.13);color-scheme:dark}
*{box-sizing:border-box;margin:0}
body{font-family:"SF Pro Display",-apple-system,"Segoe UI Variable Display","Segoe UI",Inter,Roboto,sans-serif;color:var(--ink);background:#030816;padding:28px 18px 70px;-webkit-font-smoothing:antialiased}
body::before{content:"";position:fixed;inset:0;z-index:-1;background:
  radial-gradient(60% 50% at 12% 0%,rgba(47,107,255,.55),transparent 70%),
  radial-gradient(50% 45% at 92% 14%,rgba(56,225,255,.26),transparent 70%),
  radial-gradient(70% 60% at 50% 112%,rgba(30,64,200,.5),transparent 70%),
  linear-gradient(180deg,#040b24,#020512)}
main{max-width:980px;margin:0 auto;display:grid;gap:18px}

.glass{background:linear-gradient(145deg,rgba(255,255,255,.12),rgba(255,255,255,.03));border:1px solid var(--line);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.24),inset 0 -24px 40px rgba(56,225,255,.04),0 22px 55px rgba(0,5,30,.55);
  -webkit-backdrop-filter:blur(26px) saturate(170%);backdrop-filter:blur(26px) saturate(170%)}
.tile{border-radius:34px;padding:26px}

header{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:0 6px}
h1{font-size:1.5rem;font-weight:650;letter-spacing:-.015em}
h2{font-size:1.05rem;font-weight:600}
.pill{display:flex;align-items:center;gap:8px;padding:8px 14px;border-radius:99px;font-size:.85rem;color:var(--mut)}
.dot{width:8px;height:8px;border-radius:50%;background:#ff6b6b}
.ok .dot{background:#3bf0b0;box-shadow:0 0 10px #3bf0b0}

.hero{display:grid;grid-template-columns:1.15fr 1fr;gap:28px;align-items:center}
#clock{font-size:clamp(3.6rem,10vw,6.2rem);font-weight:300;letter-spacing:-.045em;line-height:1;font-variant-numeric:tabular-nums;
  background:linear-gradient(180deg,#fff 20%,#8fd8ff);-webkit-background-clip:text;background-clip:text;color:transparent}
#clock small{font-size:.32em;letter-spacing:0;margin-left:.3em;font-weight:400;-webkit-text-fill-color:var(--mut)}
#date{margin-top:12px;color:var(--mut);font-size:1.05rem}
.wx{display:flex;align-items:center;gap:22px;justify-content:flex-end}
.orb{position:relative;flex:none;width:128px;height:128px;border-radius:50%;overflow:hidden;border:1px solid rgba(255,255,255,.25);
  background:rgba(3,12,40,.5);box-shadow:inset 0 6px 20px rgba(255,255,255,.12),0 0 40px rgba(47,107,255,.35)}
.fill{position:absolute;left:0;right:0;bottom:0;height:50%;transition:height 1.2s ease;
  background:linear-gradient(180deg,rgba(94,234,255,.92),rgba(37,99,235,.96))}
.fill::before{content:"";position:absolute;top:-9px;left:-25%;width:150%;height:18px;border-radius:50%;background:rgba(150,240,255,.95);animation:sway 6s ease-in-out infinite alternate}
@keyframes sway{to{transform:translateX(16%)}}
.orb b{position:absolute;inset:0;display:grid;place-items:center;font-size:1.7rem;font-weight:500;text-shadow:0 2px 10px rgba(0,10,50,.6)}
#temp{font-size:3.2rem;font-weight:300;line-height:1;letter-spacing:-.03em}
#cond{margin-top:6px}
#loc{color:var(--mut);font-size:.95rem;margin-top:2px}

.modes{display:grid;grid-template-columns:repeat(5,1fr);gap:12px}
.mode{display:grid;justify-items:center;gap:8px;padding:16px 6px;border-radius:26px;color:var(--ink);font:inherit;font-size:.9rem;cursor:pointer;transition:transform .2s,box-shadow .3s,background .3s}
.mode svg{width:26px;height:26px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}
.mode:hover{transform:translateY(-2px)}
.mode[aria-pressed=true]{background:linear-gradient(160deg,rgba(56,225,255,.3),rgba(47,107,255,.38));border-color:rgba(120,230,255,.6);
  box-shadow:0 0 0 1px rgba(120,230,255,.22),0 10px 40px rgba(47,107,255,.5),inset 0 1px 0 rgba(255,255,255,.38)}

.g2{display:grid;grid-template-columns:1fr 1fr;gap:18px}
.head{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.tag{font-size:.8rem;color:var(--mut);padding:4px 12px;border-radius:99px;background:rgba(255,255,255,.08)}
.ringwrap{position:relative;width:min(210px,62vw);aspect-ratio:1;margin:14px auto 22px}
.ringwrap svg{width:100%;height:100%;overflow:visible}
.trk{fill:none;stroke:rgba(255,255,255,.1);stroke-width:7}
.prg{fill:none;stroke:url(#g);stroke-width:7;stroke-linecap:round;stroke-dasharray:339.29;stroke-dashoffset:339.29;
  transform:rotate(-90deg);transform-box:fill-box;transform-origin:50% 50%;filter:drop-shadow(0 0 6px rgba(56,225,255,.7))}
.ringtxt{position:absolute;inset:0;display:grid;place-items:center;font-size:1.65rem;font-weight:400;font-variant-numeric:tabular-nums;letter-spacing:-.01em}

.row{display:flex;gap:10px;justify-content:center;flex-wrap:wrap}
.row + .row{margin-top:12px}
.btn{padding:12px 22px;border-radius:99px;font:inherit;font-weight:600;color:var(--ink);cursor:pointer;border:1px solid var(--line);
  background:rgba(255,255,255,.09);transition:transform .15s,background .25s,box-shadow .25s}
.btn:hover{background:rgba(255,255,255,.18)}
.btn:active{transform:scale(.96)}
.btn.go{background:linear-gradient(135deg,var(--a),var(--b));border-color:transparent;color:#021028;box-shadow:0 8px 26px rgba(47,107,255,.5)}
.btn.go:hover{filter:brightness(1.1)}
.chip{padding:7px 14px;font-size:.85rem;font-weight:500}
.inp{flex:1;min-width:0;padding:12px 18px;border-radius:99px;border:1px solid var(--line);background:rgba(2,8,30,.5);color:var(--ink);font:inherit}
.inp::placeholder{color:rgba(234,244,255,.4)}
.inp:focus{outline:none;border-color:var(--a);box-shadow:0 0 0 3px rgba(56,225,255,.2)}
.btn:focus-visible,.mode:focus-visible,.sw:focus-visible{outline:2px solid var(--a);outline-offset:3px}

.setrow{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:15px 0;border-top:1px solid var(--line)}
.setrow:first-of-type{border-top:0}
.setrow span{font-size:.95rem}
.setrow em{display:block;font-style:normal;font-size:.8rem;color:var(--mut);margin-top:2px}
.sw{flex:none;width:52px;height:32px;border-radius:99px;border:1px solid var(--line);background:rgba(255,255,255,.14);position:relative;cursor:pointer;transition:background .25s}
.sw::after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;box-shadow:0 2px 6px rgba(0,0,0,.4);transition:transform .28s cubic-bezier(.3,1.4,.5,1)}
.sw[aria-checked=true]{background:linear-gradient(135deg,var(--a),var(--b));border-color:transparent}
.sw[aria-checked=true]::after{transform:translateX(20px)}

#toast{position:fixed;left:50%;bottom:26px;transform:translate(-50%,20px);padding:12px 22px;border-radius:99px;opacity:0;pointer-events:none;transition:.3s;font-size:.92rem}
#toast.on{opacity:1;transform:translate(-50%,0)}

@media (max-width:720px){
  body{padding:18px 12px 80px}
  .tile{padding:20px;border-radius:28px}
  .hero,.g2{grid-template-columns:1fr}
  .wx{justify-content:flex-start}
  .modes{grid-template-columns:repeat(3,1fr)}
  h1{font-size:1.25rem}
}
@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style>
</head>
<body>
<svg width="0" height="0" style="position:absolute" aria-hidden="true"><defs>
  <linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#38e1ff"/><stop offset="1" stop-color="#2f6bff"/></linearGradient>
</defs></svg>

<main>
  <header>
    <h1>ESP32 Smart Watch</h1>
    <div class="pill glass" id="conn"><span class="dot"></span><span id="connTxt">Connecting</span></div>
  </header>

  <section class="tile glass hero">
    <div>
      <div id="clock">--:--</div>
      <div id="date">&nbsp;</div>
    </div>
    <div class="wx">
      <div class="orb"><div class="fill" id="fill"></div><b id="hum">--%</b></div>
      <div>
        <div id="temp">--&deg;</div>
        <div id="cond">--</div>
        <div id="loc">--</div>
      </div>
    </div>
  </section>

  <nav class="modes" aria-label="Watch face">
    <button class="mode glass" aria-pressed="false" onclick="setMode(0)">
      <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>Analog</button>
    <button class="mode glass" aria-pressed="false" onclick="setMode(1)">
      <svg viewBox="0 0 24 24"><rect x="3" y="6" width="18" height="12" rx="3.5"/><path d="M8 10.5v3M12 10.5v3M16 10.5v3"/></svg>Digital</button>
    <button class="mode glass" aria-pressed="false" onclick="setMode(2)">
      <svg viewBox="0 0 24 24"><path d="M7 18a4 4 0 0 1-.4-7.98A5.5 5.5 0 0 1 17.2 9 4.5 4.5 0 0 1 17 18H7z"/></svg>Weather</button>
    <button class="mode glass" aria-pressed="false" onclick="setMode(3)">
      <svg viewBox="0 0 24 24"><circle cx="12" cy="13.5" r="7.5"/><path d="M12 9.5v4l2.2 1.6M9.5 2.5h5"/></svg>Stopwatch</button>
    <button class="mode glass" aria-pressed="false" onclick="setMode(4)">
      <svg viewBox="0 0 24 24"><path d="M6 3h12M6 21h12M7 3c0 5 3 6 5 9-2 3-5 4-5 9M17 3c0 5-3 6-5 9 2 3 5 4 5 9"/></svg>Timer</button>
  </nav>

  <div class="g2">
    <section class="tile glass">
      <div class="head"><h2>Stopwatch</h2><span class="tag" id="swState">Stopped</span></div>
      <div class="ringwrap">
        <svg viewBox="0 0 120 120"><circle class="trk" cx="60" cy="60" r="54"/><circle class="prg" id="swRing" cx="60" cy="60" r="54"/></svg>
        <div class="ringtxt" id="swTxt">00:00.00</div>
      </div>
      <div class="row">
        <button class="btn go" onclick="act('/stopwatch/start')">Start</button>
        <button class="btn" onclick="act('/stopwatch/stop')">Stop</button>
        <button class="btn" onclick="act('/stopwatch/reset')">Reset</button>
      </div>
    </section>

    <section class="tile glass">
      <div class="head"><h2>Timer</h2><span class="tag" id="tState">Stopped</span></div>
      <div class="ringwrap">
        <svg viewBox="0 0 120 120"><circle class="trk" cx="60" cy="60" r="54"/><circle class="prg" id="tRing" cx="60" cy="60" r="54"/></svg>
        <div class="ringtxt" id="tTxt">00:00:00</div>
      </div>
      <div class="row">
        <button class="btn chip" onclick="setTimer(60)">1 min</button>
        <button class="btn chip" onclick="setTimer(300)">5 min</button>
        <button class="btn chip" onclick="setTimer(600)">10 min</button>
        <button class="btn chip" onclick="setTimer(1800)">30 min</button>
      </div>
      <div class="row">
        <input class="inp" id="timerInput" type="number" min="1" placeholder="Seconds" inputmode="numeric">
        <button class="btn" onclick="setTimer(+$('timerInput').value)">Set</button>
      </div>
      <div class="row">
        <button class="btn go" onclick="act('/timer/start')">Start</button>
        <button class="btn" onclick="act('/timer/stop')">Stop</button>
      </div>
    </section>
  </div>

  <div class="g2">
    <section class="tile glass">
      <div class="head"><h2>Display</h2></div>
      <div class="setrow"><span>24-hour time<em>Off shows AM / PM</em></span>
        <button class="sw" id="sw24" role="switch" aria-checked="true" aria-label="24-hour time" onclick="toggle('/toggle24h')"></button></div>
      <div class="setrow"><span>2-hour offset<em>Adds 2 hours to every clock</em></span>
        <button class="sw" id="swOff" role="switch" aria-checked="false" aria-label="2-hour offset" onclick="toggle('/toggle2h')"></button></div>
    </section>

    <section class="tile glass">
      <div class="head"><h2>City</h2></div>
      <p style="color:var(--mut);font-size:.9rem;margin-bottom:16px">Weather and timezone follow this city.</p>
      <div class="row" style="flex-wrap:nowrap">
        <input class="inp" id="city" type="text" placeholder="City" maxlength="31" onkeydown="if(event.key==='Enter')setCity()">
        <button class="btn go" onclick="setCity()">Save city</button>
      </div>
    </section>
  </div>
</main>
<div id="toast" class="glass" role="status" aria-live="polite"></div>

<script>
const $=id=>document.getElementById(id);
const pad=n=>String(n).padStart(2,'0');
const C=2*Math.PI*54;
let S=null,T0=0,lastClock='',toastT;

function toast(t){const e=$('toast');e.textContent=t;e.classList.add('on');clearTimeout(toastT);toastT=setTimeout(()=>e.classList.remove('on'),2200)}
function ring(id,p){$(id).style.strokeDashoffset=C*(1-Math.min(1,Math.max(0,p)))}

async function refresh(){
  if(document.hidden&&S)return;
  try{
    const r=await fetch('/status',{cache:'no-store'});
    S=await r.json();T0=performance.now();
    $('conn').classList.add('ok');$('connTxt').textContent='Connected';
    paint();
  }catch(e){
    $('conn').classList.remove('ok');$('connTxt').textContent='Offline';
  }
}

function paint(){
  document.querySelectorAll('.mode').forEach((b,i)=>b.setAttribute('aria-pressed',i===S.mode));
  $('temp').innerHTML=Math.round(S.temperature)+'&deg;';
  $('cond').textContent=S.condition||'--';
  $('loc').textContent=S.location||'--';
  $('hum').textContent=Math.round(S.humidity)+'%';
  $('fill').style.height=Math.min(100,Math.max(0,S.humidity))+'%';
  $('date').textContent=S.date;
  $('city').placeholder=S.location||'City';
  $('sw24').setAttribute('aria-checked',S.h24);
  $('swOff').setAttribute('aria-checked',S.off);
  $('swState').textContent=S.swRun?'Running':'Stopped';
  $('tState').textContent=S.tDone?'Done':(S.tRun?'Running':'Stopped');
}

function tick(){
  if(!S)return;
  const dt=performance.now()-T0;
  const sec=Math.floor(S.h*3600+S.m*60+S.s+dt/1000)%86400;
  let h=Math.floor(sec/3600),m=Math.floor(sec%3600/60),s=sec%60,ap='';
  if(!S.h24){ap=h>=12?' PM':' AM';h=h%12||12}
  const c=pad(h)+':'+pad(m)+'<small>'+pad(s)+ap+'</small>';
  if(c!==lastClock){$('clock').innerHTML=c;lastClock=c}
  const ms=S.swRun?S.sw+dt:S.sw,ts=Math.floor(ms/1000);
  $('swTxt').textContent=(ts>=3600?pad(Math.floor(ts/3600))+':':'')+pad(Math.floor(ts%3600/60))+':'+pad(ts%60)+'.'+pad(Math.floor(ms/10)%100);
  ring('swRing',(ms%60000)/60000);
  const rem=(S.tRun?Math.max(0,S.tRemMs-dt):S.tRemMs)/1000,r=Math.ceil(rem);
  $('tTxt').textContent=pad(Math.floor(r/3600))+':'+pad(Math.floor(r%3600/60))+':'+pad(r%60);
  ring('tRing',S.tSet?rem/S.tSet:0);
}

function setMode(m){
  document.querySelectorAll('.mode').forEach((b,i)=>b.setAttribute('aria-pressed',i===m));
  fetch('/mode?m='+m).then(refresh).catch(()=>toast('Could not reach the watch'));
}
function act(url){fetch(url).then(refresh).catch(()=>toast('Could not reach the watch'))}
function toggle(url){fetch(url).then(r=>r.text()).then(t=>{toast(t);refresh()}).catch(()=>toast('Could not reach the watch'))}
function setTimer(n){
  if(!n||n<1){toast('Enter the timer length in seconds');return}
  fetch('/timer/set?seconds='+Math.floor(n)).then(()=>{toast('Timer set');refresh()}).catch(()=>toast('Could not reach the watch'));
}
function setCity(){
  const v=$('city').value.trim();
  if(!v){toast('Type a city name first');return}
  toast('Updating weather...');
  fetch('/setcity?city='+encodeURIComponent(v))
    .then(async r=>{toast(await r.text());if(r.ok)$('city').value='';refresh()})
    .catch(()=>toast('Could not reach the watch'));
}

setInterval(tick,50);
setInterval(refresh,2000);
refresh();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);   // served from flash, no heap copy
}
