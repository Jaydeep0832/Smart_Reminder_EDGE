/****************************************************************
 *  Smart Daily Reminder Assistant - ESP32  (EDGE COMPUTING EDITION)
 *
 *  100% offline: no cloud, no internet, no external database.
 *   1. Reminders are stored in the ESP32's own flash (LittleFS)
 *   2. The ESP32 makes its own Wi-Fi hotspot + web page (192.168.4.1)
 *   3. A scheduler inside the ESP32 decides when to alert
 *   4. Three buttons: STOP (done) | SNOOZE (later) | MENU (OLED page)
 *   5. An ignored reminder rings up to MAX_ALERTS times,
 *      REPEAT_GAP_SEC apart. Press STOP once -> it never rings again.
 ****************************************************************/
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>        // version 7.x
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <sys/time.h>
#include <algorithm>
#include "web_page.h"           // the dashboard (second tab)

#define USE_DS3231 0            // set to 1 only if you add the DS3231 clock module

#if USE_DS3231
  #include <RTClib.h>
  RTC_DS3231 rtc;
  bool rtcOK = false;
#endif

/* ============ 1. SETTINGS YOU CAN CHANGE ============ */
const char* AP_SSID = "SmartReminder";        // the Wi-Fi name the ESP32 creates
const char* AP_PASS = "reminder123";          // at least 8 characters
const char* TZ_INFO = "IST-5:30";             // India Standard Time

const uint8_t  MAX_ALERTS     = 3;            // total alarms per reminder (use 2 or 3)
const uint32_t REPEAT_GAP_SEC = 5UL * 60UL;   // gap between alarms = 5 minutes
const uint32_t RING_SECONDS   = 30;           // each alarm rings for 30 seconds
const uint32_t SNOOZE_SEC     = 10UL * 60UL;  // SNOOZE button = ask again in 10 minutes

/* ============ 2. PINS ============ */
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_ADDR       0x3C
#define SDA_PIN         21
#define SCL_PIN         22
#define BUZZER_PIN      14
#define BTN_STOP_PIN    27      // button 1: I got it / done
#define BTN_SNOOZE_PIN  26      // button 2: remind me later
#define BTN_MENU_PIN    25      // button 3: change OLED page

#define MAX_REMINDERS   100
#define DEBOUNCE_MS     40

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WebServer server(80);

/* ============ 3. DATA MODEL ============ */
const uint8_t ST_PENDING = 0, ST_DONE = 1, ST_MISSED = 2;
const uint8_t REP_NONE = 0, REP_DAILY = 1, REP_WEEKLY = 2;

struct Reminder {
  uint32_t id;
  char     title[40];
  char     type[12];
  uint32_t startEpoch;   // when the event starts (seconds since 1970, UTC)
  uint8_t  repeat;       // 0 none, 1 daily, 2 weekly
  uint8_t  status;       // 0 pending, 1 done, 2 missed
  uint32_t alertBase;    // runtime only: time the alert sequence starts (changes on snooze)
};

Reminder reminders[MAX_REMINDERS];
int      reminderCount = 0;
uint32_t nextId = 1;

/* ---- scheduler results (refreshed every loop) ---- */
int      activeIdx       = -1;     // reminder that needs attention right now (-1 = none)
bool     ringingNow      = false;  // true while the buzzer should sound
uint8_t  alertNo         = 0;      // 1..MAX_ALERTS
uint32_t secsToNextAlert = 0;

/* ---- display state ---- */
uint8_t  page = 0;                 // 0 next, 1 agenda, 2 system info
uint32_t lastPageTouch = 0;
bool     msgActive = false;
char     msgL1[12] = "", msgL2[24] = "";
uint32_t msgUntilMs = 0;

bool timeIsSet = false;

/* ============ 4. BUZZER (works on ESP32 core 2.x and 3.x) ============ */
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void buzzerInit()       { ledcAttach(BUZZER_PIN, 2000, 8); }
  void buzzerTone(int hz) { ledcWriteTone(BUZZER_PIN, hz); }
