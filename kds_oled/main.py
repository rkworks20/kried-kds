# main.py — Kried KDS OLED display
# MicroPython on ESP32-S3  |  v2.1.3
# OTA: bump version.txt + push this file to GitHub → device updates on next boot.

import network, time, math, framebuf
from machine import Pin, PWM, I2C
import urequests
import ssd1306

# ── Config ───────────────────────────────────────────
WIFI_SSID       = "Airtel_Rk?s Wifi"
WIFI_PASSWORD   = "wifi1234"
SERVER_HOST     = "kried-kds-production.up.railway.app"
UPDATE_INTERVAL = 3000
WIFI_RETRY      = 5000
WIFI_TIMEOUT    = 15000

# ── Hardware ─────────────────────────────────────────
i2c  = I2C(0, scl=Pin(6), sda=Pin(5), freq=400_000)
oled = ssd1306.SSD1306_I2C(128, 32, i2c)
led  = PWM(Pin(21), freq=5000, duty=0)

# ── Kried logo — 81×32 px ────────────────────────────
_logo_raw = bytes([
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x80,
    0xff,0xe0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x80,
    0xc7,0xc0,0x40,0x00,0x78,0x40,0x00,0x04,0x00,0x3f,0x80,
    0xc7,0xc0,0xc0,0x00,0x38,0x40,0x00,0x04,0x00,0x0f,0x80,
    0xc7,0xc0,0xc0,0x00,0x18,0x40,0x00,0x04,0x00,0x0f,0x80,
    0xc7,0xc1,0xc0,0x00,0x18,0x40,0x00,0x04,0x00,0x07,0x80,
    0xc7,0x81,0xc0,0x00,0x08,0x40,0x00,0x04,0x00,0x03,0x80,
    0xc7,0x03,0xc0,0x00,0x08,0x40,0x00,0x04,0x00,0x03,0x80,
    0xc7,0x07,0xc0,0x00,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc0,0x0f,0xc0,0x00,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc0,0x03,0xc0,0x00,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc0,0x01,0xc0,0x00,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc7,0x80,0xc3,0xfc,0x08,0x40,0xff,0xfc,0x00,0x01,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x43,0xff,0xfc,0x00,0x00,0x80,
    0xc7,0xc0,0x43,0xfe,0x18,0x43,0xff,0xfc,0x00,0x00,0x80,
    0xc7,0xc0,0x43,0xfe,0x38,0x41,0xff,0xfc,0x00,0x00,0x80,
    0xc7,0xc0,0x40,0x00,0x38,0x40,0x03,0xfc,0x3f,0x00,0x80,
    0xc7,0xc0,0x40,0x00,0x18,0x40,0x03,0xfc,0x7f,0x00,0x80,
    0xc7,0xc0,0x40,0x00,0x08,0x40,0x03,0xfc,0x7f,0x00,0x80,
    0xc7,0xc0,0x43,0xfc,0x08,0x41,0xff,0xfc,0x3e,0x00,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x43,0xff,0xfc,0x00,0x00,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x43,0xff,0xfc,0x00,0x00,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x41,0xff,0xfc,0x00,0x00,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x01,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x03,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x03,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x07,0x80,
    0xc7,0xc0,0x43,0xfe,0x08,0x40,0x00,0x04,0x00,0x0f,0x80,
    0xc7,0xe0,0x63,0xfe,0x0c,0x60,0x00,0x06,0x00,0x7f,0x80,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x80,
])
LOGO_BMP = bytearray(b ^ 0xff for b in _logo_raw)
logo_fb  = framebuf.FrameBuffer(LOGO_BMP, 81, 32, framebuf.MONO_HLSB)
LOGO_X   = (128 - 81) // 2

