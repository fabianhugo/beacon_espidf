#include <Arduino.h>
#include <NimBLEDevice.h>
#include <FastLED.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_pm.h"

// ── Configuration — adjust per deployment ────────────────────────────────────

// Protocol magic — identifies beacons running THIS firmware (formerly GROUP_ID).
// Only advertisements carrying this byte are considered; foreign 0xFFFF beacons
// are ignored. Change it to isolate a deployment that must not interact.
static constexpr uint8_t PROTO_MAGIC = 0xBE;

// Manufacturer-data layout (MFR_LEN bytes):
//   [0]=0xFF [1]=0xFF   company id (unregistered / test range)
//   [2]=PROTO_MAGIC
//   [3]=flags           bit0 (FLAG_PAIRING) = sender is currently in pairing mode
//   [4..7]=node id       little-endian, derived from the chip MAC
static constexpr size_t  MFR_LEN      = 8;
static constexpr uint8_t FLAG_PAIRING = 0x01;

// RSSI threshold above which a paired peer is considered "close" (full magenta).
// NOTE: colour only, NOT detection range — any decodable packet from a paired
// peer marks it present. Coded PHY sensitivity bottoms out around -103 dBm.
//   -65 dBm ≈ 1 m  |  -75 dBm ≈ 3 m  |  -85 dBm ≈ 8 m  |  -95 dBm ≈ 25 m
static constexpr int8_t RSSI_NEAR = -95;

// How long without a packet before a paired peer is considered gone (ms).
static constexpr uint32_t TIMEOUT_MS = 8000;

// BLE advertising interval in units of 0.625 ms. 1600 = 1000 ms.
static constexpr uint16_t ADV_INTERVAL = 1600;

// TX power in dBm. ESP32-S3 tops out near +20 dBm; the chip clamps to its
// ceiling and the boot log prints what was actually applied.
static constexpr int TX_POWER_DBM = 20;

// Pairing.
static constexpr int      MAX_PAIRS         = 16;    // max paired peers stored
static constexpr uint32_t PAIRING_WINDOW_MS = 10000; // long-press pairing window
static constexpr uint32_t FORGET_HOLD_MS    = 10000; // hold BOOT this long to forget all

// LED strip wiring. Override per board from platformio.ini build_flags
// (e.g. -DLED_PIN=10 -DNUM_LEDS=1); the defaults below target the lolin_s3_mini.
#ifndef LED_PIN
#define LED_PIN 48          // WS2812B data GPIO (48 = onboard LED on lolin_s3_mini)
#endif
#ifndef NUM_LEDS
#define NUM_LEDS 10         // strip length
#endif
static constexpr uint8_t BRIGHTNESS = 60; // global FastLED brightness (0–255)

// Peak per-channel brightness for every LED effect (0–255). One knob to dim all
// modes together — idle rainbow, proximity magenta/gradient, pairing pulse and
// the feedback flashes. The global FastLED BRIGHTNESS scales on top of this.
static constexpr uint8_t MAX_BRIGHT = 128;

// BOOT button (active-low). Short press = radio on/off, long press = pairing.
// GPIO0 on most ESP32-S3 boards; GPIO9 on most ESP32-C3 boards incl. Xiao C3.
#ifndef BOOT_BUTTON_PIN
#define BOOT_BUTTON_PIN 0
#endif
static constexpr uint32_t LONG_PRESS_MS = 1000; // hold this long for a long press

// RSSI exponential moving average. Lower = smoother but slower to react.
static constexpr float ALPHA = 0.2f;

// ─────────────────────────────────────────────────────────────────────────────

CRGB leds[NUM_LEDS];

// This node's unique id, derived from the chip MAC in setup().
static uint32_t myId = 0;

// Radio on/off (short press) and pairing window (long press). Both are read on
// the BLE task and written on the loop task, so volatile.
static volatile bool radioEnabled = true;
static volatile bool pairingMode  = false;

