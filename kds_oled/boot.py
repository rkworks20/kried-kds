# boot.py — Kried KDS: OTA check before main.py starts
# This file is uploaded once via Thonny and rarely changes.
# It connects WiFi, checks GitHub for a newer main.py, downloads it, then
# falls through into main.py.  If WiFi fails OTA is skipped silently.

import network, time, machine, os

WIFI_SSID     = "Airtel_Rk?s Wifi"
WIFI_PASSWORD = "wifi1234"
VERSION       = "2.0.0"

OTA_VERSION_URL = "https://raw.githubusercontent.com/rkworks20/kried-kds/main/kds_oled/version.txt"
OTA_MAIN_URL    = "https://raw.githubusercontent.com/rkworks20/kried-kds/main/kds_oled/main.py"

WIFI_TIMEOUT = 15000  # ms

# ── OLED (optional — gracefully skipped if driver not installed yet) ─────────
oled = None
try:
    from machine import I2C, Pin
    import ssd1306
    i2c  = I2C(0, scl=Pin(6), sda=Pin(5), freq=400_000)
    oled = ssd1306.SSD1306_I2C(128, 32, i2c)
except Exception:
    pass  # OLED unavailable — continue without display

def splash(line1, line2=""):
    if oled is None:
        return
    oled.fill(0)
    oled.text(line1, 0, 8, 1)
    if line2:
        oled.text(line2[:16], 0, 20, 1)
    oled.show()

splash("KRIED KDS", "booting...")

# ── WiFi ─────────────────────────────────────────────
wlan = network.WLAN(network.STA_IF)
wlan.active(True)

def connect_wifi():
    if wlan.isconnected():
        return True
    splash("Connecting...", WIFI_SSID[:16])
    wlan.connect(WIFI_SSID, WIFI_PASSWORD)
    deadline = time.ticks_add(time.ticks_ms(), WIFI_TIMEOUT)
    while not wlan.isconnected():
        if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
            splash("WiFi failed", "skipping OTA")
            time.sleep_ms(1500)
            return False
        time.sleep_ms(200)
    return True

# ── OTA ──────────────────────────────────────────────
def check_ota():
    try:
        import urequests
        splash("Checking OTA", "v" + VERSION)
        r = urequests.get(OTA_VERSION_URL, timeout=10)
        latest = r.text.strip()
        r.close()

        if latest == VERSION:
            splash("Firmware OK", "v" + VERSION)
            time.sleep_ms(800)
            return

        # Newer version available — download main.py
        splash("Updating...", VERSION + ">" + latest)
        r = urequests.get(OTA_MAIN_URL, timeout=30)
        content = r.text
        r.close()

        with open("main.py", "w") as f:
            f.write(content)

        splash("Done!", "rebooting...")
        time.sleep_ms(1000)
        machine.reset()

    except Exception as e:
        splash("OTA error", str(e)[:16])
        time.sleep_ms(2000)

if connect_wifi():
    check_ota()

# Fall through into main.py
