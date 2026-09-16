// =========================================================================
// FILE:   HUB.ino
// FOLDER: C:\Users\skyle\Downloads\Arduino Internship In House Project\HUB
// BOARD:  plain ESP32 (the one WITHOUT a screen) - was COM16
// =========================================================================
// TO UPLOAD FROM POWERSHELL - copy the whole line below and paste it in:
//
// & "C:\Users\skyle\Downloads\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe" compile --upload -p COM16 --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" "C:\Users\skyle\Downloads\Arduino Internship In House Project\HUB"
//
// If it says the port is wrong, find the right COM number with:
//
// & "C:\Users\skyle\Downloads\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe" board list
//
//   vid 0x10C4 = this hub board       vid 0x1A86 = the CYD touchscreen
//
// TO UPLOAD FROM THE ARDUINO IDE instead:
//   Tools > Board            > ESP32 Dev Module
//   Tools > Partition Scheme > Huge APP (3MB No OTA)      <-- required
//   Tools > Port             > your COM port
//   then press Upload.
// =========================================================================
// P5 EXHIBIT - HUB   (the main ESP32 board)
// =========================================================================
// This is the brain of the exhibit. It:
//   - joins WiFi at the fixed address 192.168.1.99
//   - lights an RGB LED in the colour of the chosen zone
//   - beeps a passive buzzer as you move around
//   - is controlled by a joystick + a button
//   - tells Final.html (the 3D model) what to show
//
// PARTS USED:  RGB LED, KY-004 button, KY-023 joystick, passive buzzer.
// THAT IS ALL. Every one of them is tested and confirmed working.
//
// NOT USED: the gesture sensor (it never worked - the joystick's UP/DOWN
// direction does that job instead), and the servo / PCA9685 / buck
// converter. The servo code is still in here but switched OFF below, so
// it can be turned on later without rewriting anything.
//
// -------------------------------------------------------------------------
// SWITCHES - set to 1 for on, 0 for off
// -------------------------------------------------------------------------
#define ENABLE_SERVO   0     // PCA9685 servo driver board - NOT USED
#define ENABLE_BUZZER  1     // passive buzzer from the 37-sensor kit
//
// -------------------------------------------------------------------------
// HOW TO USE IT
// -------------------------------------------------------------------------
// There are TWO LEVELS. You start OUTSIDE, looking at the 4 zones.
// Holding the button takes you INSIDE a zone, where its exhibits live.
//
//   OUTSIDE a zone  (choosing between the 4 zones)
//   ----------------------------------------------
//   Joystick LEFT / RIGHT .. move the highlight across the 4 zone buttons
//                            (Generation / Transmission / Distribution /
//                            Overview). Nothing is chosen yet.
//   Joystick UP ............ GO INSIDE the highlighted zone. You land on
//                            its FIRST exhibit automatically.
//   Joystick CLICK ......... choose the highlighted zone (stay outside).
//
//   INSIDE a zone  (choosing between that zone's exhibits)
//   ------------------------------------------------------
//   Joystick LEFT / RIGHT .. move between the exhibits in this zone.
//   Joystick CLICK ......... open the highlighted exhibit (the web page
//                            flies to it).
//   KY-004 BUTTON .......... COME BACK OUT to the zone list.
//   Joystick DOWN .......... also comes back out (same as the button).
//
//   In one line:
//      joystick UP    = go IN
//      KY-004 BUTTON  = go OUT
//      LEFT / RIGHT   = browse whichever level you are on
//      joystick CLICK = pick the thing you are pointing at
//
// Both the KY-004 button and the joystick click do the same thing, so you
// can use whichever is easier to reach.
//
// See WIRING.txt in this folder for where every single wire goes.
// =========================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>     // lets the phone use http://p5hub.local instead of an IP
#include <esp_now.h>
#if ENABLE_SERVO
  #include <Wire.h>
  #include <Adafruit_PWMServoDriver.h>
#endif

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// USE_STATIC_IP 1 = force the address below (only works if it matches your
//                   network's subnet - e.g. 192.168.1.x on a 192.168.1.x router)
// USE_STATIC_IP 0 = let the router hand out an address automatically (DHCP).
//                   Safer when moving between networks. The board PRINTS the
//                   address it got on Serial at boot - read it there, then put
//                   that address into Final.html's ESP32_HOST line.
#define USE_STATIC_IP 0

IPAddress STATIC_IP (192, 168, 1, 99);
IPAddress GATEWAY   (192, 168, 1, 254);
IPAddress SUBNET    (255, 255, 255, 0);

// THREE SEPARATE ZONE LEDS - one dedicated LED per zone, instead of one
// mixed-colour LED. Same 3 pins as the old single RGB LED, just now each
// pin drives its own independent LED. Each LED is wired to ALWAYS show its
// zone's one fixed colour (see WIRING.txt) - so each only needs ON/OFF,
// no colour-mixing pins.
#define LED_GEN_PIN    18      // Generation LED  (green)
#define LED_XMIT_R_PIN 19      // Transmission LED (KY-011) - R leg, own pin
#define LED_XMIT_G_PIN 23      // Transmission LED (KY-011) - G leg, own pin. Both
                               // pins are switched together in code, so R+G still
                               // light at the same time = amber - just 2 wires
                               // instead of 1, easier to connect.