#else
  void buzzerInit()       { ledcSetup(0, 2000, 8); ledcAttachPin(BUZZER_PIN, 0); }
  void buzzerTone(int hz) { ledcWriteTone(0, hz); }
#endif

bool buzzerOn = false;
void buzzerSet(bool on) {
  if (on == buzzerOn) return;
  buzzerOn = on;
  buzzerTone(on ? 2093 : 0);
}

void confirmBeep(int times) {           // short "click-click" feedback for buttons
  for (int i = 0; i < times; i++) { buzzerTone(2600); delay(70); buzzerTone(0); delay(70); }
  buzzerOn = false;
}

void updateBuzzer() {                   // pattern: 3 beeps, pause, repeat (no delay() used)
  if (!ringingNow) { buzzerSet(false); return; }
  uint32_t t = millis() % 1200;
  bool on = (t < 180) || (t >= 300 && t < 480) || (t >= 600 && t < 780);
  buzzerSet(on);
}

/* ============ 5. TIME ============ */
uint32_t nowEpoch() { return (uint32_t)time(nullptr); }

void applySystemTime(uint32_t t) {
  struct timeval tv;
  tv.tv_sec = t;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  timeIsSet = true;
}

void syncFromPhone(uint32_t t) {
  applySystemTime(t);
#if USE_DS3231
  if (rtcOK) rtc.adjust(DateTime(t));   // keep the clock module correct too
#endif
}

/* ============ 6. STORAGE (JSON file inside the ESP32 flash) ============ */
const char* DATA_FILE = "/reminders.json";
const char* DATA_TMP  = "/reminders.tmp";

void sortReminders() {
  std::sort(reminders, reminders + reminderCount,
            [](const Reminder &a, const Reminder &b) { return a.startEpoch < b.startEpoch; });
}

bool saveReminders() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < reminderCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"]    = reminders[i].id;
    o["title"] = reminders[i].title;
    o["type"]  = reminders[i].type;
    o["start"] = reminders[i].startEpoch;
    o["rep"]   = reminders[i].repeat;
    o["st"]    = reminders[i].status;
  }
  File f = LittleFS.open(DATA_TMP, "w");      // write to a temp file first ...
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(DATA_FILE);                 // ... then swap it in (power-cut safe)
  return LittleFS.rename(DATA_TMP, DATA_FILE);
}

void loadReminders() {
  reminderCount = 0;
  if (!LittleFS.exists(DATA_FILE) && LittleFS.exists(DATA_TMP))
    LittleFS.rename(DATA_TMP, DATA_FILE);     // recover if power failed mid-save
  File f = LittleFS.open(DATA_FILE, "r");
  if (!f) return;                             // first boot: nothing saved yet
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.println("Saved file unreadable - starting empty"); return; }
  for (JsonObject o : doc.as<JsonArray>()) {
    if (reminderCount >= MAX_REMINDERS) break;
    Reminder &r = reminders[reminderCount++];
    r.id = o["id"] | 0UL;
    strlcpy(r.title, o["title"] | "", sizeof(r.title));
    strlcpy(r.type,  o["type"]  | "Other", sizeof(r.type));
    r.startEpoch = o["start"] | 0UL;
    r.repeat     = o["rep"] | 0;
    r.status     = o["st"] | 0;
    r.alertBase  = r.startEpoch;
    if (r.id >= nextId) nextId = r.id + 1;
  }
  sortReminders();
}

/* ============ 7. THE SCHEDULER (the "brain") ============ */
// Called when a reminder is finished (user pressed STOP, or every alert was ignored).
// One-time reminders keep their final status. Repeating ones jump to the next date.
void finishReminder(Reminder &r, uint8_t finalStatus) {
  if (r.repeat != REP_NONE) {
    uint32_t period = (r.repeat == REP_DAILY) ? 86400UL : 604800UL;
    uint32_t now = nowEpoch();
    do { r.startEpoch += period; } while (r.startEpoch <= now);
    r.status = ST_PENDING;
    r.alertBase = r.startEpoch;
  } else {
    r.status = finalStatus;
  }
}