// Peer proximity — updated by the scan callback ONLY when a *paired* peer is
// heard. uint32_t/int8_t single-word access is atomic on Xtensa/RISC-V.
static volatile bool     peerSeen   = false;
static volatile uint32_t lastSeenMs = 0;
static volatile int8_t   lastRSSI   = -127;
static float smoothRSSI = -127.0f;

// Paired-peer id set. Read on the BLE task (isPaired), written on the loop task
// (addPair) — guarded by a spinlock. Persisted to NVS.
static portMUX_TYPE pairMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t pairedIds[MAX_PAIRS];
static int      pairedCount = 0;

// New pairing candidates discovered on the BLE task, drained by loop() which
// owns the NVS write + LED feedback. FreeRTOS queues are task-safe.
static QueueHandle_t pairQueue = nullptr;

static Preferences prefs;

// ── Paired-set helpers ──────────────────────────────────────────────────────

static bool isPaired(uint32_t id) {
  bool found = false;
  portENTER_CRITICAL(&pairMux);
  for (int i = 0; i < pairedCount; i++) {
    if (pairedIds[i] == id) { found = true; break; }
  }
  portEXIT_CRITICAL(&pairMux);
  return found;
}

// Add to the in-RAM set. Returns false if already present or full. Loop task
// only; the caller persists with savePairs().
static bool addPair(uint32_t id) {
  if (isPaired(id)) return false;
  bool ok = false;
  portENTER_CRITICAL(&pairMux);
  if (pairedCount < MAX_PAIRS) { pairedIds[pairedCount++] = id; ok = true; }
  portEXIT_CRITICAL(&pairMux);
  return ok;
}

// NVS lives in its own partition and survives a normal `pio run -t upload`
// (only the app partition is rewritten). A full `erase_flash` still clears it.
static void loadPairs() {
  prefs.begin("beacon", false);
  size_t len = prefs.getBytes("pairs", pairedIds, sizeof(pairedIds));
  pairedCount = len / sizeof(uint32_t);
}

static void savePairs() {
  // Loop task is the only writer, so pairedIds is stable here.
  prefs.putBytes("pairs", pairedIds, pairedCount * sizeof(uint32_t));
}

// Wipe all pairings from RAM and NVS. Loop task only.
static void forgetAllPairs() {
  portENTER_CRITICAL(&pairMux);
  pairedCount = 0;
  portEXIT_CRITICAL(&pairMux);
  prefs.remove("pairs");
  peerSeen = false; // drop any current proximity lock
}

// ── BLE scan callback ───────────────────────────────────────────────────────

class BeaconCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveManufacturerData()) return;
    auto mfr = dev->getManufacturerData();
    if (mfr.size() < MFR_LEN) return;
    if (static_cast<uint8_t>(mfr[2]) != PROTO_MAGIC) return;

    const uint8_t  flags = static_cast<uint8_t>(mfr[3]);
    const uint32_t id    = static_cast<uint32_t>(static_cast<uint8_t>(mfr[4]))
                         | (static_cast<uint32_t>(static_cast<uint8_t>(mfr[5])) << 8)
                         | (static_cast<uint32_t>(static_cast<uint8_t>(mfr[6])) << 16)
                         | (static_cast<uint32_t>(static_cast<uint8_t>(mfr[7])) << 24);
    if (id == myId) return; // ignore our own echo, just in case

    // Pairing forms only when BOTH nodes are in pairing mode. Hand the
    // candidate to loop() (which owns NVS writes + feedback); don't touch flash
    // from the BLE task.
    if (pairingMode && (flags & FLAG_PAIRING) && !isPaired(id)) {
      xQueueSend(pairQueue, &id, 0);
    }

    // Proximity colour reacts to PAIRED peers only. Unpaired ids are ignored.
    if (isPaired(id)) {
      peerSeen   = true;
      lastSeenMs = millis();
      lastRSSI   = dev->getRSSI();
    }
  }

  // NimBLE can stop the scan after radio arbitration; restart it — unless the
  // user switched the radio off, or we'd fight the disable.
  void onScanEnd(const NimBLEScanResults&, int) override {
    if (radioEnabled) NimBLEDevice::getScan()->start(0);
  }
};