#define LED_DIST_R_PIN 5       // Distribution LED (KY-016) - R leg, own pin
#define LED_DIST_B_PIN 25      // Distribution LED (KY-016) - B leg, own pin. Both
                               // pins switch together in code, so R+B still
                               // light at the same time = purple - just 2 wires
                               // instead of 1, easier to connect. G leg unused.
#define LED_ACTIVE_LOW 0       // 0 = GPIO HIGH lights the LED (normal LED+resistor+GND wiring)

#define BTN_PIN        4       // KY-004 button signal

#define JOY_X_PIN      34      // joystick VRx  (must be 34/35 - ADC1)
#define JOY_Y_PIN      35      // joystick VRy
#define JOY_SW_PIN     32      // joystick click
#define JOY_MARGIN     900     // how far to push before it counts
#define JOY_REPEAT_MS  350     // hold-to-scroll speed
// Which reading means "pushed up". If UP and DOWN feel swapped when you
// try it, change this 1 to 0 (or 0 back to 1) and re-upload.
#define JOY_UP_IS_HIGH 1

#if ENABLE_SERVO
  #define I2C_SDA_PIN  21      // PCA9685 SDA   (pins freed by removing the
  #define I2C_SCL_PIN  22      // PCA9685 SCL    gesture sensor)
  #define PCA_ADDRESS  0x40    // default address of the PCA9685
  #define SERVO_CHAN   0       // which output socket the servo is plugged into
  // A servo is told its angle by the LENGTH of a pulse. At 50Hz these two
  // numbers are the pulse for 0 degrees and for 180 degrees. If your servo
  // does not reach the ends, or strains at the ends, adjust these.
  #define SERVO_MIN    150
  #define SERVO_MAX    600
#endif

#if ENABLE_BUZZER
  #define BUZZER_PIN   26      // passive buzzer signal
#endif

#define HOLD_MS        45000   // go back to Overview after 45 seconds
#define DEBOUNCE_MS    40
#define LONGPRESS_MS   2000    // hold this long to go INSIDE a zone, or come back OUT

// --- Where the spotlight points for each zone (degrees) ------------------
// Tune these with:  http://192.168.1.99/aim?angle=90
#define ANGLE_GEN      45
#define ANGLE_XMIT     90
#define ANGLE_DIST     135
#define ANGLE_PARK     90

// =========================================================================
// THE MENU - matches Final.html exactly
// =========================================================================
const char* ZONE_KEYS[4] = { "gen", "xmit", "dist", "all" };
#define MAX_PANELS 7
struct ZonePanels { const char* keys[MAX_PANELS]; int n; };
ZonePanels ZP[3] = {
  { {"plant", "poly", "h2"},                                        3 },  // Generation
  { {"tower", "sub"},                                               2 },  // Transmission
  { {"disttf","distx","siemens","aidc","factory","evpark","bess1"},  7 },  // Distribution
};

WebServer server(80);

#if ENABLE_SERVO
Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(PCA_ADDRESS);
bool servoUp = false;
#endif

// --- Message from the CYD remote (must match REMOTE.ino exactly) ---------
typedef struct __attribute__((packed)) {
  char zone[8];
  char panel[8];
} ZoneMsg;

volatile bool espNowPending = false;
char espNowZone[8]  = {0};
char espNowPanel[8] = {0};

// --- Current state -------------------------------------------------------
String currentZone   = "xmit";
String currentPanel  = "";
bool   zoneActive    = false;
unsigned long zoneActivatedAt = 0;

bool lastBtnReading = HIGH;
bool btnState       = HIGH;
unsigned long lastDebounceTime = 0;

// --- Highlight / cursor --------------------------------------------------
int  cursorZone   = 0;
int  cursorPanel  = -1;
// false = you are OUTSIDE, choosing between the 4 zones.
// true  = you are INSIDE one zone, choosing between its exhibits.
bool insideZone   = false;
int  joyCentreX   = 2975;
int  joyCentreY   = 2820;
int  joyDirX      = 0;
int  joyDirY      = 0;
unsigned long joyNextRepeatX = 0;
unsigned long joyNextRepeatY = 0;

// --- Servo movement (smooth, not snapping) -------------------------------
float servoNow    = ANGLE_PARK;   // where the arm is right now
int   servoTarget = ANGLE_PARK;   // where we want it to go
unsigned long lastServoStep = 0;

// =========================================================================
// BUZZER
// =========================================================================
#if ENABLE_BUZZER
void beep(int freq, int ms) { tone(BUZZER_PIN, freq, ms); delay(ms); noTone(BUZZER_PIN); }
void soundMove()   { tone(BUZZER_PIN, 2200, 20); }
void soundSelect() { beep(1300, 70); }
void soundOpen()   { beep(1600, 55); beep(2100, 70); }
void soundBack()   { beep(700, 110); }
// Going INSIDE a zone rises in pitch; coming back OUT falls. So you can
// hear which level you are on without looking at anything.
void soundEnter()  { beep(1000, 60); beep(1500, 60); beep(2000, 80); }
void soundExit()   { beep(2000, 60); beep(1500, 60); beep(1000, 80); }
#else
void soundMove()   {}
void soundSelect() {}
void soundOpen()   {}
void soundBack()   {}
void soundEnter()  {}
void soundExit()   {}
#endif