# ── 3×5 Mega Block font ───────────────────────────────
# Each char: 5 rows, each row is a 3-bit value (bit2=left col, bit0=right col)
_FONT3 = {
    '0': [0b111, 0b101, 0b101, 0b101, 0b111],
    '1': [0b110, 0b010, 0b010, 0b010, 0b111],
    '2': [0b111, 0b001, 0b111, 0b100, 0b111],
    '3': [0b111, 0b001, 0b111, 0b001, 0b111],
    '4': [0b101, 0b101, 0b111, 0b001, 0b001],
    '5': [0b111, 0b100, 0b111, 0b001, 0b111],
    '6': [0b111, 0b100, 0b111, 0b101, 0b111],
    '7': [0b111, 0b001, 0b001, 0b010, 0b010],
    '8': [0b111, 0b101, 0b111, 0b101, 0b111],
    '9': [0b111, 0b101, 0b111, 0b001, 0b111],
    '-': [0b000, 0b000, 0b111, 0b000, 0b000],
    ' ': [0b000, 0b000, 0b000, 0b000, 0b000],
}

def draw_mega(text):
    """Draw text as 3×5 mega blocks, auto-sized to fill the screen."""
    n = len(text)
    if n == 0:
        return
    gap = 4                                       # px between chars
    dy  = 6                                       # dot height: 5×6 = 30px, fits in 32
    dx  = (124 - (n - 1) * gap) // (3 * n)      # dot width from available space
    dx  = max(4, min(dx, 12))                     # clamp: min 4, max 12
    char_w = 3 * dx
    total  = n * char_w + (n - 1) * gap
    x0     = (128 - total) // 2
    y0     = (32 - 5 * dy) // 2                  # vertically centred
    for i, ch in enumerate(text):
        rows = _FONT3.get(ch, _FONT3[' '])
        cx = x0 + i * (char_w + gap)
        for row, bits in enumerate(rows):
            for col in range(3):
                if bits & (0b100 >> col):
                    # dx-1, dy-1 leave a 1 px gap between blocks
                    oled.fill_rect(cx + col * dx, y0 + row * dy, dx - 1, dy - 1, 1)

# ── OLED drawing ─────────────────────────────────────
def splash(line1, line2=""):
    oled.fill(0)
    oled.text(line1[:16], 0, 8, 1)
    if line2:
        oled.text(line2[:16], 0, 20, 1)
    oled.show()

def draw_content(bill):
    oled.fill(0)
    if not bill or bill == "__UNKNOWN__":
        oled.blit(logo_fb, LOGO_X, 0)
    else:
        draw_mega(bill)

def draw_final(bill):
    draw_content(bill)
    oled.show()

