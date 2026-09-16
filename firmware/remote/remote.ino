// =========================================================================
// FILE:   CYD touchscreen code.ino
// FOLDER: C:\Users\skyle\Downloads\Arduino Internship In House Project\CYD touchscreen code
// BOARD:  the CYD touchscreen (CH340, vid 0x1A86) - was COM18
// =========================================================================
// TO UPLOAD FROM POWERSHELL - copy this whole line and paste it in:
//
// & "C:\Users\skyle\Downloads\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe" compile --upload -p COM18 --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" --build-property "compiler.cpp.extra_flags=-DUSER_SETUP_LOADED=1 -DILI9341_2_DRIVER=1 -DTFT_WIDTH=240 -DTFT_HEIGHT=320 -DTFT_MISO=12 -DTFT_MOSI=13 -DTFT_SCLK=14 -DTFT_CS=15 -DTFT_DC=2 -DTFT_RST=-1 -DTFT_BL=21 -DTFT_BACKLIGHT_ON=HIGH -DLOAD_GLCD=1 -DLOAD_FONT2=1 -DLOAD_FONT4=1 -DSPI_FREQUENCY=55000000 -DUSE_HSPI_PORT=1" "C:\Users\skyle\Downloads\Arduino Internship In House Project\CYD touchscreen code"
//
// Not sure which COM port? Run this:
// & "C:\Users\skyle\Downloads\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe" board list
//     vid 0x1A86 = this CYD touchscreen     vid 0x10C4 = the plain ESP32 hub
//
// ARDUINO IDE WARNING for this sketch:
//   The IDE cannot set the screen settings the way the line above does.
//   Uploading this one from the IDE gives a WHITE or BLANK screen unless
//   you first edit User_Setup.h inside the TFT_eSPI library by hand.
//   USE THE POWERSHELL LINE ABOVE for this sketch - it is much easier.
// =========================================================================
// P5 Exhibit - Presenter Remote (CYD / ESP32-2432S028).
// -------------------------------------------------------------------------
// Handheld clicker for the servo-spotlight exhibit, now with a TWO-LEVEL menu:
//
//   HOME screen ......... the original 2x2 grid (GEN / XMIT / DIST / OVERVIEW).
//                         Tapping GEN/XMIT/DIST fires that zone AND opens its
//                         zone screen. OVERVIEW fires "all" and stays here.
//   ZONE screen (x3) .... shows what's INSIDE that zone (matches Final.html):
//                           GEN  -> Power Plant / SP Poly / H2 Fuel Cell
//                           XMIT -> Transmission Tower / Substation
//                           DIST -> Dist TX / Control Room / Siemens / AI Data
//                                   Centre / Factory / EV + BESS / BESS Units
//                         plus two nav buttons at the bottom that follow the
//                         energy journey (GEN -> XMIT -> DIST -> OVERVIEW),
//                         and a small MENU chip (top-right) that returns to
//                         HOME without firing any trigger.
//
// Every zone tap / nav tap sends the zone to the hub: ESP-NOW first (fast,
// ACKed), HTTP /trigger as fallback. Tapping a sub-item re-fires its zone
// (blinks the hub LED + resets the hub's 45s hold) and highlights it here.
// NOTE: sub-items do NOT yet deep-navigate Final.html to that exhibit - that
// needs hub + page changes; this build is CYD-only by request.
//
// Touch = XPT2046_Bitbang + -DUSE_HSPI_PORT=1 (stack proven on this unit).
//
// Build (from Downloads):
//   & "C:\Users\skyle\Downloads\arduino-cli_1.5.1_Windows_64bit\arduino-cli.exe" \
//     compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" \
//     --build-property "compiler.cpp.extra_flags=-DUSER_SETUP_LOADED=1 -DILI9341_2_DRIVER=1 \
//       -DTFT_WIDTH=240 -DTFT_HEIGHT=320 -DTFT_MISO=12 -DTFT_MOSI=13 -DTFT_SCLK=14 -DTFT_CS=15 \
//       -DTFT_DC=2 -DTFT_RST=-1 -DTFT_BL=21 -DTFT_BACKLIGHT_ON=HIGH -DLOAD_GLCD=1 -DLOAD_FONT2=1 \
//       -DLOAD_FONT4=1 -DSPI_FREQUENCY=55000000 -DUSE_HSPI_PORT=1" p5_remote
//   ...append `--upload -p COMxx` to flash. THIS OVERWRITES PALANOTE on the CYD
//   (safe on disk in palanote_ui/, reflashable later).

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>     // primary trigger path: direct board-to-board, bypasses the laggy router
#include <TFT_eSPI.h>
#include <XPT2046_Bitbang.h>

