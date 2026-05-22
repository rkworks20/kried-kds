#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Config ───────────────────────────────────────────
const char* WIFI_SSID     = "Airtel_Rk?s Wifi";
const char* WIFI_PASSWORD = "wifi1234";
const char* SERVER_HOST   = "kried-kds-production.up.railway.app";

// ── OTA update ───────────────────────────────────────
// Bump FIRMWARE_VERSION and kds_oled/version.txt together when you push new code.
// GitHub Actions compiles and posts the binary; the ESP32 downloads it on next boot.
#define FIRMWARE_VERSION  "1.0.6"
// Both URLs served directly from GitHub — no CDN redirects
#define OTA_VERSION_URL   "https://raw.githubusercontent.com/rkworks20/kried-kds/main/kds_oled/version.txt"
#define OTA_FIRMWARE_URL  "https://raw.githubusercontent.com/rkworks20/kried-kds/main/firmware/firmware.bin"

#define UPDATE_INTERVAL   3000    // poll /current-bill every 3 seconds
#define WIFI_TIMEOUT_MS  15000
#define WIFI_RETRY_MS     5000

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_ADDRESS  0x3C
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define LED_PIN 21

// ── App state ───────────────────────────────────────
enum AppState { STATE_WIFI_CONNECTING, STATE_CONNECTED };
AppState appState        = STATE_WIFI_CONNECTING;
uint32_t lastWifiAttempt = 0;

// ── LED ─────────────────────────────────────────────
enum LedMode { LED_OFF, LED_BREATHING, LED_BLINK_FAST };
LedMode  ledMode       = LED_OFF;
uint32_t ledModeStart  = 0;
uint32_t lastLedUpdate = 0;
float    breathVal     = 0.0;
float    breathDir     = 1.0;
bool     blinkState    = false;

// ── Bill state ──────────────────────────────────────
String   currentBill       = "__UNKNOWN__";
uint32_t lastDisplayUpdate = 0;

// ── Icon alternation (burger ↔ fries every 1 s) ──────
#define ICON_SWITCH_MS  1000
bool     showFries      = false;
uint32_t lastIconSwitch = 0;


// ── Flip animation state ─────────────────────────────
enum FlipPhase { FLIP_IDLE, FLIP_SHRINK, FLIP_FOLD, FLIP_GROW };
FlipPhase flipPhase    = FLIP_IDLE;
String    flipFromBill = "";
String    flipToBill   = "";
int       flipHeight   = 32;
uint32_t  lastFlipStep = 0;
#define   FLIP_STEP_MS 25
#define   FLIP_FOLD_MS 40

void setState(AppState s) { appState = s; }

// ── LED ─────────────────────────────────────────────
void setupLED() {
  pinMode(LED_PIN, OUTPUT);
  ledcAttach(LED_PIN, 5000, 8);
  ledcWrite(LED_PIN, 0);
}

void setLedMode(LedMode mode) {
  ledMode = mode; ledModeStart = millis(); blinkState = false;
  if (mode == LED_OFF) ledcWrite(LED_PIN, 0);
}

void updateLED() {
  uint32_t now = millis();
  if (ledMode == LED_BREATHING) {
    if (now - lastLedUpdate < 16) return;
    lastLedUpdate = now;
    breathVal += breathDir * 0.02;
    if (breathVal >= 1.0) { breathVal = 1.0; breathDir = -1.0; }
    if (breathVal <= 0.0) { breathVal = 0.0; breathDir =  1.0; }
    ledcWrite(LED_PIN, (int)((sin(breathVal * PI) + 1.0) / 2.0 * 255));
  }
  if (ledMode == LED_BLINK_FAST) {
    if (now - lastLedUpdate < 80) return;
    lastLedUpdate = now;
    blinkState = !blinkState;
    ledcWrite(LED_PIN, blinkState ? 255 : 0);
    if (now - ledModeStart > 1000) setLedMode(LED_BREATHING);
  }
}