// ── Radio control & advertising ─────────────────────────────────────────────

// Single reusable callback instance: NimBLEScan doesn't delete its callbacks and
// deinit(true) deletes the scan object, so a `new` would leak on every re-enable.
static BeaconCallbacks scanCallbacks;

// (Re)build the extended advertisement with the current node id and pairing
// flag. `restart` stops instance 0 first (needed to update data on-air while
// advertising); at first start there is nothing to stop.
static void buildAdvert(bool pairing, bool restart) {
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  if (restart) adv->stop(0);

  NimBLEExtAdvertisement adData(BLE_HCI_LE_PHY_CODED, BLE_HCI_LE_PHY_CODED);
  adData.setTxPower(TX_POWER_DBM);
  adData.setLegacyAdvertising(false);
  adData.setScannable(false);
  adData.setConnectable(false);
  adData.setMinInterval(ADV_INTERVAL);
  adData.setMaxInterval(ADV_INTERVAL);

  uint8_t mfr[MFR_LEN] = {
      0xFF, 0xFF, PROTO_MAGIC,
      static_cast<uint8_t>(pairing ? FLAG_PAIRING : 0x00),
      static_cast<uint8_t>(myId & 0xFF),
      static_cast<uint8_t>((myId >> 8) & 0xFF),
      static_cast<uint8_t>((myId >> 16) & 0xFF),
      static_cast<uint8_t>((myId >> 24) & 0xFF),
  };
  adData.setManufacturerData(mfr, sizeof(mfr));

  adv->setInstanceData(0, adData);
  adv->start(0);
}

// Fully bring up BLE: init, configure Coded-PHY advertising + continuous scan.
// Safe to call after deinit() — getScan()/getAdvertising() recreate their objects.
static void startBle() {
  NimBLEDevice::init("beacon");
  NimBLEDevice::setPower(TX_POWER_DBM);

  Serial.printf("TX power applied: requested=%d  adv=%d  scan=%d dBm\n",
                TX_POWER_DBM,
                NimBLEDevice::getPower(NimBLETxPowerType::Advertise),
                NimBLEDevice::getPower(NimBLETxPowerType::Scan));

  buildAdvert(pairingMode, false); // configure + start advertising

  // Scan duty cycle: 30 ms RX out of every 100 ms (30%), not continuous.
  // Peers advertise once per second (ADV_INTERVAL) and TIMEOUT_MS allows 8 missed
  // adverts before a paired peer is dropped, so occasional misses are harmless —
  // P(catch at least one advert within TIMEOUT_MS) stays high even at this duty
  // cycle. Continuous scanning (window == interval) also blocks the radio from
  // ever going idle, which prevents esp_pm's automatic light sleep from engaging.
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(false);
  scan->setPhy(NimBLEScan::SCAN_CODED);
  scan->setInterval(160); // 100 ms period
  scan->setWindow(48);    // 30 ms RX per period → 30% duty cycle
  scan->setDuplicateFilter(0); // report every packet; must follow setScanCallbacks
  scan->start(0);

  Serial.printf("BLE beacon started  id=0x%08X  paired=%d  rssi_near=%d dBm\n",
                myId, pairedCount, RSSI_NEAR);
}

// Turn the radio fully on or off. ON re-initialises the stack; OFF stops
// advertising (per-instance — no-arg stop() uses ext_adv_clear which fails while
// active) and scanning, then deinit(true) powers down the BLE controller.
static void setRadio(bool on) {
  if (on) {
    startBle();
  } else {
    NimBLEDevice::getScan()->stop();
    NimBLEDevice::getAdvertising()->stop(0);
    pairingMode = false; // can't pair with the radio off
    peerSeen    = false; // let LED state lapse to idle
    NimBLEDevice::deinit(true);
  }
}