// --- Credentials (standing rule: normally blank; filled for this bench test) ---
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// --- Hub target -----------------------------------------------------------
#define HUB_IP "10.28.0.216"   // hub address on the "nog" hotspot (DHCP - can change, read it off the hub Serial at boot)
#define HTTP_TIMEOUT_MS 5000     // this WiFi is laggy (hub answers in up to ~2s), so wait long

// --- ESP-NOW: PRIMARY trigger path ----------------------------------------
// Router-path pings to these boards run 0.4-2.9s - too flaky for a clicker.
// ESP-NOW goes radio-to-radio (<10ms, MAC-layer ACK). Both boards stay joined
// to the same WiFi so they share the router's channel; the hub keeps serving
// /state over WiFi to Final.html. HTTP trigger is kept as a FALLBACK.
// Hub's WiFi MAC (from ARP for 192.168.1.99):
uint8_t HUB_MAC[6] = { 0x30, 0x76, 0xF5, 0x92, 0x77, 0x34 };

// Payload - must be byte-identical on both boards (see p5_hub.ino).
typedef struct __attribute__((packed)) {
  char zone[8];    // "gen" | "xmit" | "dist" | "all"
  char panel[8];   // optional exhibit inside the zone ("sub", "plant", ...) or ""
} ZoneMsg;

bool espNowUp = false;
volatile bool espAckOk   = false;   // set by the send callback
volatile bool espAckDone = false;

// --- CYD touch (bit-bang, proven on this unit) ----------------------------
#define T_CLK  25
#define T_MOSI 32
#define T_MISO 39
#define T_CS   33
#define LED_R  4     // onboard RGB LED, active-LOW (HIGH = off)
#define LED_G  16
#define LED_B  17
const int SCRW = 320, SCRH = 240;
const int RAW_MIN_X = 200, RAW_MAX_X = 3700;   // from palanote_ui.ino, this exact unit
const int RAW_MIN_Y = 240, RAW_MAX_Y = 3800;

XPT2046_Bitbang touch(T_MOSI, T_MISO, T_CLK, T_CS, SCRW, SCRH);
TFT_eSPI tft = TFT_eSPI();

// --- Palette (RGB565 computed in setup) -----------------------------------
uint16_t C_BG, C_CARD, C_HAIR, C_TXT, C_MUT, C_ERR, C_OK;

// ==========================================================================
// DATA - the whole menu is described by these two tables.
// ==========================================================================

// HOME grid (unchanged from the liked 2x2 design).
struct Zone {
  int   x, y;
  const char* zone;   // command sent to hub
  const char* label;  // short name (also used on nav buttons)
  const char* sub;    // full name (also the zone-screen title)
  const char* tail;   // status-bar message tail
  uint8_t r, g, b;    // zone color
  uint8_t glyph;      // 0=triangle 1=circle 2=square 3=grid
  bool  whiteLabel;   // white text (overview) vs black
  bool  border2;      // 2px white idle border (overview)
  uint16_t fill, press; // computed 565
};

#define BTN_W 146
#define BTN_H 92
#define NZ 4
Zone Z[NZ] = {
  {   6,  40, "gen",  "GEN",      "Generation",   "spotlight on Generation",   34,197, 94, 0, false, false, 0, 0},
  { 168,  40, "xmit", "XMIT",     "Transmission", "spotlight on Transmission",245,158, 11, 1, false, false, 0, 0},
  {   6, 144, "dist", "DIST",     "Distribution", "spotlight on Distribution",167,139,250, 2, false, false, 0, 0},
  { 168, 144, "all",  "OVERVIEW", "Reset view",   "full view, all reset",      38, 43, 51, 3, true,  true,  0, 0},
};