def draw_squished(bill, h):
    oled.fill(0)
    y_top = max(0, 16 - h // 2)
    y_bot = min(32, y_top + h)
    if not bill or bill == "__UNKNOWN__":
        oled.blit(logo_fb, LOGO_X, 0)
    else:
        draw_mega(bill)
    if y_top > 0:  oled.fill_rect(0, 0,     128, y_top,      0)
    if y_bot < 32: oled.fill_rect(0, y_bot, 128, 32 - y_bot, 0)
    oled.show()

# ── Flip animation (non-blocking) ─────────────────────
FLIP_IDLE, FLIP_SHRINK, FLIP_FOLD, FLIP_GROW = 0, 1, 2, 3
FLIP_STEP_MS = 25
FLIP_FOLD_MS = 40

_flip_phase = FLIP_IDLE
_flip_from  = ""
_flip_to    = ""
_flip_h     = 32
_flip_t     = 0

def start_flip(from_bill, to_bill):
    global _flip_phase, _flip_from, _flip_to, _flip_h, _flip_t
    _flip_from  = from_bill
    _flip_to    = to_bill
    _flip_h     = 32
    _flip_phase = FLIP_SHRINK
    _flip_t     = time.ticks_ms()

def update_flip():
    global _flip_phase, _flip_h, _flip_t
    if _flip_phase == FLIP_IDLE:
        return
    interval = FLIP_FOLD_MS if _flip_phase == FLIP_FOLD else FLIP_STEP_MS
    if time.ticks_diff(time.ticks_ms(), _flip_t) < interval:
        return
    _flip_t = time.ticks_ms()
    if _flip_phase == FLIP_SHRINK:
        _flip_h -= 4
        if _flip_h <= 2:
            oled.fill(0)
            oled.hline(0, 15, 128, 1)
            oled.hline(0, 16, 128, 1)
            oled.show()
            _flip_phase = FLIP_FOLD
        else:
            draw_squished(_flip_from, _flip_h)
    elif _flip_phase == FLIP_FOLD:
        _flip_h     = 2
        _flip_phase = FLIP_GROW
        draw_squished(_flip_to, _flip_h)
    elif _flip_phase == FLIP_GROW:
        _flip_h += 4
        if _flip_h >= 32:
            _flip_phase = FLIP_IDLE
            draw_final(_flip_to)
        else:
            draw_squished(_flip_to, _flip_h)

# ── LED ───────────────────────────────────────────────
LED_OFF, LED_BREATHE, LED_BLINK = 0, 1, 2
_led_mode    = LED_OFF
_led_mode_t  = 0
_led_tick_t  = 0
_breathe_val = 0.0
_breathe_dir = 1.0
_blink_state = False

def set_led(mode):
    global _led_mode, _led_mode_t, _blink_state
    _led_mode    = mode
    _led_mode_t  = time.ticks_ms()
    _blink_state = False
    if mode == LED_OFF:
        led.duty(0)

def update_led():
    global _breathe_val, _breathe_dir, _blink_state, _led_tick_t, _led_mode
    now = time.ticks_ms()
    if _led_mode == LED_BREATHE:
        if time.ticks_diff(now, _led_tick_t) < 16: return
        _led_tick_t   = now
        _breathe_val += _breathe_dir * 0.02
        if _breathe_val >= 1.0: _breathe_val = 1.0; _breathe_dir = -1.0
        if _breathe_val <= 0.0: _breathe_val = 0.0; _breathe_dir =  1.0
        led.duty(int((math.sin(_breathe_val * math.pi) + 1.0) / 2.0 * 1023))
    elif _led_mode == LED_BLINK:
        if time.ticks_diff(now, _led_tick_t) < 80: return
        _led_tick_t  = now
        _blink_state = not _blink_state
        led.duty(1023 if _blink_state else 0)
        if time.ticks_diff(now, _led_mode_t) > 1000:
            set_led(LED_BREATHE)

# ── WiFi ─────────────────────────────────────────────
wlan = network.WLAN(network.STA_IF)
if not wlan.active():
    wlan.active(True)

_was_connected = False
_last_wifi_t   = 0
_last_poll_t   = 0
current_bill   = "__UNKNOWN__"

def try_wifi():
    global _last_wifi_t
    _last_wifi_t = time.ticks_ms()
    splash("Connecting...", WIFI_SSID[:16])
    if not wlan.isconnected():
        wlan.connect(WIFI_SSID, WIFI_PASSWORD)
    deadline = time.ticks_add(time.ticks_ms(), WIFI_TIMEOUT)
    while not wlan.isconnected():
        if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
            splash("WiFi failed")
            time.sleep_ms(1000)
            return
    splash("Connected!", wlan.ifconfig()[0])
    time.sleep_ms(400)

def poll_bill():
    global current_bill
    try:
        r = urequests.get("https://" + SERVER_HOST + "/current-bill", timeout=5)
        if r.status_code != 200:
            r.close(); return
        body = r.text
        r.close()
        idx = body.find('"bill":"')
        if idx < 0: return
        idx += 8
        end = body.find('"', idx)
        if end < 0: return
        fetched = body[idx:end]
        if fetched != current_bill:
            from_b = "" if current_bill == "__UNKNOWN__" else current_bill
            start_flip(from_b, fetched)
            current_bill = fetched
            set_led(LED_BLINK)
    except Exception as e:
        print("Poll error:", e)

# ── Main loop ─────────────────────────────────────────
splash("KRIED KDS")

while True:
    update_led()
    update_flip()
    now = time.ticks_ms()

    if not wlan.isconnected():
        if _was_connected:
            splash("WiFi lost...")
            set_led(LED_OFF)
            _was_connected = False
        if time.ticks_diff(now, _last_wifi_t) >= WIFI_RETRY:
            try_wifi()
        continue

    if not _was_connected:
        _was_connected = True
        set_led(LED_BREATHE)
        draw_final(current_bill)

    if _flip_phase == FLIP_IDLE and time.ticks_diff(now, _last_poll_t) >= UPDATE_INTERVAL:
        _last_poll_t = now
        poll_bill()