void updateEngine() {
  activeIdx = -1;
  ringingNow = false;
  if (!timeIsSet) return;                       // never alarm with a wrong clock

  uint32_t now = nowEpoch();
  const uint32_t windowEnd = (uint32_t)(MAX_ALERTS - 1) * REPEAT_GAP_SEC + RING_SECONDS;

  // Step 1: reminders whose alert window is over were ignored -> MISSED
  bool changed = false;
  for (int i = 0; i < reminderCount; i++) {
    Reminder &r = reminders[i];
    if (r.status != ST_PENDING || now < r.alertBase) continue;
    if (now - r.alertBase >= windowEnd) { finishReminder(r, ST_MISSED); changed = true; }
  }
  if (changed) { sortReminders(); saveReminders(); }

  // Step 2: which reminder is due right now? (the oldest one wins)
  uint32_t best = 0xFFFFFFFFUL;
  for (int i = 0; i < reminderCount; i++) {
    Reminder &r = reminders[i];
    if (r.status == ST_PENDING && now >= r.alertBase && r.alertBase < best) {
      best = r.alertBase;
      activeIdx = i;
    }
  }
  if (activeIdx < 0) return;

  // Step 3: which alarm number is it, and should the buzzer sound?
  uint32_t elapsed = now - reminders[activeIdx].alertBase;
  uint32_t slot    = elapsed / REPEAT_GAP_SEC;      // 0 = 1st alarm, 1 = 2nd, 2 = 3rd
  uint32_t phase   = elapsed % REPEAT_GAP_SEC;      // seconds since this alarm started
  alertNo    = slot + 1;
  ringingNow = (slot < MAX_ALERTS) && (phase < RING_SECONDS);
  secsToNextAlert = REPEAT_GAP_SEC - phase;
}

/* ============ 8. BUTTONS ============ */
struct Button { uint8_t pin; bool stable; bool last; uint32_t changedAt; };
Button btnStop   = { BTN_STOP_PIN,   HIGH, HIGH, 0 };
Button btnSnooze = { BTN_SNOOZE_PIN, HIGH, HIGH, 0 };
Button btnMenu   = { BTN_MENU_PIN,   HIGH, HIGH, 0 };

bool pressedEvent(Button &b) {                    // true once per press (debounced)
  bool reading = digitalRead(b.pin);
  if (reading != b.last) { b.last = reading; b.changedAt = millis(); }
  if (millis() - b.changedAt > DEBOUNCE_MS && reading != b.stable) {
    b.stable = reading;
    if (b.stable == LOW) return true;             // pull-up: LOW = pressed
  }
  return false;
}

void showMessage(const char* l1, const char* l2, uint32_t ms = 1500) {
  strlcpy(msgL1, l1, sizeof(msgL1));
  strlcpy(msgL2, l2, sizeof(msgL2));
  msgUntilMs = millis() + ms;
  msgActive = true;
}

void onStop() {
  if (activeIdx < 0) { confirmBeep(1); showMessage("NO ALERT", "Nothing to stop"); return; }
  finishReminder(reminders[activeIdx], ST_DONE);  // done -> will NEVER alarm again
  sortReminders();
  saveReminders();
  activeIdx = -1;
  ringingNow = false;
  buzzerSet(false);
  confirmBeep(2);
  showMessage("DONE!", "Will not alert again");
}

void onSnooze() {
  if (activeIdx < 0) { confirmBeep(1); showMessage("NO ALERT", "Nothing to snooze"); return; }
  reminders[activeIdx].alertBase = nowEpoch() + SNOOZE_SEC;   // restart the sequence later
  activeIdx = -1;
  ringingNow = false;
  buzzerSet(false);
  confirmBeep(1);
  char buf[24];
  snprintf(buf, sizeof(buf), "Back in %lu min", (unsigned long)(SNOOZE_SEC / 60));
  showMessage("SNOOZED", buf);
}