// What's INSIDE each zone. `items` = display labels, `panels` = Final.html's
// own panel ids (sent to the hub so the page can open that exact exhibit).
// Nav target = a Z[] index; index 3 (OVERVIEW) = "fire all + back to HOME".
#define MAX_ITEMS 7
struct ZoneDetail {
  const char* items[MAX_ITEMS];    // what the presenter sees
  const char* panels[MAX_ITEMS];   // what Final.html calls it (showPanel key)
  int nItems;
  int navL, navR;     // Z[] index for bottom-left / bottom-right nav button
};
ZoneDetail ZD[3] = {
  { {"Power Plant", "SP Poly", "H2 Fuel Cell"},
    {"plant",       "poly",    "h2"},                                  3, 3, 1 },  // GEN:  [OVERVIEW] [XMIT >]
  { {"Transmission Tower", "Substation"},
    {"tower",              "sub"},                                     2, 0, 2 },  // XMIT: [< GEN]    [DIST >]
  { {"Dist TX", "Control Room", "Siemens", "AI Data Centre",
     "Factory", "EV + BESS", "BESS Units"},
    {"disttf",  "distx",        "siemens", "aidc",
     "factory", "evpark",    "bess1"},                                 7, 1, 3 },  // DIST: [< XMIT]   [OVERVIEW]
};

// --- Screen / UI state -----------------------------------------------------
#define SCR_HOME (-1)
int screen    = SCR_HOME;  // SCR_HOME, or 0..2 = that zone's screen
int selItem   = -1;        // highlighted sub-item on the current zone screen
int activeIdx = -1;        // which zone the hub is showing (drives highlights + status text)

enum BtnState { IDLE, PRESSED, ACTIVE };

bool wasTouched = false;
unsigned long lockoutUntil = 0;
unsigned long doneUntil    = 0;
unsigned long errorUntil   = 0;
int errPressedIdx = -1;              // HOME-grid button to un-press when an error expires
String lastDoneMsg = "";
bool wifiUp = false;
unsigned long wifiBannerUntil = 0;   // shows "WiFi OK <ip>" briefly on connect
unsigned long lastWifiRetry   = 0;
String myIP = "";

// --- Zone-screen layout constants ------------------------------------------
#define HDR_Y     40    // header strip top (below the status bar)
#define HDR_H     26
#define ITEMS_Y   72    // first sub-item button top
#define NAV_Y     192   // bottom nav row top
#define NAV_H     42
#define MENU_X    250   // small MENU chip in the header (returns HOME, no trigger)
#define MENU_W    64

// ==========================================================================
// Drawing helpers
// ==========================================================================
uint16_t mix(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

void drawGlyph(const Zone& z, int cx, int cy, uint16_t col) {
  switch (z.glyph) {
    case 0: tft.fillTriangle(cx, cy - 11, cx - 11, cy + 11, cx + 11, cy + 11, col); break; // triangle
    case 1: tft.fillCircle(cx, cy, 11, col); break;                                        // circle
    case 2: tft.fillRoundRect(cx - 10, cy - 10, 20, 20, 3, col); break;                    // square
    case 3:                                                                                // 2x2 grid
      tft.fillRoundRect(cx - 10, cy - 10, 9, 9, 2, col);
      tft.fillRoundRect(cx + 1,  cy - 10, 9, 9, 2, col);
      tft.fillRoundRect(cx - 10, cy + 1,  9, 9, 2, col);
      tft.fillRoundRect(cx + 1,  cy + 1,  9, 9, 2, col);
      break;
  }
}

void drawButton(int i, BtnState st) {
  Zone& z = Z[i];
  int cx = z.x + BTN_W / 2;
  uint16_t fill = (st == PRESSED) ? z.press : z.fill;
  uint16_t lab  = z.whiteLabel ? C_TXT : TFT_BLACK;

  tft.fillRoundRect(z.x, z.y, BTN_W, BTN_H, 10, fill);
  drawGlyph(z, cx, z.y + 26, lab);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(lab, fill);
  tft.drawString(z.label, cx, z.y + 56, 4);
  tft.drawString(z.sub,   cx, z.y + 78, 2);

  if (st == PRESSED) {
    for (int k = 0; k < 3; k++) tft.drawRoundRect(z.x + k, z.y + k, BTN_W - 2 * k, BTN_H - 2 * k, 10 - k, C_TXT);
  } else if (st == ACTIVE) {
    for (int k = 0; k < 4; k++) tft.drawRoundRect(z.x + k, z.y + k, BTN_W - 2 * k, BTN_H - 2 * k, 10 - k, C_TXT);
  } else if (z.border2) {
    tft.drawRoundRect(z.x, z.y, BTN_W, BTN_H, 10, C_TXT);
    tft.drawRoundRect(z.x + 1, z.y + 1, BTN_W - 2, BTN_H - 2, 9, C_TXT);
  } else {
    tft.drawRoundRect(z.x, z.y, BTN_W, BTN_H, 10, C_HAIR);
  }
}

// Trim a string with an ellipsis so it fits maxW at the given font.
String fitText(const String& s, int maxW, uint8_t font) {
  if (tft.textWidth(s, font) <= maxW) return s;
  String out = s;
  while (out.length() > 1 && tft.textWidth(out + "...", font) > maxW) out.remove(out.length() - 1);
  return out + "...";
}

void drawWifiBars(bool up) {
  uint16_t c = up ? C_OK : C_ERR;
  uint16_t off = C_HAIR;
  tft.fillRect(290, 22, 4, 6,  up ? c : off);
  tft.fillRect(297, 17, 4, 11, up ? c : off);
  tft.fillRect(304, 12, 4, 16, c);
}

// General status bar (dot + text). isTick draws the green done badge instead of a dot.
void statusBar(const String& msg, uint16_t dotCol, bool isTick) {
  tft.fillRect(0, 0, 320, 34, C_BG);
  if (isTick) {
    tft.fillCircle(14, 17, 7, C_OK);
    tft.drawLine(10, 17, 13, 20, C_BG);
    tft.drawLine(13, 20, 19, 13, C_BG);
    tft.drawLine(10, 18, 13, 21, C_BG);
    tft.drawLine(13, 21, 19, 14, C_BG);
  } else {
    tft.fillCircle(14, 17, 5, dotCol);
  }
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_TXT, C_BG);
  tft.drawString(fitText(msg, 250, 2), 28, 17, 2);
  drawWifiBars(wifiUp);
  tft.drawFastHLine(0, 34, 320, C_HAIR);
}

