#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Config ───────────────────────────────────────────
const char* WIFI_SSID     = "Airtel_Rk?s Wifi";
const char* WIFI_PASSWORD = "wifi1234";
const char* SERVER_IP     = "kried-kds-production.up.railway.app";  // ← your Railway domain
const int   SERVER_PORT   = 443;    // WSS uses port 443 (HTTPS)

#define UPDATE_INTERVAL      3000
#define WIFI_TIMEOUT_MS      15000
#define WIFI_RETRY_MS        5000
#define WS_CONNECT_TIMEOUT   8000
#define WS_RETRY_BASE_MS     2000
#define WS_RETRY_MAX_MS      30000
#define HEARTBEAT_INTERVAL   10000
#define HEARTBEAT_TIMEOUT    20000

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
#define OLED_ADDRESS  0x3C
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define LED_PIN 21

// ── App state ───────────────────────────────────────
enum AppState { STATE_WIFI_CONNECTING, STATE_WS_CONNECTING, STATE_CONNECTED, STATE_RETRY_WAIT };
AppState appState = STATE_WIFI_CONNECTING;
uint32_t stateEnteredAt  = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastWsAttempt   = 0;
uint32_t lastTrafficAt   = 0;
uint32_t wsRetryDelay    = WS_RETRY_BASE_MS;
bool     justConnected   = false;

using namespace websockets;
WebsocketsClient wsClient;

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
String   pendingBill       = "";
bool     pendingUpdate     = false;
uint32_t lastDisplayUpdate = 0;

// ── Flip animation state ─────────────────────────────
enum FlipPhase { FLIP_IDLE, FLIP_SHRINK, FLIP_FOLD, FLIP_GROW };
FlipPhase flipPhase    = FLIP_IDLE;
String    flipFromBill = "";
String    flipToBill   = "";
int       flipHeight   = 32;
uint32_t  lastFlipStep = 0;
#define   FLIP_STEP_MS 25
#define   FLIP_FOLD_MS 40

void setState(AppState s) { appState = s; stateEnteredAt = millis(); }
uint32_t timeInState() { return millis() - stateEnteredAt; }

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

// ── WebSocket ───────────────────────────────────────
void onMessage(WebsocketsMessage msg) {
  lastTrafficAt = millis();
  Serial.print("WS RX: ");
  Serial.println(msg.data());
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, msg.data());
  if (err) {
    Serial.printf("  JSON parse err: %s (len=%u)\n", err.c_str(), (unsigned)msg.data().length());
    return;
  }
  const char* type = doc["type"];
  if (!type) {
    Serial.println("  ignored: no 'type' field");
    return;
  }
  if (strcmp(type, "display") != 0) {
    Serial.printf("  ignored: type=%s (expected 'display')\n", type);
    return;
  }
  JsonArray arr = doc["bills"].as<JsonArray>();
  pendingBill   = arr.size() > 0 ? arr[0].as<String>() : "";
  pendingUpdate = true;
  Serial.printf("  queued pendingBill=%s\n", pendingBill.c_str());
}

void onEvent(WebsocketsEvent event, String data) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.println("Server connected");
    lastTrafficAt = millis();
    wsRetryDelay  = WS_RETRY_BASE_MS;
    setState(STATE_CONNECTED);
    showSplash("CONNECTED");
    setLedMode(LED_BREATHING);
    justConnected = true;
    currentBill   = "__UNKNOWN__";
    pendingUpdate = false;
  } else if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("Server disconnected");
    setState(STATE_RETRY_WAIT);
    showSplash("RECONNECTING...");
    setLedMode(LED_OFF);
  } else if (event == WebsocketsEvent::GotPing || event == WebsocketsEvent::GotPong) {
    lastTrafficAt = millis();
  }
}

void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.println("WiFi dropped");
    if (appState == STATE_CONNECTED || appState == STATE_WS_CONNECTING) {
      wsClient.close();
      setState(STATE_WIFI_CONNECTING);
      lastWifiAttempt = 0;
      showSplash("WiFi lost...");
      setLedMode(LED_OFF);
    }
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
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    // non-blocking-ish wait — yield to system
    yield();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected — " + WiFi.localIP().toString());
    showSplash("WiFi OK");
    setState(STATE_WS_CONNECTING);
    lastWsAttempt = 0;
  } else {
    Serial.println("WiFi failed — will retry");
    showSplash("WIFI FAIL");
  }
}