// ── 81x32 Kried logo (shown full-screen when no bill is on display) ─────────
// White-background source → drawn with fg=BLACK, bg=WHITE so the logo
// appears white on the dark OLED screen.  Centred: x=(128-81)/2=23, y=0.
static const uint8_t PROGMEM kriedLogo[] = {
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80,
  0xff, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80,
  0xc7, 0xc0, 0x40, 0x00, 0x78, 0x40, 0x00, 0x04, 0x00, 0x3f, 0x80,
  0xc7, 0xc0, 0xc0, 0x00, 0x38, 0x40, 0x00, 0x04, 0x00, 0x0f, 0x80,
  0xc7, 0xc0, 0xc0, 0x00, 0x18, 0x40, 0x00, 0x04, 0x00, 0x0f, 0x80,
  0xc7, 0xc1, 0xc0, 0x00, 0x18, 0x40, 0x00, 0x04, 0x00, 0x07, 0x80,
  0xc7, 0x81, 0xc0, 0x00, 0x08, 0x40, 0x00, 0x04, 0x00, 0x03, 0x80,
  0xc7, 0x03, 0xc0, 0x00, 0x08, 0x40, 0x00, 0x04, 0x00, 0x03, 0x80,
  0xc7, 0x07, 0xc0, 0x00, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc0, 0x0f, 0xc0, 0x00, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc0, 0x03, 0xc0, 0x00, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc0, 0x01, 0xc0, 0x00, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc7, 0x80, 0xc3, 0xfc, 0x08, 0x40, 0xff, 0xfc, 0x00, 0x01, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x43, 0xff, 0xfc, 0x00, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x18, 0x43, 0xff, 0xfc, 0x00, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x38, 0x41, 0xff, 0xfc, 0x00, 0x00, 0x80,
  0xc7, 0xc0, 0x40, 0x00, 0x38, 0x40, 0x03, 0xfc, 0x3f, 0x00, 0x80,
  0xc7, 0xc0, 0x40, 0x00, 0x18, 0x40, 0x03, 0xfc, 0x7f, 0x00, 0x80,
  0xc7, 0xc0, 0x40, 0x00, 0x08, 0x40, 0x03, 0xfc, 0x7f, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfc, 0x08, 0x41, 0xff, 0xfc, 0x3e, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x43, 0xff, 0xfc, 0x00, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x43, 0xff, 0xfc, 0x00, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x41, 0xff, 0xfc, 0x00, 0x00, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x01, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x03, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x03, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x07, 0x80,
  0xc7, 0xc0, 0x43, 0xfe, 0x08, 0x40, 0x00, 0x04, 0x00, 0x0f, 0x80,
  0xc7, 0xe0, 0x63, 0xfe, 0x0c, 0x60, 0x00, 0x06, 0x00, 0x7f, 0x80,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80,
};
#define LOGO_W  81
#define LOGO_H  32
#define LOGO_X  ((SCREEN_WIDTH - LOGO_W) / 2)  // = 23 — horizontally centred
#define LOGO_Y   0