void statusError() {
  tft.fillRect(0, 0, 320, 34, C_ERR);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(C_TXT, C_ERR);
  tft.drawString("HUB UNREACHABLE", 14, 11, 2);
  tft.drawString("check hub power & WiFi", 14, 26, 1);
  tft.drawFastHLine(0, 34, 320, C_HAIR);
}

void statusResting() {
  if (activeIdx < 0) { statusBar("SPOTLIGHT REMOTE", C_MUT, false); return; }
  Zone& z = Z[activeIdx];
  String m = String(z.label) + " ACTIVE  -  " + z.tail;
  uint16_t dot = (activeIdx == 3) ? C_TXT : z.fill;
  statusBar(m, dot, false);
}

// Brief on-screen confirmation that WiFi is up (the user couldn't tell otherwise).
void showWifiBanner() {
  statusBar(String("WiFi OK   ") + myIP, C_OK, false);
  wifiBannerUntil = millis() + 2500;
}

// ==========================================================================
// ZONE screen drawing
// ==========================================================================

// Rectangle of sub-item k on zone screen zi. <=3 items get big full-width
// rows; more items (DIST has 7) flow into a compact 2-column grid.
void itemRect(int zi, int k, int& x, int& y, int& w, int& h) {
  int n = ZD[zi].nItems;
  if (n <= 3) {
    x = 6;  w = 308;  h = 34;
    y = ITEMS_Y + k * (h + 6);
  } else {
    int rows = (n + 1) / 2;
    int avail = (NAV_Y - 6) - ITEMS_Y;
    h = (avail - (rows - 1) * 6) / rows;  if (h > 34) h = 34;
    w = 150;
    x = 6 + (k % 2) * (w + 8);
    y = ITEMS_Y + (k / 2) * (h + 6);
  }
}

void drawItem(int zi, int k) {
  int x, y, w, h;  itemRect(zi, k, x, y, w, h);
  bool sel = (k == selItem);
  uint16_t fill = sel ? Z[zi].fill : C_CARD;
  uint16_t text = sel ? TFT_BLACK  : C_TXT;
  uint8_t  font = (h >= 30) ? 4 : 2;

  tft.fillRoundRect(x, y, w, h, 6, fill);
  tft.drawRoundRect(x, y, w, h, 6, sel ? C_TXT : C_HAIR);
  if (!sel) tft.fillRoundRect(x + 3, y + 4, 3, h - 8, 1, Z[zi].fill);  // zone-color accent tick
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(text, fill);
  tft.drawString(fitText(ZD[zi].items[k], w - 22, font), x + w / 2 + (sel ? 0 : 3), y + h / 2, font);
}