// =========================================================================
// THREE ZONE LEDS  (Generation / Transmission / Distribution)
// =========================================================================
// zi:  0 = Generation, 1 = Transmission, 2 = Distribution, anything else = all off
void ledsSet(int zi) {
  bool g = (zi == 0), x = (zi == 1), d = (zi == 2);
#if LED_ACTIVE_LOW
  digitalWrite(LED_GEN_PIN,    g ? LOW : HIGH);
  digitalWrite(LED_XMIT_R_PIN, x ? LOW : HIGH);
  digitalWrite(LED_XMIT_G_PIN, x ? LOW : HIGH);
  digitalWrite(LED_DIST_R_PIN, d ? LOW : HIGH);
  digitalWrite(LED_DIST_B_PIN, d ? LOW : HIGH);
#else
  digitalWrite(LED_GEN_PIN,    g ? HIGH : LOW);
  digitalWrite(LED_XMIT_R_PIN, x ? HIGH : LOW);
  digitalWrite(LED_XMIT_G_PIN, x ? HIGH : LOW);
  digitalWrite(LED_DIST_R_PIN, d ? HIGH : LOW);
  digitalWrite(LED_DIST_B_PIN, d ? HIGH : LOW);
#endif
}
void ledOff() { ledsSet(-1); }

// Index of a zone key in ZONE_KEYS (0/1/2), or -1 if it is "all"/unknown.
int zoneLedIndex(const String& z) {
  if (z == "gen")  return 0;
  if (z == "xmit") return 1;
  if (z == "dist") return 2;
  return -1;
}

// 3 blinks of THAT zone's own LED, then hold it on. This is the "a zone was
// chosen" signal - unchanged in meaning from the old single-LED version,
// just shown on a dedicated LED instead of a colour mix.
void flashZone(const String& z) {
  int zi = zoneLedIndex(z);
  for (int i = 0; i < 3; i++) { ledsSet(zi); delay(90); ledOff(); delay(90); }
  ledsSet(zi);
}

// "An exhibit was opened" signal. The old version did 2 WHITE blinks (only
// possible with a real RGB LED). With 3 separate single-colour LEDs there is
// no white, so instead: 2 quick blinks of the CURRENT zone's own LED, then
// hold it on again - still clearly different from the slower 3-blink
// "zone chosen" pattern above.
void flashPanel(const String& z) {
  int zi = zoneLedIndex(z);
  for (int i = 0; i < 2; i++) { ledsSet(zi); delay(60); ledOff(); delay(60); }
  ledsSet(zi);
}

void ledRestore() {
  if (!zoneActive) { ledOff(); return; }
  ledsSet(zoneLedIndex(currentZone));
}