// ── 40x32 burger icon (from Burger.jpg via image2cpp) ───────────────────────
// 40×32 matches the OLED height exactly — no vertical offset needed.
// White-background source → drawn with fg=BLACK, bg=WHITE so the black outline
// pixels render as WHITE on the dark OLED screen.
static const uint8_t PROGMEM burgerIcon[] = {
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0x21, 0xff, 0xff, 0xff, 0xf8, 0x00, 0x3f, 0xff,
  0xf7, 0xe0, 0x00, 0x0f, 0xff, 0xfb, 0xc0, 0x01, 0x07, 0xff,
  0xdf, 0x80, 0x00, 0x03, 0xff, 0xef, 0x00, 0x80, 0x01, 0xdf,
  0xfe, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x00, 0xf7,
  0xfe, 0x00, 0x00, 0x00, 0x7f, 0xfe, 0x00, 0x00, 0x00, 0x7f,
  0xff, 0x00, 0x00, 0x01, 0xff, 0xfe, 0xff, 0xff, 0xff, 0x7f,
  0xfb, 0xff, 0xff, 0xfb, 0xff, 0xfa, 0x33, 0x80, 0x60, 0xff,
  0xfe, 0x00, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x7f,
  0xfb, 0x00, 0x00, 0xe0, 0xdf, 0xfc, 0x8e, 0x79, 0xff, 0x7f,
  0xf8, 0xd1, 0x4e, 0x00, 0x7f, 0xfe, 0x61, 0x86, 0x00, 0xff,
  0xff, 0x01, 0x80, 0x01, 0xff, 0xff, 0x80, 0x00, 0x03, 0xff,
  0xff, 0xc0, 0x00, 0x07, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
#define BURGER_W  40
#define BURGER_H  32
#define BURGER_X   0
#define BURGER_Y   0   // 32px icon fits the 32px screen perfectly

// Text (textSize 3, 18px/char) laid out to the right of the icon, centred in
// the remaining area.  Icon is 40px + 4px gap → text area starts at x=44,
// leaving 84px which comfortably fits a 4-char bill ID (4×18 = 72px).
static int billTextX(const String& bill) {
  int tw       = bill.length() * 18;   // textSize 3: 6px × 3 = 18px per char
  int areaLeft = BURGER_X + BURGER_W + 4;
  int areaW    = SCREEN_WIDTH - areaLeft;
  int x        = areaLeft + max(0, (areaW - tw) / 2);
  return x;
}

// ── OLED drawing ────────────────────────────────────
// Icon bitmaps are drawn inverted (fg=BLACK, bg=WHITE) so the black outlines
// from the white-background source images appear white on the dark OLED screen.

// Render content into the buffer without pushing to display.
// Call oled.display() after any overlaid effects.
void renderContent(const String& bill) {
  if (bill == "" || bill == "__UNKNOWN__") {
    // No bill — show Kried logo full-screen, centred
    oled.drawBitmap(LOGO_X, LOGO_Y, kriedLogo, LOGO_W, LOGO_H, SSD1306_BLACK, SSD1306_WHITE);
  } else {
    // Bill active — burger icon on left, bill number on right
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(3);                   // 24px tall; 18px/char
    oled.setCursor(billTextX(bill), 4);    // vertically centred: (32-24)/2 = 4
    oled.print(bill);
    oled.drawBitmap(BURGER_X, BURGER_Y, burgerIcon, BURGER_W, BURGER_H, SSD1306_BLACK, SSD1306_WHITE);
  }
}

void drawFinal(const String& bill) {
  oled.clearDisplay();
  renderContent(bill);
  oled.display();
}

// ── Flip animation (non-blocking) ───────────────────
// Squishes content vertically around the screen centre, holds a fold line,
// then expands back revealing the new content.
// Logo (bill=="") squishes full-screen; bill cards squish text area only
// while the burger icon stays pinned in place.
void drawBillSquished(const String& bill, int height) {
  oled.clearDisplay();
  int yCenter = 16;
  int yTop    = yCenter - height / 2;

  if (bill == "" || bill == "__UNKNOWN__") {
    // Squish the full-screen logo
    oled.drawBitmap(LOGO_X, LOGO_Y, kriedLogo, LOGO_W, LOGO_H, SSD1306_BLACK, SSD1306_WHITE);
    if (yTop > 0)
      oled.fillRect(0, 0, SCREEN_WIDTH, yTop, SSD1306_BLACK);
    if (yTop + height < SCREEN_HEIGHT)
      oled.fillRect(0, yTop + height, SCREEN_WIDTH, SCREEN_HEIGHT - (yTop + height), SSD1306_BLACK);
  } else {
    // Squish text area only; burger icon stays static
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(3);
    oled.setCursor(billTextX(bill), 4);
    oled.print(bill);
    int maskX = BURGER_X + BURGER_W + 4;
    int maskW = SCREEN_WIDTH - maskX;
    if (yTop > 0)
      oled.fillRect(maskX, 0, maskW, yTop, SSD1306_BLACK);
    if (yTop + height < SCREEN_HEIGHT)
      oled.fillRect(maskX, yTop + height, maskW, SCREEN_HEIGHT - (yTop + height), SSD1306_BLACK);
    oled.drawBitmap(BURGER_X, BURGER_Y, burgerIcon, BURGER_W, BURGER_H, SSD1306_BLACK, SSD1306_WHITE);
  }
  oled.display();
}

void startFlip(const String& from, const String& to) {
  flipFromBill = from;
  flipToBill   = to;
  flipHeight   = 32;
  flipPhase    = FLIP_SHRINK;
  lastFlipStep = millis();
}

void updateFlip() {
  if (flipPhase == FLIP_IDLE) return;
  uint32_t interval = (flipPhase == FLIP_FOLD) ? FLIP_FOLD_MS : FLIP_STEP_MS;
  if (millis() - lastFlipStep < interval) return;
  lastFlipStep = millis();

  switch (flipPhase) {
    case FLIP_SHRINK:
      flipHeight -= 4;
      if (flipHeight <= 2) {
        oled.clearDisplay();
        oled.drawFastHLine(0, 15, SCREEN_WIDTH, SSD1306_WHITE);
        oled.drawFastHLine(0, 16, SCREEN_WIDTH, SSD1306_WHITE);
        oled.display();
        flipPhase = FLIP_FOLD;
      } else {
        drawBillSquished(flipFromBill, flipHeight);
      }
      break;
    case FLIP_FOLD:
      flipHeight = 2;
      flipPhase  = FLIP_GROW;
      drawBillSquished(flipToBill, flipHeight);
      break;
    case FLIP_GROW:
      flipHeight += 4;
      if (flipHeight >= 32) {
        flipPhase = FLIP_IDLE;
        drawFinal(flipToBill);
      } else {
        drawBillSquished(flipToBill, flipHeight);
      }
      break;
    case FLIP_IDLE: break;
  }
}

void showSplash(const String& msg) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 12);
  oled.print(msg);
  oled.display();
}