// Bottom nav button (side 0 = left, 1 = right). Its fill = the TARGET zone's
// color, so the button color always says where it takes you.
void navRect(int side, int& x, int& y, int& w, int& h) {
  w = 151;  h = NAV_H;  y = NAV_Y;
  x = (side == 0) ? 6 : 163;
}

void drawNavBtn(int zi, int side) {
  int x, y, w, h;  navRect(side, x, y, w, h);
  int t = (side == 0) ? ZD[zi].navL : ZD[zi].navR;
  Zone& tz = Z[t];
  String label = (t == 3) ? String("OVERVIEW")
               : (side == 0) ? String("< ") + tz.label
                             : String(tz.label) + " >";

  tft.fillRoundRect(x, y, w, h, 8, tz.fill);
  if (t == 3) {  // overview keeps its white-outline style
    tft.drawRoundRect(x, y, w, h, 8, C_TXT);
    tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, C_TXT);
  } else {
    tft.drawRoundRect(x, y, w, h, 8, C_HAIR);
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(tz.whiteLabel ? C_TXT : TFT_BLACK, tz.fill);
  tft.drawString(label, x + w / 2, y + h / 2, 4);
}

void drawZoneScreen() {
  int zi = screen;
  tft.fillScreen(C_BG);

  // status bar (keep any pending Done/error banner alive across the redraw)
  if (errorUntil)      statusError();
  else if (doneUntil)  statusBar(lastDoneMsg, C_OK, true);
  else                 statusResting();

  // header: zone name in its color + MENU chip (back to HOME, fires nothing)
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(Z[zi].fill, C_BG);
  tft.drawString(Z[zi].sub, 8, HDR_Y + HDR_H / 2, 4);
  tft.drawRoundRect(MENU_X, HDR_Y + 1, MENU_W, HDR_H - 2, 6, C_HAIR);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_MUT, C_BG);
  tft.drawString("MENU", MENU_X + MENU_W / 2, HDR_Y + HDR_H / 2, 2);
  tft.drawFastHLine(6, HDR_Y + HDR_H + 3, 308, C_HAIR);

  for (int k = 0; k < ZD[zi].nItems; k++) drawItem(zi, k);
  drawNavBtn(zi, 0);
  drawNavBtn(zi, 1);
}

void drawHome() {
  tft.fillScreen(C_BG);
  for (int i = 0; i < NZ; i++) drawButton(i, i == activeIdx ? ACTIVE : IDLE);
  if (errorUntil)      statusError();
  else if (doneUntil)  statusBar(lastDoneMsg, C_OK, true);
  else                 statusResting();
}

void openZone(int zi) { screen = zi; selItem = -1; errPressedIdx = -1; drawZoneScreen(); }
void goHome()         { screen = SCR_HOME; errPressedIdx = -1; drawHome(); }

// ==========================================================================
// Touch
// ==========================================================================
bool readTouch(int& sx, int& sy) {
  TouchPoint p = touch.getTouch();
  if (p.zRaw == 0) return false;
  sx = p.x; sy = p.y;
  return true;
}

bool inRect(int px, int py, int x, int y, int w, int h, int slop) {
  return px >= x - slop && px <= x + w + slop && py >= y - slop && py <= y + h + slop;
}

// HOME grid hit test (visual rect grown by 6px - kiosk gutter trick).
int hitButton(int x, int y) {
  for (int i = 0; i < NZ; i++)
    if (inRect(x, y, Z[i].x, Z[i].y, BTN_W, BTN_H, 6)) return i;
  return -1;
}

// ==========================================================================
// Networking
// ==========================================================================
bool triggerHub(const char* zone, const char* panel) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + HUB_IP + "/trigger?zone=" + zone;
  if (panel[0]) url += String("&panel=") + panel;
  http.begin(client, url);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = http.GET();
  http.end();
  return code == 200;
}

// --- ESP-NOW (primary trigger) ---------------------------------------------
// esp32 core 3.x callback signature (older examples use `const uint8_t* mac`).
void onEspNowSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  espAckOk   = (status == ESP_NOW_SEND_SUCCESS);
  espAckDone = true;
}