// Joystick moved the highlight but nothing is chosen yet: one short blip of
// the HIGHLIGHTED zone's LED (not held), then back to whatever is really
// active. Highlighting "Overview" (zi==3) has no LED of its own, so it
// blinks all three together once instead - a different, recognisable
// pattern that still means "previewing Overview".
void blipCursor(int zi) {
  if (zi == 3) {
    digitalWrite(LED_GEN_PIN,    LED_ACTIVE_LOW ? LOW : HIGH);
    digitalWrite(LED_XMIT_R_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
    digitalWrite(LED_XMIT_G_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
    digitalWrite(LED_DIST_R_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
    digitalWrite(LED_DIST_B_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
    delay(110);
  } else {
    ledOff(); delay(40);
    ledsSet(zi); delay(110);
  }
  ledRestore();
}

// =========================================================================
// SERVO  (PCA9685)
// =========================================================================
// aimServo() only sets a TARGET. The arm is walked towards that target a
// little at a time in stepServo(), so the spotlight glides instead of
// snapping - much nicer to watch, and far kinder to the servo gears.
void aimServo(int angle) {
  servoTarget = constrain(angle, 0, 180);
  Serial.printf("[servo] target -> %d deg\n", servoTarget);
}

void stepServo() {
#if ENABLE_SERVO
  if (!servoUp) return;
  if (millis() - lastServoStep < 15) return;      // move every 15ms
  lastServoStep = millis();
  if (fabs(servoTarget - servoNow) < 0.5) return; // already there
  servoNow += (servoTarget > servoNow) ? 1.2 : -1.2;   // speed, degrees per step
  int pulse = map((int)servoNow, 0, 180, SERVO_MIN, SERVO_MAX);
  pca.setPWM(SERVO_CHAN, 0, pulse);
#endif
}

int angleForZone(const String& zone) {
  if (zone == "gen")  return ANGLE_GEN;
  if (zone == "dist") return ANGLE_DIST;
  return ANGLE_XMIT;
}

void setRing(const String& zone, bool on) {
  Serial.printf("[ring]  %s -> %s (not built yet)\n", zone.c_str(), on ? "on" : "off");
}

// =========================================================================
// Zone control
// =========================================================================
void enterZone(const String& zone, const String& panel) {
  currentZone  = zone;
  currentPanel = panel;
  zoneActive = true;
  zoneActivatedAt = millis();
  Serial.printf(">> ZONE %s%s%s\n", zone.c_str(),
                panel.length() ? " / " : "", panel.c_str());
  aimServo(angleForZone(zone));
  setRing(zone, true);
  if (panel.length()) { flashPanel(zone); soundOpen(); }
  else                { flashZone(zone);  soundSelect(); }
}
void enterZone(const String& zone) { enterZone(zone, ""); }

void enterOverview() {
  zoneActive = false;
  currentPanel = "";
  cursorPanel = -1;
  insideZone = false;        // Overview always puts you back on the zone list
  aimServo(ANGLE_PARK);
  setRing(currentZone, false);
  ledOff();
  soundBack();
  Serial.println(">> OVERVIEW");
}

// =========================================================================
// Input
// =========================================================================
int zoneIndex(const String& z) {
  for (int i = 0; i < 4; i++) if (z == ZONE_KEYS[i]) return i;
  return -1;
}

// ---- OUTSIDE: short press chooses the highlighted zone ------------------
void commitCursor() {
  cursorPanel = -1;
  if (cursorZone == 3) { Serial.println("PRESS -> OVERVIEW"); enterOverview(); }
  else { Serial.printf("PRESS -> zone %s\n", ZONE_KEYS[cursorZone]); enterZone(ZONE_KEYS[cursorZone], ""); }
}

// ---- INSIDE: short press opens the highlighted exhibit ------------------
void openHighlightedPanel() {
  int zi = zoneIndex(currentZone);
  if (!zoneActive || zi < 0 || zi > 2) return;
  if (cursorPanel < 0) cursorPanel = 0;
  Serial.printf("PRESS -> open exhibit %s\n", ZP[zi].keys[cursorPanel]);
  enterZone(currentZone, ZP[zi].keys[cursorPanel]);
}

// ---- HOLD 2s: go INSIDE the highlighted zone ----------------------------
// You land on the FIRST exhibit in that zone straight away.
void goInside() {
  if (cursorZone == 3) {          // Overview has nothing inside it
    Serial.println("HOLD -> Overview has no exhibits inside");
    soundBack();
    return;
  }
  // If that zone was not chosen yet, choose it now, so one long hold is
  // enough to go from the zone list straight into the zone.
  if (!zoneActive || currentZone != String(ZONE_KEYS[cursorZone]))
    enterZone(ZONE_KEYS[cursorZone], "");

  insideZone  = true;
  cursorPanel = 0;                // land on the first exhibit
  int zi = zoneIndex(currentZone);
  Serial.printf("HOLD -> INSIDE %s, on exhibit %s\n",
                currentZone.c_str(), (zi >= 0 && zi <= 2) ? ZP[zi].keys[0] : "?");
  soundEnter();
  // two quick blinks of this zone's own LED so you can see you changed level
  for (int i = 0; i < 2; i++) { ledOff(); delay(55); ledRestore(); delay(55); }
}

// ---- HOLD 2s again: come back OUT to the zone list ----------------------
void goOutside() {
  insideZone   = false;
  cursorPanel  = -1;
  currentPanel = "";              // clear the exhibit, keep the zone
  zoneActivatedAt = millis();     // restart the 45s timer
  Serial.println("HOLD -> back OUT to the zone list");
  soundExit();
  ledRestore();
}

// One place that decides what a long hold means, based on where you are.
void toggleInside() { if (insideZone) goOutside(); else goInside(); }

// One place that decides what a short press means, based on where you are.
void shortPress()   { if (insideZone) openHighlightedPanel(); else commitCursor(); }

// LEFT/RIGHT = move the zone highlight.  UP/DOWN = move the exhibit
// highlight inside the chosen zone (this replaces the gesture sensor).
void pollJoystick() {
  // ---- X axis: zones ----
  int x = analogRead(JOY_X_PIN);
  int dx = 0;
  if (x > joyCentreX + JOY_MARGIN)      dx = +1;
  else if (x < joyCentreX - JOY_MARGIN) dx = -1;

  bool fireX = false;
  if (dx != joyDirX) {
    joyDirX = dx;
    if (dx != 0) { fireX = true; joyNextRepeatX = millis() + 600; }
  } else if (dx != 0 && millis() > joyNextRepeatX) {
    fireX = true; joyNextRepeatX = millis() + JOY_REPEAT_MS;
  }
  if (fireX) {
    int zi = zoneIndex(currentZone);
    if (insideZone && zi >= 0 && zi <= 2) {
      // INSIDE a zone: left/right steps through that zone's exhibits.
      int n = ZP[zi].n;
      cursorPanel = (cursorPanel < 0) ? 0 : (cursorPanel + dx + n) % n;
      Serial.printf("JOY %s -> exhibit %s (%d of %d)\n",
                    dx > 0 ? "right" : "left", ZP[zi].keys[cursorPanel], cursorPanel + 1, n);
      soundMove();
      for (int i = 0; i < 2; i++) { ledOff(); delay(45); ledRestore(); delay(45); }
    } else {
      // OUTSIDE: left/right moves the highlight across the four zones.
      cursorZone = (cursorZone + dx + 4) % 4;
      cursorPanel = -1;
      Serial.printf("JOY %s -> highlight %s\n", dx > 0 ? "right" : "left", ZONE_KEYS[cursorZone]);
      soundMove();
      blipCursor(cursorZone);
    }
  }

  // ---- Y axis: UP goes INTO a zone, DOWN comes back OUT ----------------
  // Push up  = step deeper (zone list -> inside that zone)
  // Push down = step back out (inside a zone -> zone list)
  // Only fires once per push; you must let the stick return to centre
  // before it will fire again, so it can't run away from you.
  int y = analogRead(JOY_Y_PIN);
  int dy = 0;
  if (y > joyCentreY + JOY_MARGIN)      dy = +1;
  else if (y < joyCentreY - JOY_MARGIN) dy = -1;

  // Only act on a NEW push (no hold-to-repeat here - going in and out
  // should be one deliberate flick at a time).
  bool fireY = (dy != joyDirY && dy != 0);
  joyDirY = dy;

  if (fireY) {
    // Which way is "up" depends on how the stick is physically mounted.
    // If up and down feel swapped, change JOY_UP_IS_HIGH at the top of
    // this file from 1 to 0 (or back).
    bool pushedUp = (JOY_UP_IS_HIGH ? (dy > 0) : (dy < 0));

    if (pushedUp) {
      if (!insideZone) {
        Serial.println("JOY up -> going INSIDE the zone");
        goInside();
      } else {
        Serial.println("JOY up -> already inside (push down to come out)");
      }
    } else {
      if (insideZone) {
        Serial.println("JOY down -> coming back OUT to the zone list");
        goOutside();
      } else {
        Serial.println("JOY down -> already at the zone list");
      }
    }
  }
}

// =========================================================================
// ESP-NOW (from the CYD remote)
// =========================================================================
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len < 8 || len > (int)sizeof(ZoneMsg)) return;
  ZoneMsg m = {};
  memcpy(&m, data, len);
  m.zone[sizeof(m.zone) - 1]   = '\0';
  m.panel[sizeof(m.panel) - 1] = '\0';
  strncpy(espNowZone,  m.zone,  sizeof(espNowZone));
  strncpy(espNowPanel, m.panel, sizeof(espNowPanel));
  espNowPending = true;
}

bool initEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init FAILED"); return false; }
  esp_now_register_recv_cb(onEspNowRecv);
  Serial.println("ESP-NOW ready.");
  return true;
}

// =========================================================================
// Web endpoints
// =========================================================================
void cors() { server.sendHeader("Access-Control-Allow-Origin", "*"); }

void handleState() {
  cors();
  int zi = zoneIndex(currentZone);
  String curPanelKey = "";
  if (zi >= 0 && zi <= 2 && cursorPanel >= 0 && cursorPanel < ZP[zi].n)
    curPanelKey = ZP[zi].keys[cursorPanel];
  server.send(200, "application/json",
    String("{\"zone\":\"") + currentZone + "\",\"active\":" + (zoneActive ? "true" : "false") +
    ",\"panel\":\"" + currentPanel + "\"" +
    ",\"cursor\":\"" + ZONE_KEYS[cursorZone] + "\"" +
    ",\"cursorPanel\":\"" + curPanelKey + "\"" +
    ",\"inside\":" + (insideZone ? "true" : "false") +
    ",\"angle\":" + (int)servoNow + "}");
}

void handleTrigger() {
  String zone  = server.hasArg("zone")  ? server.arg("zone")  : "";
  String panel = server.hasArg("panel") ? server.arg("panel") : "";
  if (panel.length() > 7) panel = "";
  cors();
  if (zone == "gen" || zone == "xmit" || zone == "dist") {
    server.send(200, "application/json",
      String("{\"ok\":true,\"zone\":\"") + zone + "\",\"active\":true,\"panel\":\"" + panel + "\"}");
    enterZone(zone, panel);
  } else if (zone == "all") {
    server.send(200, "application/json", "{\"ok\":true,\"zone\":\"all\",\"active\":false,\"panel\":\"\"}");
    enterOverview();
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"zone must be gen|xmit|dist|all\"}");
  }
}