// ── WiFi ─────────────────────────────────────────────
void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.println("WiFi dropped");
    setState(STATE_WIFI_CONNECTING);
    lastWifiAttempt = 0;
    showSplash("WiFi lost...");
    setLedMode(LED_OFF);
  }
}

void tryWifi() {
  if (millis() - lastWifiAttempt < WIFI_RETRY_MS && lastWifiAttempt != 0) return;
  lastWifiAttempt = millis();
  Serial.println("Attempting WiFi...");
  showSplash("Connecting WiFi...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) yield();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected — " + WiFi.localIP().toString());
    showSplash("CONNECTED");
    setLedMode(LED_BREATHING);
    setState(STATE_CONNECTED);
    currentBill = "__UNKNOWN__";  // force a fresh display on first poll
    checkOTAUpdate();             // check GitHub for a newer firmware build
  } else {
    Serial.println("WiFi failed — will retry");
    showSplash("WIFI FAIL");
  }
}

// ── HTTP poll ────────────────────────────────────────
// Fetches https://SERVER_HOST/current-bill and returns the bill string.
// Returns "__ERROR__" on any failure so the display stays unchanged.
String pollCurrentBill() {
  WiFiClientSecure client;
  client.setInsecure();  // skip cert verification — server cert is valid but ESP32 has no CA store
  HTTPClient http;
  String url = "https://";
  url += SERVER_HOST;
  url += "/current-bill";
  if (!http.begin(client, url)) { http.end(); return "__ERROR__"; }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP poll failed: %d\n", code);
    http.end();
    return "__ERROR__";
  }
  String body = http.getString();
  http.end();
  // Response is {"bill":"B105"} or {"bill":""}
  // Simple parse — no JSON library needed
  int start = body.indexOf("\"bill\":\"") + 8;
  int end   = body.indexOf("\"", start);
  if (start < 8 || end < 0) return "__ERROR__";
  return body.substring(start, end);
}

// ── Fries icon (drawn with GFX primitives, same 40×32 area as burger) ────────
void drawFriesIcon() {
  // Five fry sticks — 3 px wide, 16 px tall
  oled.fillRect( 2, 0, 3, 16, SSD1306_WHITE);
  oled.fillRect( 8, 0, 3, 16, SSD1306_WHITE);
  oled.fillRect(14, 0, 3, 16, SSD1306_WHITE);
  oled.fillRect(20, 0, 3, 16, SSD1306_WHITE);
  oled.fillRect(26, 0, 3, 16, SSD1306_WHITE);
  // Container body
  oled.fillRect(0, 16, 32, 15, SSD1306_WHITE);
  // Centre divider on the container (dark line)
  oled.drawFastVLine(16, 16, 15, SSD1306_BLACK);
}

// ── Icon alternation (non-blocking) ──────────────────
// Switches between burger and fries every ICON_SWITCH_MS while a bill
// is shown and no flip animation is running.
void updateIconSwitch() {
  if (flipPhase != FLIP_IDLE) return;
  if (currentBill == "" || currentBill == "__UNKNOWN__") return;
  if (millis() - lastIconSwitch < ICON_SWITCH_MS) return;

  lastIconSwitch = millis();
  showFries      = !showFries;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(3);
  oled.setCursor(billTextX(currentBill), 4);
  oled.print(currentBill);
  if (showFries) {
    drawFriesIcon();
  } else {
    oled.drawBitmap(BURGER_X, BURGER_Y, burgerIcon, BURGER_W, BURGER_H,
                    SSD1306_BLACK, SSD1306_WHITE);
  }
  oled.display();
}