bool initEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW: init FAILED"); return false; }
  esp_now_register_send_cb(onEspNowSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, HUB_MAC, 6);
  peer.channel = 0;               // 0 = follow the current STA channel (= router's = hub's)
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) { Serial.println("ESP-NOW: add_peer FAILED"); return false; }
  Serial.println("ESP-NOW ready. Peer = hub 30:76:F5:92:77:34");
  return true;
}

// Send the zone (+ optional exhibit panel) over ESP-NOW; ACK lands in <10ms.
bool sendZoneEspNow(const char* zone, const char* panel) {
  if (!espNowUp) return false;
  ZoneMsg m = {};
  strncpy(m.zone,  zone,  sizeof(m.zone)  - 1);
  strncpy(m.panel, panel, sizeof(m.panel) - 1);
  for (int attempt = 1; attempt <= 3; attempt++) {
    espAckOk = false; espAckDone = false;
    if (esp_now_send(HUB_MAC, (const uint8_t*)&m, sizeof(m)) == ESP_OK) {
      unsigned long t0 = millis();
      while (!espAckDone && millis() - t0 < 150) delay(2);
      if (espAckOk) return true;
    }
    Serial.printf("ESP-NOW: attempt %d no ACK\n", attempt);
    delay(30);
  }
  return false;
}

// One call = one trigger to the hub, with the full status-bar story.
// zIdx is a Z[] index; panel = "" for the whole zone, or a Final.html panel id.
// Returns true if the hub confirmed (ESP-NOW ACK or HTTP 200).
bool fireZone(int zIdx, const char* panel) {
  statusBar("Sending...", mix(245, 158, 11), false);
  bool ok = sendZoneEspNow(Z[zIdx].zone, panel);
  Serial.printf("Fire %s/%s: ESP-NOW %s\n", Z[zIdx].zone, panel, ok ? "ACK" : "no ACK");
  if (!ok) {
    ok = triggerHub(Z[zIdx].zone, panel);
    Serial.printf("Fire %s/%s: HTTP fallback %s\n", Z[zIdx].zone, panel, ok ? "ok" : "FAILED");
  }
  if (ok) {
    activeIdx = zIdx;
    lastDoneMsg = String("Done  -  ") + Z[zIdx].tail;
    statusBar(lastDoneMsg, C_OK, true);
    doneUntil = millis() + 900;
    errorUntil = 0;
  } else {
    statusError();
    errorUntil = millis() + 3000;
  }
  return ok;
}

// Best-effort: adopt the hub's current zone at boot so the remote reflects reality.
void adoptHubState() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, String("http://") + HUB_IP + "/state");
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (http.GET() == 200) {
    String body = http.getString();
    if (body.indexOf("\"active\":true") >= 0) {
      for (int i = 0; i < NZ; i++) {
        if (body.indexOf(String("\"zone\":\"") + Z[i].zone + "\"") >= 0) { activeIdx = i; break; }
      }
    }
  }
  http.end();
}

// ==========================================================================
// Tap handling - one function per screen, routed by handleTap().
// ==========================================================================
void handleHomeTap(int x, int y) {
  int i = hitButton(x, y);
  if (i < 0) return;

  drawButton(i, PRESSED);
  bool ok = fireZone(i, "");
  lockoutUntil = millis() + 300;

  if (i == 3) {                    // OVERVIEW: stay on HOME
    for (int k = 0; k < NZ; k++) drawButton(k, (ok && k == 3) ? ACTIVE : IDLE);
    if (!ok) errPressedIdx = 3;
  } else {                         // zone: open its screen even if the hub missed,
    openZone(i);                   // so the presenter can still see/talk the content
  }
}