// ── Visual feedback ─────────────────────────────────────────────────────────

// Blink the whole strip a solid colour `times` times. Blocks briefly; the next
// loop() redraw restores the pattern.
static void flashFeedback(const CRGB& color, int times) {
  CRGB c = color;
  c.nscale8(MAX_BRIGHT); // cap feedback at the shared peak brightness
  for (int i = 0; i < times; i++) {
    fill_solid(leds, NUM_LEDS, c);
    FastLED.show();
    delay(120);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(120);
  }
}

// Pairing indicator: all pixels pulse yellow together — a clear "searching"
// heartbeat. Works identically for one LED or a full strip. Called every frame
// from loop() while pairing mode is active.
static void renderPairingScan() {
  uint8_t b = beatsin8(90, 20, MAX_BRIGHT); // ~1.5 Hz pulse
  fill_solid(leds, NUM_LEDS, CRGB(b, b, 0)); // equal red+green = yellow
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  // With ARDUINO_USB_CDC_ON_BOOT, Serial is native USB CDC and re-enumerates on
  // reset — wait up to 3 s for the host so boot prints aren't lost.
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) {
    delay(10);
  }
  delay(100);

  // Dynamic frequency scaling + automatic light sleep: the CPU runs at up to
  // 80 MHz (the lowest clock the BLE controller supports) while active, drops to
  // 40 MHz (XTAL rate) when idle, and light-sleeps entirely between FreeRTOS
  // ticks and scheduled BLE radio events. The BT stack holds its own PM lock
  // during active TX/RX, so this doesn't corrupt BLE timing. Config struct is
  // target-specific — no unified typedef across IDF targets.
