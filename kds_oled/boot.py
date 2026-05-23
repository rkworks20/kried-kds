# boot.py — Kried KDS: OTA check before main.py starts
# This file is uploaded once via Thonny and rarely changes.
# It connects WiFi, checks GitHub for a newer main.py, downloads it, then
# falls through into main.py.  If WiFi fails OTA is skipped silently.

import network, time, machine, os, gc

WIFI_NETWORKS = [
    ("Airtel_Rk?s Wifi", "wifi1234"),
]

# Read the installed version from a local file written after each OTA update.
# Falls back to "0.0.0" on first boot so the initial download always runs.
def _read_local_version():
    try:
        with open("fw_version.txt") as f:
            return f.read().strip()
    except:
        return "0.0.0"

VERSION = _read_local_version()

OTA_VERSION_URL = "https://raw.githubusercontent.com/rkworks20/kried-kds/main/kds_oled/version.txt"
OTA_MAIN_URL    = "https://raw.githubusercontent.com/rkworks20/kried-kds/main/kds_oled/main.py"

WIFI_TIMEOUT = 10000  # ms per network attempt

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
    for ssid, password in WIFI_NETWORKS:
        if wlan.isconnected():
            break
        splash("Connecting...", ssid[:16])
        wlan.disconnect()
        time.sleep_ms(200)
        wlan.connect(ssid, password)
        deadline = time.ticks_add(time.ticks_ms(), WIFI_TIMEOUT)
        while not wlan.isconnected():
            if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                splash("Trying next...", ssid[:16])
                time.sleep_ms(400)
                break
            time.sleep_ms(200)
    if wlan.isconnected():
        return True
    splash("WiFi failed", "skipping OTA")
    time.sleep_ms(1500)
    return False

# ── OTA ──────────────────────────────────────────────
def check_ota():
    try:
        import urequests

        # Free heap before making requests — prevents silent partial downloads
        gc.collect()

        splash("Checking OTA", "v" + VERSION)
        r = urequests.get(OTA_VERSION_URL, timeout=10)
        latest = r.text.strip()
        r.close()
        del r
        gc.collect()  # free the response before the big download

        if latest == VERSION:
            splash("Firmware OK", "v" + VERSION)
            time.sleep_ms(800)
            return

        # ── Download new main.py ──────────────────────
        splash("Updating...", VERSION + ">" + latest)
        r = urequests.get(OTA_MAIN_URL, timeout=30)
        content = r.text
        r.close()
        del r
        gc.collect()

        # Validate: must be a real Python file, not an error page or empty body
        if len(content) < 500 or not content.startswith("#"):
            splash("OTA error", "bad download")
            time.sleep_ms(2000)
            return

        # Write to a temp file first — never touch main.py until we're sure
        with open("main_ota.py", "w") as f:
            f.write(content)
        del content
        gc.collect()

        # Verify the temp file was written completely
        try:
            written = os.stat("main_ota.py")[6]  # index 6 = file size in bytes
        except:
            written = 0

        if written < 500:
            splash("OTA error", "write failed")
            time.sleep_ms(2000)
            try:
                os.remove("main_ota.py")
            except:
                pass
            return

        # Atomic swap: remove old main.py, rename temp into place
        try:
            os.remove("main.py")
        except:
            pass
        os.rename("main_ota.py", "main.py")

        # Only save the new version AFTER main.py is confirmed on disk
        with open("fw_version.txt", "w") as f:
            f.write(latest)

        splash("Done! v" + latest, "rebooting...")
        time.sleep_ms(1000)
        machine.reset()

    except Exception as e:
        splash("OTA error", str(e)[:16])
        time.sleep_ms(2000)

if connect_wifi():
    check_ota()

# Fall through into main.py
