# BLE_BEACON

Minimal proximity-pairing beacon firmware for ESP32-S3 / ESP32-C3.
No WLED, no WiFi — just BLE 5 Coded PHY advertising + scanning and a WS2812B strip.

Each node has a unique ID and continuously advertises it while scanning for peers.
Nodes **pair** with each other on demand; once two nodes are paired, they light up
(blue → magenta, pulsing when close) whenever they come within range of one another.
Unpaired nodes are ignored. When nothing paired is around, the strip idles on a slow
rainbow.

The on-board **BOOT button** controls everything: short press toggles the radio,
a long press starts pairing, a very long press forgets all pairings.

---

## Hardware

| Item | Notes |
|---|---|
| MCU | ESP32-S3 (`beacon_s3` → `lolin_s3_mini`) or ESP32-C3 (`beacon_c3` → `seeed_xiao_esp32c3`) |
| LED strip | WS2812B (or any FastLED-compatible RGB strip); works with a single LED too |
| LED data pin | `LED_PIN` — 48 (S3 default) / 10 (C3 env). Set per board via build flags |
| Button | **BOOT** button, active-low. `BOOT_BUTTON_PIN` — GPIO 0 (S3) / GPIO 9 (C3) |
| Power | 5 V via USB or regulated supply; strip current dominates — see *Range & power* |

Per-board pins (`LED_PIN`, `NUM_LEDS`, `BOOT_BUTTON_PIN`) are set from
`platformio.ini` build flags, so no source edits are needed to switch boards. The
defaults compiled into `main.cpp` target the lolin_s3_mini.

---

## Build & flash