#if CONFIG_IDF_TARGET_ESP32S3
  esp_pm_config_esp32s3_t pmConfig = {
#elif CONFIG_IDF_TARGET_ESP32C3
  esp_pm_config_esp32c3_t pmConfig = {
#endif
      .max_freq_mhz       = 80,
      .min_freq_mhz       = 40,
      .light_sleep_enable = true,
  };
  esp_err_t pmErr = esp_pm_configure(&pmConfig);
  Serial.printf("PM config: max=80 min=40 light_sleep=1  result=%s\n",
                pmErr == ESP_OK ? "OK" : esp_err_to_name(pmErr));

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP); // BOOT is active-low

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  // Unique id from the factory MAC (low 32 bits) — no storage needed for it.
  myId = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFFFULL);
  loadPairs();
  pairQueue = xQueueCreate(8, sizeof(uint32_t));

  Serial.printf("Node id=0x%08X  loaded %d pairing(s)\n", myId, pairedCount);

  startBle();
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void loop() {
  const uint32_t now = millis();

  static uint32_t pairingUntil = 0; // millis() deadline for the pairing window

  // ── BOOT button ─────────────────────────────────────────────────────────────
  //   short press (< 1 s) ....... radio on/off
  //   hold ≥ 1 s ................ pairing mode (fast rainbow, 10 s window)
  //   hold ≥ 10 s ............... forget all pairings (passes through pairing)
  static bool     btnDown     = false;
  static uint32_t btnDownAt   = 0;
  static bool     longFired   = false;
  static bool     forgetFired = false;
  const bool      pressed     = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  const uint32_t  held        = now - btnDownAt; // only meaningful while btnDown

  if (pressed && !btnDown) {                 // press began
    btnDown     = true;
    btnDownAt   = now;
    longFired   = false;
    forgetFired = false;
  } else if (pressed && btnDown && !longFired && held >= LONG_PRESS_MS) {
    longFired = true;                        // ≥1 s → enter pairing mode
    if (radioEnabled) {
      pairingMode  = true;
      pairingUntil = now + PAIRING_WINDOW_MS;
      buildAdvert(true, true);               // advertise the pairing flag
      Serial.println("Pairing mode ON (10 s)");
    } else {
      Serial.println("Long press ignored — radio is off");
    }
  } else if (pressed && btnDown && !forgetFired && held >= FORGET_HOLD_MS) {
    forgetFired = true;                      // ≥10 s → forget all pairings
    pairingMode = false;                     // cancel the pairing window we passed through
    if (radioEnabled) buildAdvert(false, true);
    forgetAllPairs();
    Serial.println("All pairings FORGOTTEN (10 s hold)");
    flashFeedback(CRGB::Red, 5);
  } else if (!pressed && btnDown) {          // released
    btnDown = false;
    if (!longFired && !forgetFired) {        // SHORT PRESS → toggle radio
      radioEnabled = !radioEnabled;
      setRadio(radioEnabled);
      flashFeedback(radioEnabled ? CRGB::Green : CRGB::Red, 3);
      Serial.printf("Radio %s (short press)\n", radioEnabled ? "ENABLED" : "DISABLED");
    }
  }

  // Drain pairing candidates found on the BLE task; persist + acknowledge each.
  if (pairQueue) {
    uint32_t id;
    while (xQueueReceive(pairQueue, &id, 0) == pdTRUE) {
      if (addPair(id)) {
        savePairs();
        Serial.printf("Paired with 0x%08X  (total %d)\n", id, pairedCount);
        flashFeedback(CRGB::Blue, 5);
      }
    }
  }

  // Expire the pairing window and drop the pairing flag from the advert.
  if (pairingMode && now >= pairingUntil) {
    pairingMode = false;
    if (radioEnabled) buildAdvert(false, true);
    Serial.println("Pairing mode OFF");
  }

  // ── Proximity → colour (paired peers only) ──────────────────────────────────
  const bool   nearby = peerSeen && (now - lastSeenMs) < TIMEOUT_MS;
  const int8_t rssi   = lastRSSI;
  if (nearby) {
    smoothRSSI = ALPHA * rssi + (1.0f - ALPHA) * smoothRSSI;
  } else {
    smoothRSSI = -127.0f;
  }

  static uint8_t hue = 0;
  if (pairingMode) {
    renderPairingScan(); // white sweeping dot / pulse — "searching"
  } else if (!nearby) {
    hue += 1; // slow idle rainbow
    fill_rainbow(leds, NUM_LEDS, hue, 255 / NUM_LEDS);
    for (int i = 0; i < NUM_LEDS; i++) leds[i].nscale8(MAX_BRIGHT); // cap peak
  } else if (smoothRSSI >= RSSI_NEAR) {
    uint8_t pulse = beatsin8(40, 40, MAX_BRIGHT); // ~1.5 s breathe
    fill_solid(leds, NUM_LEDS, CRGB(pulse, 0, pulse));
  } else {
    float t = constrain((smoothRSSI + 127.0f) / (RSSI_NEAR + 127.0f), 0.0f, 1.0f);
    fill_solid(leds, NUM_LEDS, CRGB(static_cast<uint8_t>(MAX_BRIGHT * t), 0, MAX_BRIGHT));
  }
  FastLED.show();

  // Diagnostic line throttled to ~1 Hz (was every 100 ms loop) — status doesn't
  // need finer resolution and this cuts USB CDC activity by 10x. Button/pairing
  // events above still log immediately regardless of this gate.
  static uint32_t lastStatusPrint = 0;
  if (now - lastStatusPrint >= 1000) {
    lastStatusPrint = now;
    Serial.printf("nearby=%d  rssi=%d  smooth=%.1f  pairing=%d  paired=%d\n",
                  nearby, rssi, smoothRSSI, pairingMode, pairedCount);
  }

  delay(100);
}