void tryWebSocket() {
  if (WiFi.status() != WL_CONNECTED) {
    setState(STATE_WIFI_CONNECTING);
    lastWifiAttempt = 0;
    return;
  }
  if (millis() - lastWsAttempt < wsRetryDelay && lastWsAttempt != 0) return;
  lastWsAttempt = millis();
  Serial.println("Attempting server connection...");
  showSplash("Connecting server...");
  wsClient.close();
  wsClient.onMessage(onMessage);
  wsClient.onEvent(onEvent);
  String url = "wss://"; url += SERVER_IP;  // Railway terminates TLS; no port needed
  bool ok = wsClient.connect(url);
  if (!ok) {
    Serial.println("Connect call returned false");
    setState(STATE_RETRY_WAIT);
    wsRetryDelay = min((uint32_t)WS_RETRY_MAX_MS, wsRetryDelay * 2);
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
  wsClient.addHeader("x-client-type", "esp32");
  WiFi.onEvent(onWiFiEvent);
  setState(STATE_WIFI_CONNECTING);
}

// ── Loop ─────────────────────────────────────────────
void loop() {
  updateLED();
  updateFlip(); // runs animation steps non-blocking

  switch (appState) {
    case STATE_WIFI_CONNECTING:
      tryWifi();
      break;
    case STATE_WS_CONNECTING:
      if (timeInState() > WS_CONNECT_TIMEOUT) {
        Serial.println("WS connect timeout");
        setState(STATE_RETRY_WAIT);
        wsRetryDelay = min((uint32_t)WS_RETRY_MAX_MS, wsRetryDelay * 2);
      } else if (lastWsAttempt == 0) {
        tryWebSocket();
      } else {
        wsClient.poll();
      }
      break;
    case STATE_CONNECTED:
      wsClient.poll();
      if (millis() - lastTrafficAt > HEARTBEAT_TIMEOUT) {
        Serial.println("Heartbeat timeout");
        wsClient.close();
        setState(STATE_RETRY_WAIT);
      }
      static uint32_t lastPing = 0;
      if (millis() - lastPing > HEARTBEAT_INTERVAL) {
        lastPing = millis();
        wsClient.ping();
      }
      if (WiFi.status() != WL_CONNECTED) {
        setState(STATE_WIFI_CONNECTING);
        lastWifiAttempt = 0;
      }
      break;
    case STATE_RETRY_WAIT:
      if (timeInState() > wsRetryDelay) {
        if (WiFi.status() == WL_CONNECTED) {
          setState(STATE_WS_CONNECTING);
          lastWsAttempt = 0;
        } else {
          setState(STATE_WIFI_CONNECTING);
          lastWifiAttempt = 0;
        }
      }
      break;
  }

  // Display update — only apply when not mid-animation
  if (flipPhase == FLIP_IDLE && millis() - lastDisplayUpdate >= UPDATE_INTERVAL) {
    lastDisplayUpdate = millis();
    if (appState == STATE_CONNECTED && (pendingUpdate || justConnected)) {
      if (justConnected) {
        justConnected = false;
        pendingUpdate = false;
        Serial.println("Reconnect refresh: " + (pendingBill == "" ? "WAITING" : pendingBill));
        drawFinal(pendingBill);
        currentBill = pendingBill;
        setLedMode(LED_BLINK_FAST);
      } else if (pendingBill != currentBill) {
        pendingUpdate = false;
        Serial.println("Bill changed: " + currentBill + " → " + pendingBill);
        startFlip(currentBill == "__UNKNOWN__" ? "" : currentBill, pendingBill);
        currentBill = pendingBill;
        setLedMode(LED_BLINK_FAST);
      } else {
        // Same value re-sent — keep flag in case the screen is desynced; force a redraw.
        pendingUpdate = false;
        drawFinal(currentBill);
      }
    }
  }
}