// Point the spotlight by hand - use this to find each zone's angle.
//   http://192.168.1.99/aim?angle=90
void handleAim() {
  cors();
  if (server.hasArg("angle")) {
    int a = constrain(server.arg("angle").toInt(), 0, 180);
    aimServo(a);
    server.send(200, "application/json", String("{\"angle\":") + a + "}");
  } else {
    server.send(200, "application/json",
      String("{\"angle\":") + (int)servoNow + ",\"target\":" + servoTarget + "}");
  }
}

void handleJoy() {
  cors();
  int x = analogRead(JOY_X_PIN);
  int y = analogRead(JOY_Y_PIN);
  bool pressed = (digitalRead(JOY_SW_PIN) == LOW);
  server.send(200, "application/json",
    String("{\"x\":") + x + ",\"y\":" + y + ",\"press\":" + (pressed ? "true" : "false") + "}");
}

#if ENABLE_SERVO
// Is the PCA9685 there? Should show 0x40.   http://192.168.1.99/i2c
void handleI2c() {
  cors();
  String found = ""; int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (n++) found += ",";
      char buf[8]; snprintf(buf, sizeof(buf), "\"0x%02X\"", a);
      found += buf;
    }
  }
  server.send(200, "application/json",
    String("{\"servoReady\":") + (servoUp ? "true" : "false") +
    ",\"count\":" + n + ",\"found\":[" + found + "]}");
}

// Sweep the servo end to end so you can see it move.
void handleSweep() {
  cors();
  server.send(200, "text/plain", "sweeping 45 -> 135 -> 90");
  aimServo(45);  delay(1200);
  aimServo(135); delay(1200);
  aimServo(90);
}
#endif

#if ENABLE_BUZZER
void handleBeep() {
  cors();
  server.send(200, "text/plain", "beeping");
  soundSelect(); delay(120); soundOpen(); delay(120); soundBack();
}
#endif