void handleZoneTap(int x, int y) {
  int zi = screen;
  int rx, ry, rw, rh;

  // MENU chip: back to the grid, no trigger (spotlight stays as-is)
  if (inRect(x, y, MENU_X, HDR_Y, MENU_W, HDR_H, 6)) { goHome(); return; }

  // sub-items: highlight + send zone+panel (Final.html opens that exhibit,
  // hub LED does the white double-blink, 45s hold resets)
  for (int k = 0; k < ZD[zi].nItems; k++) {
    itemRect(zi, k, rx, ry, rw, rh);
    if (inRect(x, y, rx, ry, rw, rh, 3)) {
      selItem = k;
      for (int j = 0; j < ZD[zi].nItems; j++) drawItem(zi, j);
      if (fireZone(zi, ZD[zi].panels[k])) {
        lastDoneMsg = String(Z[zi].label) + "  -  " + ZD[zi].items[k];
        statusBar(lastDoneMsg, C_OK, true);
      }
      lockoutUntil = millis() + 300;
      return;
    }
  }

  // bottom nav buttons
  for (int side = 0; side < 2; side++) {
    navRect(side, rx, ry, rw, rh);
    if (inRect(x, y, rx, ry, rw, rh, 6)) {
      int t = (side == 0) ? ZD[zi].navL : ZD[zi].navR;
      fireZone(t, "");
      lockoutUntil = millis() + 300;
      if (t == 3) goHome();       // OVERVIEW resets the journey
      else        openZone(t);    // walk to the next/previous zone screen
      return;
    }
  }
}

void handleTap(int x, int y) {
  if (screen == SCR_HOME) handleHomeTap(x, y);
  else                    handleZoneTap(x, y);
}

// ==========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Onboard RGB LED is active-LOW: force OFF so it doesn't glow/flicker white.
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);

  tft.init();
  tft.setRotation(1);              // landscape 320x240
  tft.fillScreen(TFT_BLACK);

  touch.begin();
  touch.setCalibration(RAW_MIN_X, RAW_MAX_X, RAW_MIN_Y, RAW_MAX_Y);

  // palette
  C_BG   = mix(15, 17, 21);
  C_CARD = mix(26, 30, 38);
  C_HAIR = mix(42, 47, 56);
  C_TXT  = TFT_WHITE;
  C_MUT  = mix(154, 160, 170);
  C_ERR  = mix(239, 68, 68);
  C_OK   = mix(34, 197, 94);
  for (int i = 0; i < NZ; i++) {
    Z[i].fill  = mix(Z[i].r, Z[i].g, Z[i].b);
    Z[i].press = mix(Z[i].r * 0.72, Z[i].g * 0.72, Z[i].b * 0.72);
  }

  drawHome();

  // WiFi (30s boot window; this router is slow to associate). Auto-reconnect stays
  // on and the loop keeps retrying, so a slow join still recovers on its own.
  statusBar("WiFi connecting...", mix(245, 158, 11), false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // keep the radio responsive (matches the hub)
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  wifiUp = (WiFi.status() == WL_CONNECTED);
  if (wifiUp) {
    myIP = WiFi.localIP().toString();
    Serial.print("Connected. Remote IP: "); Serial.println(myIP);
    Serial.print("Hub target: http://"); Serial.println(HUB_IP);
    adoptHubState();
  } else {
    Serial.println("WiFi not connected yet - loop will keep retrying in the background.");
  }

  // ESP-NOW rides on the STA interface; joining the router first pins both
  // boards to the same channel, which ESP-NOW requires.
  espNowUp = initEspNow();

  drawHome();
  if (wifiUp) showWifiBanner();
}

void loop() {
  unsigned long now = millis();

  // WiFi state tracking + on-screen indicator
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up != wifiUp) {
    wifiUp = up;
    if (up) {
      myIP = WiFi.localIP().toString();
      Serial.print("WiFi up. Remote IP: "); Serial.println(myIP);
      showWifiBanner();
    } else {
      statusBar("WiFi reconnecting...", C_ERR, false);
    }
  }
  if (!up && now - lastWifiRetry > 8000) { WiFi.reconnect(); lastWifiRetry = now; }

  // status timers
  if (wifiBannerUntil && now > wifiBannerUntil) { wifiBannerUntil = 0; statusResting(); }
  if (doneUntil && now > doneUntil)  { doneUntil = 0; statusResting(); }
  if (errorUntil && now > errorUntil) {
    errorUntil = 0;
    if (screen == SCR_HOME && errPressedIdx >= 0)
      drawButton(errPressedIdx, errPressedIdx == activeIdx ? ACTIVE : IDLE);
    errPressedIdx = -1;
    statusResting();
  }

  // touch (act on touch-down edge, with 300ms lockout)
  int x, y;
  bool t = readTouch(x, y);
  bool tap = t && !wasTouched;
  wasTouched = t;
  if (tap && now > lockoutUntil) handleTap(x, y);

  delay(20);   // ~50 Hz poll
}