// ── OTA update check ─────────────────────────────────
// Called once after WiFi connects.  Fetches version.txt from GitHub; if
// the remote version differs from FIRMWARE_VERSION it downloads the binary
// from the firmware-latest release and flashes it, then auto-restarts.
void checkOTAUpdate() {
  // Show check on OLED
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 4);  oled.println("CHECKING UPDATE");
  oled.setCursor(2, 16); oled.println("v" FIRMWARE_VERSION);
  oled.display();
  delay(500);

  Serial.println("OTA: checking version at " OTA_VERSION_URL);

  // Step 1 — fetch version.txt
  WiFiClientSecure verClient;
  verClient.setInsecure();
  HTTPClient http;
  if (!http.begin(verClient, OTA_VERSION_URL)) {
    Serial.println("OTA: cannot reach version URL — skipping");
    showSplash("OTA: no reach");
    delay(1500);
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("OTA: version fetch failed (HTTP %d) — skipping\n", code);
    http.end();
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(2, 8); oled.print("OTA ERR: "); oled.println(code);
    oled.display();
    delay(2000);
    return;
  }
  String latest = http.getString();
  http.end();
  latest.trim();

  Serial.println("OTA: local=" FIRMWARE_VERSION "  remote=" + latest);

  if (latest == FIRMWARE_VERSION) {
    Serial.println("OTA: firmware is up to date");
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(2, 4);  oled.println("FIRMWARE OK");
    oled.setCursor(2, 16); oled.println("v" FIRMWARE_VERSION);
    oled.display();
    delay(1000);
    return;
  }

  // Step 2 — show update screen
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 4);  oled.println("OTA UPDATING...");
  oled.setCursor(2, 16); oled.print("v" FIRMWARE_VERSION " -> v"); oled.println(latest);
  oled.display();
  Serial.println("OTA: downloading firmware from " OTA_FIRMWARE_URL);

  // Step 3 — download and flash
  WiFiClientSecure fwClient;
  fwClient.setInsecure();
  httpUpdate.setLedPin(LED_PIN, LOW);
  httpUpdate.rebootOnUpdate(true);   // auto-restart on success

  t_httpUpdate_return ret = httpUpdate.update(fwClient, OTA_FIRMWARE_URL);

  switch (ret) {
    case HTTP_UPDATE_OK:
      // rebootOnUpdate(true) means we never reach here — ESP32 restarts
      break;
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA: failed — error %d: %s\n",
        httpUpdate.getLastError(),
        httpUpdate.getLastErrorString().c_str());
      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setTextColor(SSD1306_WHITE);
      oled.setCursor(2, 8); oled.println("OTA FAILED");
      oled.setCursor(2, 20); oled.println("resuming...");
      oled.display();
      delay(2000);   // brief pause so the message is readable
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: server says no update");
      break;
  }
}

// ── Setup ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\nKDS OLED booting...");
  setupLED();
  Wire.begin(5, 6);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED not found");
  }
  showSplash("KRIED KDS");
  WiFi.onEvent(onWiFiEvent);
  setState(STATE_WIFI_CONNECTING);
}

// ── Loop ─────────────────────────────────────────────
void loop() {
  updateLED();
  updateFlip();       // runs flip animation steps non-blocking
  updateIconSwitch(); // alternates burger ↔ fries every 1 s

  // WiFi watchdog
  if (appState == STATE_WIFI_CONNECTING) {
    tryWifi();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setState(STATE_WIFI_CONNECTING);
    lastWifiAttempt = 0;
    return;
  }

  // Poll server every UPDATE_INTERVAL ms (only when animation is idle)
  if (flipPhase == FLIP_IDLE && millis() - lastDisplayUpdate >= UPDATE_INTERVAL) {
    lastDisplayUpdate = millis();

    String fetched = pollCurrentBill();
    if (fetched == "__ERROR__") {
      Serial.println("Poll failed — retrying next interval");
      return;
    }

    Serial.println("Poll OK — bill: '" + fetched + "'");

    if (fetched != currentBill) {
      Serial.println("Bill changed: " + currentBill + " → " + fetched);
      startFlip(currentBill == "__UNKNOWN__" ? "" : currentBill, fetched);
      currentBill    = fetched;
      showFries      = false;        // always start with burger after a flip
      lastIconSwitch = millis();
      setLedMode(LED_BLINK_FAST);
    }
  }
}