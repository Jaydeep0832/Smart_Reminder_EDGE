/****************************************************
 * hardware_test.ino - run this BEFORE the main firmware
 * Checks: OLED, passive buzzer, and the 3 buttons.
 ****************************************************/
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define BUZZER_PIN     14
#define BTN_STOP_PIN   27
#define BTN_SNOOZE_PIN 26
#define BTN_MENU_PIN   25

Adafruit_SSD1306 display(128, 64, &Wire, -1);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void buzzerInit()       { ledcAttach(BUZZER_PIN, 2000, 8); }
  void buzzerTone(int hz) { ledcWriteTone(BUZZER_PIN, hz); }
#else
  void buzzerInit()       { ledcSetup(0, 2000, 8); ledcAttachPin(BUZZER_PIN, 0); }
  void buzzerTone(int hz) { ledcWriteTone(0, hz); }
#endif

void setup() {
  Serial.begin(115200);
  pinMode(BTN_STOP_PIN, INPUT_PULLUP);
  pinMode(BTN_SNOOZE_PIN, INPUT_PULLUP);
  pinMode(BTN_MENU_PIN, INPUT_PULLUP);
  buzzerInit();
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED NOT FOUND");
  buzzerTone(2093); delay(200); buzzerTone(0);      // one start-up beep
}

void loop() {
  bool stopP   = (digitalRead(BTN_STOP_PIN)   == LOW);
  bool snoozeP = (digitalRead(BTN_SNOOZE_PIN) == LOW);
  bool menuP   = (digitalRead(BTN_MENU_PIN)   == LOW);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);   display.println("HARDWARE TEST - OK?");
  display.setCursor(0, 20);  display.println(stopP   ? "STOP   : PRESSED" : "STOP   : -");
  display.setCursor(0, 32);  display.println(snoozeP ? "SNOOZE : PRESSED" : "SNOOZE : -");
  display.setCursor(0, 44);  display.println(menuP   ? "MENU   : PRESSED" : "MENU   : -");
  display.display();

  if (stopP)        buzzerTone(1000);    // low tone
  else if (snoozeP) buzzerTone(1500);    // medium tone
  else if (menuP)   buzzerTone(2000);    // high tone
  else              buzzerTone(0);       // silence
  delay(30);
}