void onMenu() {
  if (activeIdx >= 0) return;                     // during an alert, only STOP/SNOOZE work
  page = (page + 1) % 3;
  lastPageTouch = millis();
  confirmBeep(1);
}

void handleButtons() {
  if (pressedEvent(btnStop))   onStop();
  if (pressedEvent(btnSnooze)) onSnooze();
  if (pressedEvent(btnMenu))   onMenu();
}

/* ============ 9. OLED SCREENS (21 characters x 8 rows) ============ */
void putLine(uint8_t row, const char* s) {
  display.setCursor(0, row * 8);
  display.print(s);
}

uint8_t putWrapped(uint8_t row, const char* s, uint8_t cols, uint8_t maxLines) {
  size_t n = strlen(s), pos = 0;
  char buf[32];
  for (uint8_t line = 0; line < maxLines && pos < n; line++) {
    size_t len = n - pos;
    if (len > cols) {                           // too long: try to break at a space
      len = cols;
      for (size_t k = cols; k > cols / 2; k--)
        if (s[pos + k] == ' ') { len = k; break; }
    }
    memcpy(buf, s + pos, len);
    buf[len] = 0;
    putLine(row++, buf);
    pos += len;
    while (s[pos] == ' ') pos++;                // skip the space we broke on
  }
  return row;
}