Requires [PlatformIO](https://platformio.org/) (CLI or VS Code extension).

```sh
# S3 board (lolin_s3_mini)
pio run -e beacon_s3 -t upload

# C3 board (Seeed Xiao ESP32-C3)
pio run -e beacon_c3 -t upload

# Monitor serial output (115200 baud)
pio device monitor
```

First build downloads NimBLE-Arduino and FastLED automatically.

> **Seeing the boot log:** `Serial` is native USB CDC, so `setup()` waits up to 3 s
> for the monitor to attach. Start `pio device monitor` (or press reset with it open)
> within that window to catch the `Node id=…` / `TX power applied:` / `BLE beacon
> started` lines.

> **Pairings survive firmware uploads.** They live in the NVS partition; a normal
> `pio run -t upload` only rewrites the app. A full `esptool erase_flash` wipes them.

---

## Controls (BOOT button)

| Action | Effect | Feedback |
|---|---|---|
| **Short press** (< 1 s) | Toggle the radio on/off. Off = advertising **and** scanning stop and the BLE controller is fully powered down. | 3 flashes — **green** = on, **red** = off |
| **Hold ≥ 1 s** | Enter **pairing mode** for 10 s (see below) | pulsing **yellow** while active |
| **Hold ≥ 10 s** | **Forget all pairings** (RAM + NVS). Passes through pairing mode on the way. | 5 **red** flashes |

Notes:
- Pairing requires the radio to be on; a long press with the radio off is ignored.
- Forgetting works regardless of radio state (it just clears storage).
- To reach the 10 s "forget", keep holding through the yellow pulse — at 10 s it
  cancels pairing and wipes everything.

---

## Pairing

Pairing is **mutual**: two nodes bond only when **both** are in pairing mode at the
same time.

1. Long-press BOOT on node A → it pulses yellow and advertises a "pairing" flag for 10 s.
2. Do the same on node B within that window.
3. Each node hears the other's flagged advertisement, stores the peer's ID, and
   confirms with **5 blue flashes**. The bond is saved to NVS immediately.

From then on, whenever the two paired nodes are within radio range they react to each
other (blue → magenta / pulsing). Each node can hold up to `MAX_PAIRS` (16) peers, and
every node can pair with every other node.

IDs are derived from the chip's factory MAC, so they're unique with no configuration.

---

## LED behaviour

| State | Appearance |
|---|---|
| Pairing mode active | **Yellow** pulse on all pixels (~1.5 s) |
| No paired peer nearby (idle) | Slowly cycling **rainbow** |
| Paired peer detected, distant | **Blue → magenta** gradient (brighter as RSSI rises toward `RSSI_NEAR`) |
| Paired peer above `RSSI_NEAR` | **Pulsing magenta** (~1.5 s breathe) |

Only **paired** peers drive the proximity colours; unpaired beacons nearby leave the
node on its idle rainbow.

All effects share one peak-brightness knob, `MAX_BRIGHT` (128), which the global
FastLED `BRIGHTNESS` (60) then scales on top of.

---

## Tuning

All knobs are at the top of `src/main.cpp`:

| Constant | Default | Effect |
|---|---|---|
| `PROTO_MAGIC` | `0xBE` | Identifies beacons of this firmware. Only matching adverts are considered; change to isolate a deployment. |
| `RSSI_NEAR` | `-95 dBm` | Colour-only threshold for "close" (full/pulsing magenta). **Does not** set detection range — any decodable packet from a paired peer marks it present. |
| `TIMEOUT_MS` | `8000 ms` | How long without a packet before a paired peer is considered gone. Must exceed `ADV_INTERVAL` with margin. |
| `ADV_INTERVAL` | `1600 × 0.625 ms = 1000 ms` | How often each node advertises. Lower = faster detection, higher average current. |
| `TX_POWER_DBM` | `20 dBm` | Transmit power; the chip clamps to its ceiling (~+18…+20 dBm). The boot log prints the value actually applied. May exceed regional regulatory limits. |
| `MAX_PAIRS` | `16` | Max paired peers stored per node. |
| `PAIRING_WINDOW_MS` | `10000 ms` | Duration of the pairing window after a long press. |
| `FORGET_HOLD_MS` | `10000 ms` | Hold BOOT this long to forget all pairings. |
| `LONG_PRESS_MS` | `1000 ms` | Press shorter than this = radio toggle; longer = pairing. |
| `ALPHA` | `0.2` | RSSI smoothing factor. Lower = smoother but slower to react. |
| `MAX_BRIGHT` | `128` | Peak per-channel brightness for **all** effects (one knob). |
| `BRIGHTNESS` | `60` | Global FastLED brightness (0–255), scales on top of `MAX_BRIGHT`. Biggest single lever for power. |
| `LED_PIN` / `NUM_LEDS` / `BOOT_BUTTON_PIN` | 48 / 10 / 0 | Wiring; overridden per board from `platformio.ini` build flags. |

Scan duty cycle is **30 %** (30 ms RX out of every 100 ms) rather than continuous —
peers advertise once per second and `TIMEOUT_MS` tolerates 8 missed adverts, so
occasional misses are harmless while RX time drops to 30 % of before. The CPU runs
under `esp_pm_configure`: up to 80 MHz (the lowest clock the BLE controller
supports) while active, down to 40 MHz idle, with **automatic light sleep** enabled
between FreeRTOS ticks and scheduled BLE events. Continuous scanning would prevent
light sleep from ever engaging, since the radio would never go idle — cutting scan
duty cycle and enabling light sleep are complementary, not independent, levers.

---

## Range & power

> All figures below are **rough estimates**, not bench measurements. Actual numbers
> depend heavily on antenna, enclosure, environment, and your strip. Measure your own
> setup before relying on these.

### Range

Detection range is set by TX power + Coded-PHY receiver sensitivity (~-103 dBm) and,
indoors, is usually **limited by 2.4 GHz interference and the board's small PCB/chip
antenna** rather than by link budget. Both nodes at +20 dBm on Coded PHY:

| Situation | Approximate range |
|---|---|
| Open air, line of sight | 50–150 m (antenna-limited; far less than the theoretical Coded-PHY maximum) |
| Office / indoors | 10–30 m |
| Through walls / floors | 5–15 m |

Notes:
- **Frequency / channel choice barely affects range** (~0.3 dB across the band). The
  three advertising channels (37/38/39) are used together for frequency diversity.
- Elevation and keeping the boards away from bodies, metal, and ground planes typically
  helps more than any radio setting.
- An **external antenna** is the real range unlock, but these boards have only a PCB
  antenna and no u.FL connector.

### Current draw

Powered from 5 V over USB. The **LED strip dominates** — at `BRIGHTNESS=60` /
`MAX_BRIGHT=128` a lit pixel draws roughly 4–8 mA, so 10 px ≈ 30–70 mA. Radio and CPU
are smaller:

| Subsystem | Approx. average |
|---|---|
| CPU with `esp_pm` light sleep (80/40 MHz, USB CDC active) | ~5–12 mA |
| BLE advertising @ 1 s interval, +20 dBm | ~1–3 mA (tiny duty cycle, brief high-power bursts) |
| BLE Coded-PHY scan @ 30 % duty cycle | ~5–10 mA |
| LED strip (10 px, `BRIGHTNESS=60`, `MAX_BRIGHT=128`) | ~30–70 mA |

Indicative whole-system current (10-pixel strip):

| State | Approx. total @ 5 V |
|---|---|
| Radio **on**, idle rainbow | ~40–90 mA |
| Radio **on**, paired peer near (pulsing, averages dimmer) | ~35–80 mA |
| Radio **off** (short press): BLE powered down, rainbow only | ~35–80 mA |

These are rough deltas from the previous continuous-scan/fixed-80 MHz build (see git
history), not bench measurements — light sleep effectiveness depends on how much idle
time `esp_pm` actually finds between BLE events, which you should verify on real
hardware with a current meter.

To go lower still: reduce `MAX_BRIGHT` / `BRIGHTNESS` / `NUM_LEDS` (biggest remaining
lever — the LED strip now dominates), drop the scan duty cycle further (impacts
detection latency), or sleep the MCU while the radio is off (not implemented).

---

## Debugging

Serial output (115200 baud). On boot:

```
Node id=0x1A2B3C4D  loaded 2 pairing(s)
TX power applied: requested=20  adv=20  scan=20 dBm
BLE beacon started  id=0x1A2B3C4D  paired=2  rssi_near=-95 dBm
```

Then one line per loop (~100 ms):

```
nearby=1  rssi=-74  smooth=-76.3  pairing=0  paired=2
```

Button and pairing events also log, e.g. `Pairing mode ON (10 s)`,
`Paired with 0x… (total N)`, `Radio DISABLED (short press)`,
`All pairings FORGOTTEN (10 s hold)`.

If `TX power applied:` shows less than requested, the chip clamped to its hardware
ceiling — that is the true maximum, no software change will exceed it.

**Coded PHY is not visible to standard phone BLE scanners** (iOS/Android only support
legacy BLE 4.x advertising). To debug from a phone, temporarily set
`adData.setLegacyAdvertising(true)` in `buildAdvert()` — this drops to BLE 4.x range but
makes the packet visible to nRF Connect etc.

---

## How it works

1. Each node derives a unique 32-bit ID from its factory MAC. On boot (and on every
   radio re-enable) it starts an **extended BLE 5 advertisement** on Coded PHY
   (S=8, 125 kbps) carrying an 8-byte manufacturer payload:
   `[0xFF, 0xFF, PROTO_MAGIC, flags, id0..3]`. `flags` bit 0 signals "in pairing mode".

2. Simultaneously it runs a **passive extended scan** on Coded PHY (continuous). The
   controller duplicate filter is disabled so every packet refreshes the peer's
   last-seen time (otherwise an unchanging beacon is reported once and would falsely
   "time out").

3. The scan callback ignores foreign adverts (wrong `PROTO_MAGIC`). For a matching one:
   - if **both** this node and the sender are in pairing mode, the sender's ID is queued
     to be added to the paired set (stored in NVS by the main loop);
   - if the sender's ID is already **paired**, its `millis()` and RSSI are recorded.

4. `loop()` smooths the paired peer's RSSI (exponential moving average) and maps it to
   the LED behaviour in *LED behaviour*, handles the BOOT button (radio toggle / pairing
   / forget), and persists new pairings. NVS writes and LED feedback happen on the loop
   task, never in the BLE callback.

Coded PHY improves receiver sensitivity by ~5 dB over standard BLE 1M, roughly 1.8× more
range at the same TX power — though indoors that advantage is often masked by interference.