// =========================================================================
// PHONE REMOTE  - a touch remote served by the hub itself.
// =========================================================================
// Open http://<hub ip>/ on any phone joined to the same WiFi. This replaces
// the CYD touchscreen: same 4 zones, same exhibits inside each, same deep
// navigation. Everything is embedded here - the phone needs NO internet,
// only to be on the same network as the hub.
const char REMOTE_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>P5 Remote</title><style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:#0f1115;color:#fff;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;padding:12px;user-select:none}
h1{font-size:15px;letter-spacing:.14em;color:#9aa0aa;font-weight:600;margin:0 0 4px}
#st{font-size:13px;color:#9aa0aa;margin:0 0 12px;min-height:18px}
#st b{color:#fff}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.z{border:none;border-radius:14px;padding:22px 8px;font-size:19px;font-weight:700;color:#10140f;
   font-family:inherit;cursor:pointer;transition:transform .08s,box-shadow .15s}
.z small{display:block;font-size:11px;font-weight:600;opacity:.75;margin-top:3px}
.z:active{transform:scale(.96)}
.z.on{box-shadow:0 0 0 3px #fff}
#gen{background:#22c55e}#xmit{background:#f59e0b}#dist{background:#a78bfa}
#all{background:#262b33;color:#fff;box-shadow:inset 0 0 0 2px #4a5160}
#all.on{box-shadow:0 0 0 3px #fff}
h2{font-size:12px;letter-spacing:.12em;color:#9aa0aa;margin:20px 0 8px;font-weight:600}
.ex{display:block;width:100%;text-align:left;background:#1a1e26;color:#fff;border:1px solid #2a2f38;
    border-radius:10px;padding:15px 14px;font-size:15px;margin-bottom:7px;font-family:inherit;cursor:pointer}
.ex:active{background:#252b36}
.ex.on{background:#fff;color:#10140f;font-weight:700}
.bar{height:4px;border-radius:2px;margin-bottom:14px;background:#2a2f38}
</style></head><body>
<h1>P5 SPOTLIGHT REMOTE</h1>
<div id="st">connecting...</div>
<div class="bar" id="bar"></div>
<div class="grid">
<button class="z" id="gen"  onclick="go('gen')">GEN<small>Generation</small></button>
<button class="z" id="xmit" onclick="go('xmit')">XMIT<small>Transmission</small></button>
<button class="z" id="dist" onclick="go('dist')">DIST<small>Distribution</small></button>
<button class="z" id="all"  onclick="go('all')">OVERVIEW<small>reset</small></button>
</div>
<h2 id="exh" style="display:none">INSIDE THIS ZONE</h2>
<div id="list"></div>
<script>
var EX={gen:[["plant","Power Plant"],["poly","SP Poly"],["h2","H2 Fuel Cell"]],
xmit:[["tower","Transmission Tower"],["sub","Substation"]],
dist:[["disttf","Dist TX"],["distx","Control Room"],["siemens","Siemens"],["aidc","AI Data Centre"],["factory","Factory"],["evpark","EV + BESS"],["bess1","BESS Units"]]};
var COL={gen:"#22c55e",xmit:"#f59e0b",dist:"#a78bfa",all:"#4a5160"};
var cur="",curPanel="";
function go(z){fetch("/trigger?zone="+z).then(poll);}
function open1(z,p){fetch("/trigger?zone="+z+"&panel="+p).then(poll);}
function draw(zone,active,panel){
 ["gen","xmit","dist","all"].forEach(function(k){
   document.getElementById(k).classList.toggle("on",active?k===zone:k==="all");});
 document.getElementById("bar").style.background=active?(COL[zone]||"#2a2f38"):"#2a2f38";
 var st=document.getElementById("st");
 st.innerHTML=active?("showing <b>"+zone.toUpperCase()+"</b>"+(panel?" &rsaquo; <b>"+panel+"</b>":"")):"<b>Overview</b> - nothing selected";
 var list=document.getElementById("list"),h=document.getElementById("exh");
 if(active&&EX[zone]){h.style.display="block";
   if(cur!==zone||curPanel!==panel){list.innerHTML="";
     EX[zone].forEach(function(e){var b=document.createElement("button");
       b.className="ex"+(e[0]===panel?" on":"");b.textContent=e[1];
       b.onclick=function(){open1(zone,e[0]);};list.appendChild(b);});}
 }else{h.style.display="none";list.innerHTML="";}
 cur=zone;curPanel=panel;}
function poll(){fetch("/state",{cache:"no-store"}).then(function(r){return r.json();})
 .then(function(d){draw(d.zone,d.active,d.panel||"");})
 .catch(function(){document.getElementById("st").textContent="hub not reachable";});}
poll();setInterval(poll,1500);
</script></body></html>)rawliteral";

void handleRemote() {
  cors();
  server.send_P(200, "text/html", REMOTE_HTML);
}

// ---- DIAGNOSTIC: light each LED pin one at a time -----------------------
// Turns ON one pin for 2 seconds, in order, announcing each on Serial.
// Watch which LEDs light. Any pin that does nothing = that wire/leg is the
// problem, not the code.   GET /ledtest
void handleLedTest() {
  cors();
  server.send(200, "text/plain",
    "LED test running for ~10s. Order:\n"
    "  1. GPIO18  Generation  (KY-009 G)\n"
    "  2. GPIO19  Transmission R (KY-011 R)\n"
    "  3. GPIO23  Transmission G (KY-011 G)\n"
    "  4. GPIO5   Distribution R (KY-016 R)\n"
    "  5. GPIO25  Distribution B (KY-016 B)\n"
    "  6. ALL together\n"
    "Watch which ones light up.\n");

  const int pins[5]   = { LED_GEN_PIN, LED_XMIT_R_PIN, LED_XMIT_G_PIN, LED_DIST_R_PIN, LED_DIST_B_PIN };
  const char* names[5] = { "GPIO18 Generation", "GPIO19 Xmit-R", "GPIO23 Xmit-G",
                           "GPIO5 Dist-R", "GPIO25 Dist-B" };
  ledOff();
  for (int i = 0; i < 5; i++) {
    Serial.printf("[ledtest] ON  -> %s\n", names[i]);
    digitalWrite(pins[i], LED_ACTIVE_LOW ? LOW : HIGH);
    delay(2000);
    digitalWrite(pins[i], LED_ACTIVE_LOW ? HIGH : LOW);
    delay(300);
  }
  Serial.println("[ledtest] ALL ON");
  for (int i = 0; i < 5; i++) digitalWrite(pins[i], LED_ACTIVE_LOW ? LOW : HIGH);
  delay(2000);
  ledOff();
  Serial.println("[ledtest] done");
}

// ---- PHOTO MODE: hold ALL zone LEDs on -----------------------------------
// For taking a picture of the finished build with every LED lit at once.
//   GET /allon   -> all three zone LEDs stay on until something changes them
//   GET /alloff  -> all off
// Normal operation resumes the moment a zone is chosen (joystick, button,
// phone remote, or GET /trigger?zone=...), so nothing needs undoing.
void handleAllOn() {
  cors();
  digitalWrite(LED_GEN_PIN,    LED_ACTIVE_LOW ? LOW : HIGH);
  digitalWrite(LED_XMIT_R_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
  digitalWrite(LED_XMIT_G_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
  digitalWrite(LED_DIST_R_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
  digitalWrite(LED_DIST_B_PIN, LED_ACTIVE_LOW ? LOW : HIGH);
  Serial.println("PHOTO MODE: all LEDs ON");
  server.send(200, "text/plain", "all LEDs ON - say when you are done");
}

void handleAllOff() {
  cors();
  ledOff();
  Serial.println("PHOTO MODE: all LEDs OFF");
  server.send(200, "text/plain", "all LEDs off");
}

// ---- DIAGNOSTIC: read the buttons right now -----------------------------
// Shows the raw electrical level on each button pin. Both idle HIGH and go
// LOW when pressed. If a pin never changes, that button is not reaching the
// ESP32.   GET /btn
void handleBtn() {
  cors();
  bool ky004 = digitalRead(BTN_PIN);
  bool joy   = digitalRead(JOY_SW_PIN);
  server.send(200, "application/json",
    String("{\"ky004_gpio4\":\"") + (ky004 ? "HIGH (not pressed)" : "LOW (PRESSED)") + "\"" +
    ",\"joystick_gpio32\":\"" + (joy ? "HIGH (not pressed)" : "LOW (PRESSED)") + "\"" +
    ",\"note\":\"both should read HIGH when untouched, LOW while held down\"}");
}

void handleSimulate() {
  cors();
  server.send(200, "text/plain", "Simulated press.");
  enterZone("xmit");
}

void handleRoot() {
  cors();
  server.send(200, "text/plain",
    String("P5 HUB up. zone=") + currentZone + " active=" + (zoneActive ? "1" : "0") + "\n"
    "GET /state                       -> status (Final.html reads this)\n"
    "GET /trigger?zone=gen|xmit|dist  -> choose a zone\n"
    "GET /trigger?zone=all            -> Overview\n"
    "GET /trigger?zone=xmit&panel=sub -> open one exhibit\n"
    "GET /aim?angle=90                -> point the spotlight by hand\n"
    "GET /joy                         -> live joystick reading\n"
#if ENABLE_SERVO
    "GET /i2c                         -> check the PCA9685 (expect 0x40)\n"
    "GET /sweep                       -> make the servo move, to test it\n"
#endif
#if ENABLE_BUZZER
    "GET /beep                        -> test the buzzer\n"
#endif
    "GET /simulate                    -> fake a press\n");
}

// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== P5 HUB starting ===");
  Serial.printf("servo=%d  buzzer=%d\n", ENABLE_SERVO, ENABLE_BUZZER);

  pinMode(LED_GEN_PIN, OUTPUT); pinMode(LED_XMIT_R_PIN, OUTPUT); pinMode(LED_XMIT_G_PIN, OUTPUT);
  pinMode(LED_DIST_R_PIN, OUTPUT); pinMode(LED_DIST_B_PIN, OUTPUT);
  ledOff();
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(JOY_SW_PIN, INPUT_PULLUP);
#if ENABLE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
#endif

  // Measure where the joystick rests. DON'T hold it while switching on.
  { long sx = 0, sy = 0;
    for (int i = 0; i < 16; i++) { sx += analogRead(JOY_X_PIN); sy += analogRead(JOY_Y_PIN); delay(5); }
    joyCentreX = sx / 16; joyCentreY = sy / 16;
    Serial.printf("Joystick centre: x=%d y=%d\n", joyCentreX, joyCentreY);
  }

#if ENABLE_SERVO
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
  delay(100);
  Wire.beginTransmission(PCA_ADDRESS);
  if (Wire.endTransmission() == 0) {
    pca.begin();
    pca.setOscillatorFrequency(27000000);
    pca.setPWMFreq(50);          // servos want 50Hz
    delay(10);
    servoUp = true;
    servoNow = ANGLE_PARK; servoTarget = ANGLE_PARK;
    pca.setPWM(SERVO_CHAN, 0, map(ANGLE_PARK, 0, 180, SERVO_MIN, SERVO_MAX));
    Serial.println("PCA9685 servo driver READY. Servo parked at centre.");
  } else {
    Serial.println("PCA9685 NOT FOUND at 0x40 - check SDA=21 SCL=22, VCC=3V3, GND shared.");
    Serial.println("Everything else still works. See GET /i2c");
  }
#endif

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
#if USE_STATIC_IP
  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, GATEWAY))
    Serial.println("Static IP failed - using DHCP.");
#else
  Serial.println("Using DHCP - the address will be printed below once connected.");
#endif
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { delay(400); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. HUB IP: "); Serial.println(WiFi.localIP());

    // Give the hub a fixed NAME on the network. The WiFi hands out a
    // different IP each time it restarts, but the name never changes - so
    // the phone remote can just use http://p5hub.local and always find it.
    if (MDNS.begin("p5hub")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("Name registered: http://p5hub.local  <-- use this on the phone");
    } else {
      Serial.println("mDNS failed to start - use the IP address above instead.");
    }
    // startup confirmation: all 3 zone LEDs flash together twice
    for (int i = 0; i < 2; i++) {
      digitalWrite(LED_GEN_PIN, HIGH); digitalWrite(LED_XMIT_R_PIN, HIGH); digitalWrite(LED_XMIT_G_PIN, HIGH);
      digitalWrite(LED_DIST_R_PIN, HIGH); digitalWrite(LED_DIST_B_PIN, HIGH);
      delay(120);
      ledOff(); delay(120);
    }
#if ENABLE_BUZZER
    beep(1800, 60); beep(2400, 90);
#endif
  } else {
    Serial.println("WiFi NOT connected - check the name/password at the top of this file.");
  }

  initEspNow();

  server.on("/", handleRemote);        // phone opens the hub IP -> touch remote
  server.on("/remote", handleRemote);
  server.on("/help", handleRoot);      // the old text list of endpoints
  server.on("/state", handleState);
  server.on("/trigger", handleTrigger);
  server.on("/aim", handleAim);
  server.on("/joy", handleJoy);
#if ENABLE_SERVO
  server.on("/i2c", handleI2c);
  server.on("/sweep", handleSweep);
#endif
#if ENABLE_BUZZER
  server.on("/beep", handleBeep);
#endif
  server.on("/allon", handleAllOn);
  server.on("/alloff", handleAllOff);
  server.on("/ledtest", handleLedTest);
  server.on("/btn", handleBtn);
  server.on("/simulate", handleSimulate);
  server.begin();
  Serial.println("Ready.");
}

void loop() {
  server.handleClient();
  stepServo();            // glide the spotlight towards its target

  if (espNowPending) {
    espNowPending = false;
    String z(espNowZone), p(espNowPanel);
    Serial.printf("ESP-NOW -> %s%s%s\n", z.c_str(), p.length() ? " / " : "", p.c_str());
    if (z == "gen" || z == "xmit" || z == "dist") enterZone(z, p);
    else if (z == "all")                          enterOverview();
  }

  pollJoystick();

  // ---- KY-004 button = the BACK button ----------------------------------
  // Its only job is to come back OUT of a zone. Joystick UP goes in, this
  // button comes out. One button, one meaning - nothing to remember.
  static bool btnLastRaw   = HIGH;
  static bool btnStable    = HIGH;
  static unsigned long btnChangedAt = 0;

  bool btnRaw = digitalRead(BTN_PIN);
  if (btnRaw != btnLastRaw) btnChangedAt = millis();
  if ((millis() - btnChangedAt) > DEBOUNCE_MS && btnRaw != btnStable) {
    btnStable = btnRaw;
    if (btnStable == LOW) {                 // pressed down
      if (insideZone) {
        Serial.println("BUTTON -> coming back OUT to the zone list");
        goOutside();
      } else {
        Serial.println("BUTTON -> already at the zone list (nothing to go back from)");
        soundBack();
      }
    }
  }
  btnLastRaw = btnRaw;

  // ---- Joystick click - does exactly the same as the button -------------
  static bool joyLastSw   = HIGH;
  static unsigned long joyDownAt = 0;
  static bool joyLongFired = false;

  bool joySw = digitalRead(JOY_SW_PIN);
  if (joySw == LOW && joyLastSw == HIGH) { joyDownAt = millis(); joyLongFired = false; }
  if (joySw == LOW && !joyLongFired && millis() - joyDownAt > LONGPRESS_MS) {
    joyLongFired = true;
    toggleInside();
  }
  if (joySw == HIGH && joyLastSw == LOW && !joyLongFired) {
    if (millis() - joyDownAt > 30) shortPress();
  }
  joyLastSw = joySw;

  // Auto-return to Overview
  if (zoneActive && millis() - zoneActivatedAt > HOLD_MS) {
    Serial.println("Hold expired ->");
    enterOverview();
  }
}