void drawClockHeader(uint32_t now) {
  time_t tt = now;
  struct tm t;
  localtime_r(&tt, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%H:%M %a %d %b", &t);
  putLine(0, buf);
  putLine(1, "---------------------");
}

void drawNeedTime() {
  putLine(0, "SMART REMINDER");
  putLine(1, "---------------------");
  putLine(2, "Clock not set yet!");
  putLine(3, "1 Join Wi-Fi:");
  putLine(4, AP_SSID);
  char buf[24];
  snprintf(buf, sizeof(buf), "2 Open %s", WiFi.softAPIP().toString().c_str());
  putLine(5, buf);
  putLine(6, "3 Page sets the time");
}

void drawMessage() {
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(msgL1);
  display.setTextSize(1);
  putLine(5, msgL2);
}

void drawRinging() {
  Reminder &r = reminders[activeIdx];
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("REMINDER!");
  display.setTextSize(1);
  putWrapped(2, r.title, 21, 3);
  char buf[24];
  snprintf(buf, sizeof(buf), "Alert %u of %u", alertNo, MAX_ALERTS);
  putLine(5, buf);
  putLine(7, "STOP=done  SNZ=later");
}

void drawWaiting() {                              // between two alarms
  Reminder &r = reminders[activeIdx];
  putLine(0, "PENDING - not ack'd");
  putLine(1, "---------------------");
  putWrapped(2, r.title, 21, 2);
  char buf[32];
  snprintf(buf, sizeof(buf), "Next alert %u/%u: %02lu:%02lu", alertNo + 1, MAX_ALERTS,
           (unsigned long)(secsToNextAlert / 60), (unsigned long)(secsToNextAlert % 60));
  putLine(5, buf);
  putLine(7, "STOP=done  SNZ=later");
}

void drawNextPage(uint32_t now) {
  drawClockHeader(now);
  int n = -1;
  uint32_t best = 0xFFFFFFFFUL;
  int pending = 0;
  for (int i = 0; i < reminderCount; i++) {
    if (reminders[i].status != ST_PENDING) continue;
    pending++;
    if (reminders[i].alertBase > now && reminders[i].alertBase < best) {
      best = reminders[i].alertBase;
      n = i;
    }
  }
  if (n < 0) { putLine(2, "No upcoming"); putLine(3, "reminders :)"); return; }

  Reminder &r = reminders[n];
  char buf[32];
  snprintf(buf, sizeof(buf), "NEXT [%s]:", r.type);
  putLine(2, buf);
  putWrapped(3, r.title, 21, 2);

  time_t tt = r.alertBase;
  struct tm t;
  localtime_r(&tt, &t);
  snprintf(buf, sizeof(buf), "at %02d:%02d  %02d/%02d",
           t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
  putLine(5, buf);

  unsigned long diff = r.alertBase - now;
  if (diff < 60)
    snprintf(buf, sizeof(buf), "in %lu sec", diff);
  else if (diff >= 86400)
    snprintf(buf, sizeof(buf), "in %lud %luh", diff / 86400, (diff % 86400) / 3600);
  else
    snprintf(buf, sizeof(buf), "in %luh %02lum", diff / 3600, (diff % 3600) / 60);
  putLine(6, buf);
  snprintf(buf, sizeof(buf), "Pending: %d", pending);
  putLine(7, buf);
}

void drawAgendaPage(uint32_t now) {
  putLine(0, "UPCOMING");
  putLine(1, "---------------------");
  time_t nt = now;
  struct tm today;
  localtime_r(&nt, &today);
  uint8_t row = 2;
  for (int i = 0; i < reminderCount && row < 8; i++) {
    Reminder &r = reminders[i];
    if (r.status != ST_PENDING || r.alertBase <= now) continue;
    time_t tt = r.alertBase;
    struct tm t;
    localtime_r(&tt, &t);
    char line[32];
    if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year)
      snprintf(line, sizeof(line), "%02d:%02d %.15s", t.tm_hour, t.tm_min, r.title);
    else
      snprintf(line, sizeof(line), "%02d/%02d %.15s", t.tm_mday, t.tm_mon + 1, r.title);
    putLine(row++, line);
  }
  if (row == 2) putLine(2, "Nothing scheduled");
}

void drawSystemPage() {
  putLine(0, "SYSTEM INFO");
  putLine(1, "---------------------");
  char buf[32];
  snprintf(buf, sizeof(buf), "IP %s", WiFi.softAPIP().toString().c_str());
  putLine(2, buf);
  snprintf(buf, sizeof(buf), "Phones connected: %d", WiFi.softAPgetStationNum());
  putLine(3, buf);
  snprintf(buf, sizeof(buf), "Saved: %d/%d", reminderCount, MAX_REMINDERS);
  putLine(4, buf);
  snprintf(buf, sizeof(buf), "Flash free: %lu KB",
           (unsigned long)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
  putLine(5, buf);
#if USE_DS3231
  putLine(6, "Clock: DS3231 module");
#else
  putLine(6, "Clock: phone sync");
#endif
}

void updateDisplay() {
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw < 250) return;          // redraw 4 times a second
  lastDraw = millis();

  if (page != 0 && millis() - lastPageTouch > 20000) page = 0;   // auto-return
  if (msgActive && (int32_t)(msgUntilMs - millis()) <= 0) msgActive = false;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  uint32_t now = nowEpoch();
  if (msgActive)            drawMessage();
  else if (!timeIsSet)      drawNeedTime();
  else if (activeIdx >= 0)  { if (ringingNow) drawRinging(); else drawWaiting(); }
  else if (page == 0)       drawNextPage(now);
  else if (page == 1)       drawAgendaPage(now);
  else                      drawSystemPage();
  display.display();
}

/* ============ 10. WEB SERVER (runs on the ESP32 itself) ============ */
void sendJson(int code, const char* body) { server.send(code, "application/json", body); }

void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

void handleList() {
  JsonDocument doc;
  doc["now"]     = nowEpoch();
  doc["timeSet"] = timeIsSet;
  doc["max"]     = MAX_REMINDERS;
  doc["freeKB"]  = (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024;
  JsonArray items = doc["items"].to<JsonArray>();
  for (int i = 0; i < reminderCount; i++) {
    JsonObject o = items.add<JsonObject>();
    o["id"]    = reminders[i].id;
    o["title"] = reminders[i].title;
    o["type"]  = reminders[i].type;
    o["start"] = reminders[i].startEpoch;
    o["rep"]   = reminders[i].repeat;
    o["st"]    = reminders[i].status;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAdd() {
  if (reminderCount >= MAX_REMINDERS) {
    sendJson(200, "{\"ok\":false,\"err\":\"Storage full\"}");
    return;
  }
  String title = server.arg("title");
  title.trim();
  long start = server.arg("start").toInt();
  if (title.length() == 0 || start < 1700000000L) {
    sendJson(200, "{\"ok\":false,\"err\":\"Bad title or time\"}");
    return;
  }
  String type = server.arg("type");
  if (type.length() == 0) type = "Other";

  Reminder &r = reminders[reminderCount++];
  r.id = nextId++;
  strlcpy(r.title, title.c_str(), sizeof(r.title));
  strlcpy(r.type, type.c_str(), sizeof(r.type));
  r.startEpoch = (uint32_t)start;
  r.repeat     = constrain(server.arg("repeat").toInt(), 0, 2);
  r.status     = ST_PENDING;
  r.alertBase  = r.startEpoch;
  sortReminders();
  saveReminders();
  sendJson(200, "{\"ok\":true}");
}

void handleDelete() {
  uint32_t id = (uint32_t)server.arg("id").toInt();
  for (int i = 0; i < reminderCount; i++) {
    if (reminders[i].id != id) continue;
    for (int j = i; j < reminderCount - 1; j++) reminders[j] = reminders[j + 1];
    reminderCount--;
    saveReminders();
    break;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleDone() {                               // "Done" button on the web page
  uint32_t id = (uint32_t)server.arg("id").toInt();
  for (int i = 0; i < reminderCount; i++) {
    if (reminders[i].id == id && reminders[i].status == ST_PENDING) {
      finishReminder(reminders[i], ST_DONE);
      sortReminders();
      saveReminders();
      break;
    }
  }
  sendJson(200, "{\"ok\":true}");
}

void handleClear() {                              // remove finished + missed items
  int w = 0;
  for (int i = 0; i < reminderCount; i++)
    if (reminders[i].status == ST_PENDING) reminders[w++] = reminders[i];
  reminderCount = w;
  saveReminders();
  sendJson(200, "{\"ok\":true}");
}

void handleSetTime() {                            // phone tells the ESP32 the time
  uint32_t t = (uint32_t)server.arg("t").toInt();
  if (t > 1700000000UL) {
    uint32_t now = nowEpoch();
    uint32_t diff = (t > now) ? (t - now) : (now - t);
    if (!timeIsSet || diff > 3) syncFromPhone(t);
  }
  sendJson(200, "{\"ok\":true}");
}

/* ============ 11. SETUP + LOOP ============ */
void setup() {
  Serial.begin(115200);
  pinMode(BTN_STOP_PIN, INPUT_PULLUP);
  pinMode(BTN_SNOOZE_PIN, INPUT_PULLUP);
  pinMode(BTN_MENU_PIN, INPUT_PULLUP);
  buzzerInit();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    Serial.println("OLED not found - check wiring / address 0x3C");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Smart Reminder");
  display.println("EDGE MODE (offline)");
  display.println("Starting...");
  display.display();

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadReminders();
  Serial.printf("Loaded %d reminders from flash\n", reminderCount);

  setenv("TZ", TZ_INFO, 1);
  tzset();

#if USE_DS3231
  rtcOK = rtc.begin();
  if (rtcOK && !rtc.lostPower()) applySystemTime(rtc.now().unixtime());
#endif

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/list",    HTTP_GET,  handleList);
  server.on("/api/add",     HTTP_POST, handleAdd);
  server.on("/api/delete",  HTTP_POST, handleDelete);
  server.on("/api/done",    HTTP_POST, handleDone);
  server.on("/api/clear",   HTTP_POST, handleClear);
  server.on("/api/settime", HTTP_POST, handleSetTime);
  server.begin();
  Serial.printf("Hotspot '%s' ready - open http://%s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();    // 1. answer the phone's web page (if connected)
  handleButtons();          // 2. read STOP / SNOOZE / MENU
  updateEngine();           // 3. decide: is something due? which alarm number?
  updateBuzzer();           // 4. sound the buzzer pattern if ringing
  updateDisplay();          // 5. draw the OLED
  delay(2);
}
