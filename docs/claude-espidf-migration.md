# ESP-IDF migration — proximity-pairing BLE beacon

Working notes and decision history for moving the beacon firmware from the
Arduino framework (`ble-beacon/`) to the hybrid ESP-IDF + Arduino-as-component
framework (`beacon_espidf/`), whose *sole* motivation is to unlock
BT-coordinated **automatic light sleep** (`CONFIG_PM_ENABLE`), which the plain
Arduino build cannot provide.

> **2026-07-21 correction.** An earlier version of this file was written against
> a stale checkout of `ble-beacon/src/main.cpp` (wrong git branch). This entry
> supersedes it. The authoritative source is `ble-beacon` on branch **`nonsleep`**
> (its `HEAD`, `31b397c "revert to non burst, non sleep mode"`). That tree also
> already carries two detailed topic docs which are the primary references:
> - [ble-beacon/docs/claude-power-optimization.md](../../ble-beacon/docs/claude-power-optimization.md)
> - [ble-beacon/docs/claude-device-color.md](../../ble-beacon/docs/claude-device-color.md)

---

## 2026-07-21 — Firmware summary (current `ble-beacon` @ `nonsleep`)

Read: [ble-beacon/src/main.cpp](../../ble-beacon/src/main.cpp) (authoritative,
current) vs [beacon_espidf/src/main.cpp](../src/main.cpp) (a spike that predates
several features — see "Current state" below).

### Concept

Minimal proximity-pairing beacon for ESP32-C3 / ESP32-S3. No WiFi, no WLED —
BLE 5 **Coded PHY** advertising + scanning driving a WS2812B strip. Each node
has a unique ID (low 32 bits of the factory MAC) and its own **chosen colour**.
Two nodes **pair** on demand; once paired, each shows the *other's* colour
whenever it is within radio range. Unpaired beacons are ignored; idle = rainbow.

### Features

- **BLE protocol — 9-byte manufacturer payload** (was 8; protocol break):
  `[0xFF,0xFF, PROTO_MAGIC(0xBE), flags, id0..3, hue]`. Byte `[8]` = the sender's
  chosen hue. `MFR_LEN=9`; peers running the older 8-byte firmware are rejected
  on RX (`mfr.size() < MFR_LEN`) and must be reflashed.
- **Extended advertising** on Coded PHY (S=8), non-connectable/non-scannable,
  1 s interval, TX +20 dBm (chip clamps).
- **Passive extended scan**, Coded PHY, duplicate filter **off** so every packet
  refreshes last-seen. Scan is **continuous small-window**: `setInterval(160)` /
  `setWindow(48)` = 30 ms RX per 100 ms (30 % duty), `start(0)`, restarted from
  `onScanEnd()`. (This is the *reverted* steady state — burst scanning was tried
  and dropped; see power doc.)
- **Device colour picker** (`runColorPicker`, blocking): on first boot (no `hue`
  key in NVS) and after a full reset, all LEDs cycle the rainbow; a short BOOT
  press captures the on-screen hue and saves it. BLE runs on its own task so the
  blocking picker doesn't stall advertising. Waits for button *release* first
  (the 10 s reset path may enter it with BOOT still held).
- **Mutual pairing** (unchanged mechanism): both nodes must be in pairing mode
  simultaneously (`FLAG_PAIRING`), 10 s window, up to `MAX_PAIRS=16`, persisted
  to NVS (`Preferences` namespace `beacon`, key `pairs`).
- **Per-peer colour display.** Proximity state is now **per-peer arrays** indexed
  1:1 with `pairedIds[]` (`peerSeenAt[]`, `peerRssi[]`, `peerHue[]`, plus
  loop-task `smoothRssi[]` EMA). `pairIndex(id)` returns the slot; `isPaired` =
  `pairIndex >= 0`. When ≥1 paired peer is present, `loop()` runs a **timed
  slideshow**: each present peer's own colour for `PEER_CYCLE_MS` (2000 ms), a
  `PEER_GAP_MS` (250 ms) blank between slots only when >1 peer is present.
  Within a slot, brightness encodes that peer's proximity — near (≥`RSSI_NEAR`
  −95 dBm) = breathe at full; far = dimmer (floor 30). No peer present → idle
  rainbow.
- **BOOT button ladder:** short (<1 s) = radio on/off; ≥1 s = pairing (yellow
  pulse); ≥10 s = forget all pairings *and* re-run the colour picker.
- **NVS:** `pairs` (paired IDs) and `hue` (device colour; its presence = the
  "provisioned" flag). Forget-all removes both, forcing re-provisioning.
- **Diagnostics:** per-packet `RX id=… rssi=… paired=… peerPairing=…` log in
  `onResult`; ~1 Hz throttled status line in `loop()`.

### Concurrency

BLE task (scan callback) writes single-word per-peer fields lock-free and queues
pairing candidates onto `pairQueue`; loop task owns all NVS writes, LED
rendering, the button, and the pair-queue drain. Paired-set membership is guarded
by the `pairMux` spinlock. `scanCallbacks` is a single reusable static instance
(NimBLE doesn't delete its callbacks; `deinit(true)` deletes the scan object).

---

## 2026-07-21 — Why ESP-IDF: the power story (summary of the full investigation)

Full detail: [ble-beacon/docs/claude-power-optimization.md](../../ble-beacon/docs/claude-power-optimization.md).
Condensed, with the facts that drive this migration:

1. **Baseline ~90 mA.** LED strip dominates, but CPU never sleeps.
2. **`esp_pm_configure()` is a confirmed no-op under `framework = arduino`.**
   That framework links **precompiled** static libs built against a fixed
   `sdkconfig` where `CONFIG_PM_ENABLE` is **unset for every target** (C3/S3/
   ESP32). No `build_flags` / `board_build.*` can change it — plain `arduino.py`
   has no sdkconfig hook at all. Verified statically (grep of the packaged
   sdkconfig) **and** at runtime — boot log printed:
   `PM config: max=80 min=40 light_sleep=1  result=ESP_ERR_NOT_SUPPORTED`.
   So there has been no DFS and no automatic light sleep the whole time.
3. **Manual `esp_light_sleep_start()` works but breaks BLE.** It doesn't need
   `CONFIG_PM_ENABLE`. Wired into a burst-scan design it dropped current
   dramatically (**~90 → ~30 mA**, ~5 mA with scanning off) — proving sleep was
   really engaging. **But** it is a *whole-chip* sleep with only a 100 ms RTC
   timer wakeup, uncoordinated with the BLE controller's own advertising
   schedule; advertising was suppressed during the sleep gap and **two paired
   nodes stopped finding each other** (confirmed dual-board: zero `RX id=…` lines
   over 8 burst windows). Reverted entirely — reliable detection beats the power
   saving.
4. **Also root-caused a separate pairing bug** along the way
   (`PAIRING_WINDOW_MS` == burst `SCAN_CYCLE_MS`, zero slack) — pairing now
   bypasses any duty cycling; but the whole burst/sleep machinery was ultimately
   removed on revert. Current state is the original continuous 30 %-duty scan,
   `TIMEOUT_MS=8000`, plain `delay(100)` loop tail. The harmless
   `esp_pm_configure()` diagnostic call is left in.

### The identified correct path → this project

The power doc's ranked "if revisited" list puts **path #1** first:

> *Fix `CONFIG_PM_ENABLE` via the hybrid `arduino, espidf` framework migration —
> the only path that gets BT-coordinated automatic light sleep, avoiding exactly
> the failure mode found here. Bigger lift, but the "correct" answer.*

**`beacon_espidf/` is that path.** With Arduino compiled as an ESP-IDF component,
`sdkconfig.defaults` is honoured, so `CONFIG_PM_ENABLE=y` etc. compile in and
`esp_pm_configure(light_sleep_enable=true)` actually engages — and because the
BT stack holds its own PM lock during active TX/RX, light sleep is coordinated
with the radio instead of blindly halting it. (Path #2, a static
`setCpuFrequencyMhz()` cut, remains an unverified lower-risk fallback.)

---

## 2026-07-21 — first clean build + firmware port (steps 2 & 1)

Both done in one session. Builds run from the PlatformIO venv
(`source ~/priv/lichterketten/plattformioVenv/bin/activate`); `pio` isn't on the
default PATH. Build/flash need the command sandbox disabled (the venv,
`~/.platformio`, and network are outside the sandbox allowlist).

### Step 2 — clean build

- **`CONTROLLER_ONLY` is a non-issue** (my earlier suspicion was wrong). The
  first build compiled Arduino-core + NimBLE-Arduino 2.5.0 + FastLED 3.10.3 +
  our code and linked *through* NimBLE — the library brings its own BLE host and
  only needs IDF's controller, so `CONFIG_BT_CONTROLLER_ONLY=y` (with both
  `BLUEDROID_ENABLED` and `NIMBLE_ENABLED` off in IDF) is correct.
- **The real blocker was the entry point:** link failed with
  `undefined reference to 'app_main'`. Arduino-as-component only defines
  `app_main()` (→ `initArduino()` + the setup/loop task) when
  **`CONFIG_AUTOSTART_ARDUINO=y`**. Added it to `sdkconfig.defaults`.
- **Gotcha — sdkconfig regeneration:** PlatformIO does NOT re-apply
  `sdkconfig.defaults` when `sdkconfig.<env>` already exists (a rebuild after the
  edit was a 10 s no-op and the option stayed unset). Fix: **delete
  `sdkconfig.beacon_c3_espidf`** to force regeneration. After that,
  `CONFIG_AUTOSTART_ARDUINO=y`, `CONFIG_PM_ENABLE=y`,
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_FREERTOS_HZ=1000` are all present
  and the build **succeeds** (RAM 6.0 %, Flash ~49.7 % of 1 MB app).
- **⚠ Power caveat found in the generated sdkconfig:** `CONFIG_BT_LE_SLEEP_ENABLE`
  (which I'd put in defaults) is **not a valid symbol** for the C3 controller and
  silently didn't apply — the C3 BLE controller modem sleep is still OFF
  (`# CONFIG_BT_CTRL_MODEM_SLEEP is not set`, `BT_CTRL_SLEEP_MODE_EFF=0`) and RTC
  runs off the internal RC (`CONFIG_ESP32C3_RTC_CLK_SRC_INT_RC=y`). Automatic
  light sleep can compile and engage, but without controller modem sleep the
  radio may keep the SoC out of deep idle — this is the key step-4 tuning lever
  (likely need `CONFIG_BT_CTRL_MODEM_SLEEP=y` + a suitable low-power clock).

### Step 1 — firmware port

- Overwrote `beacon_espidf/src/main.cpp` with `ble-beacon@nonsleep:src/main.cpp`
  (the current feature set: colour picker, per-peer colour slideshow, 9-byte
  protocol, RX debug log, continuous 30 %-duty scan).
- Rewrote the `esp_pm_configure()` comment in `setup()` for the IDF context: it's
  no longer the Arduino no-op — here it should return `OK` and actually enable
  DFS + coordinated light sleep. Also inlined the controller-modem-sleep caveat.
- Rebuild after the port: **SUCCESS** (~11 s incremental).

**Not yet done (needs hardware):** confirm the boot log prints
`PM config: … result=OK` (was `ESP_ERR_NOT_SUPPORTED` under Arduino), and the
two-board detection + current-draw verification (step 4 below). No flashing done
this session.

---

## 2026-07-21 — first hardware run: light sleep works, but corrupts the LEDs

**User flashed the ported build.** Result: **current dropped to ~30 mA** (light
sleep is genuinely engaging — the whole goal, and something the Arduino build
never achieved). But the LEDs were wrong (a 2-pixel strip showed one very bright
white pixel + one barely-visible blue) and buttons appeared dead.

**Diagnosis — `esp_pm` vs WS2812 timing (classic, expected).** FastLED clocks the
WS2812 waveform off the APB clock via the RMT peripheral. With DFS + automatic
light sleep active, the APB clock drops (or the SoC light-sleeps) *during* the
LED transmission, so the bits latch garbage — exactly the "first pixel bright
white, rest dim/wrong" signature. The buttons were almost certainly fine all
along: `loop()` was clearly running (30 mA = it's reaching its `delay(100)` and
sleeping), so button polling ran too — the press *feedback* was just invisible
because every `FastLED.show()` was corrupted.

**Fix — pin APB only around each LED write.** Added `ledPmLock`
(`ESP_PM_APB_FREQ_MAX`, created in `setup()`) and a `ledShow()` wrapper that
acquires the lock, calls `FastLED.show()`, releases. All five `FastLED.show()`
sites now go through `ledShow()`. Holding an `APB_FREQ_MAX` lock pins APB to max
*and* blocks light sleep for its (microsecond) duration, so the RMT waveform is
clean; it's released immediately after, so the idle `delay(100)` still
light-sleeps and keeps the ~30 mA. Builds clean; **awaiting a reflash to confirm
the LEDs render correctly and button feedback is visible again.**

> If corruption persists after this, escalate: (a) also disable DFS
> (`min_freq_mhz = 80`) to remove the APB-drop path entirely, and/or (b) confirm
> FastLED's `show()` is synchronous (a driver that returns before the RMT
> finishes would need the lock held slightly longer). But `APB_FREQ_MAX` around
> `show()` is the textbook fix and should suffice for a 10-pixel strip.

---

## 2026-07-21 — second hardware run: LEDs fixed; diagnosed the 90 mA / long boot

Read the actual boot log off a live board (`/dev/ttyACM0`, pyserial reset+read —
`scratchpad/capread.py`). **Serial works on the IDF build** (we read a clean log),
which settles most of the questions:

- `PM config: … result=OK` and IDF's own `pm: Frequency switching config:
  CPU_MAX: 80, APB_MAX: 80, APB_MIN: 40, Light sleep: ENABLED`. **`LED PM lock:
  created`.** The `ledShow()` fix works — LEDs render (user stopped reporting
  corruption).
- Board is provisioned (`hue=194 provisioned=1`, `loaded 1 pairing(s)`), reaches
  `BLE beacon started`, and runs `loop()` normally: `present=1/1 … showing
  0x9D818C58 hue=6` at 1 Hz, seeing its paired peer at rssi −28. **Detection
  works.** So `loop()` (and button polling) is live — the earlier "buttons dead"
  was the *broken first flash* (corrupted RMT/FastLED, loop not running right),
  not a real input bug.

**Re-reading the 30 mA → 90 mA "regression":** the 30 mA was the *broken* first
flash (FastLED/loop not fully running, radio likely not fully up). **90 mA is the
true steady-state of the design running correctly**, and it's dominated by the
**continuous BLE scan** (`scan->start(0)`, 30 % windowed, `onScanEnd` restarts
forever). Light sleep is enabled and *coordinated* now, but continuous scanning
leaves almost no idle for it to exploit — exactly the finding in
claude-power-optimization.md. Confirmed contributor: **BLE controller modem sleep
is still OFF** (`CONFIG_BT_CTRL_MODEM_SLEEP` unset), so the radio never powers
down between scan windows.

**"Long boot":** the `while (!Serial && …3000 ms)` USB-CDC wait in `setup()`.
`Serial` (HWCDC) only asserts ready when a host is *reading* the port; on battery
/ normal use it never does, so every boot stalled the full 3 s. Reduced to 500 ms.

**Variant warning is benign for us:** boot log confirms the target is genuinely
esp32c3 (RISC-V, correct `esp_pm` struct compiled, Serial working). The Arduino
"variant" only sets `pins_arduino.h` defaults, which we override (LED 10, BOOT 9);
the board's USB flags (`ARDUINO_USB_MODE=1`) apply regardless of the variant. The
"flash 4 MB vs 2 MB" warning is cosmetic — boot log shows `SPI Flash Size: 4MB`
and the partition table fits.

**Next real power lever (needs the user's current meter to validate):** enable
`CONFIG_BT_CTRL_MODEM_SLEEP=y` (+ a low-power clock — Xiao C3 has no 32 kHz
crystal, so `RTC_SLOW`/internal, with the BLE-timing-drift caveat) so the radio
sleeps between scan windows and the SoC can actually light-sleep; and/or retry
**burst scanning**, which broke detection under *manual* sleep but should be safe
now that light sleep is BT-coordinated (the whole reason for this migration).

---

## 2026-07-21 — BT modem sleep works: 90 mA → 30 mA (migration goal achieved)

Enabled BLE controller modem sleep in `sdkconfig.defaults`
(`CONFIG_BT_CTRL_MODEM_SLEEP=y`, `MODEM_SLEEP_MODE_1=y`,
`LPCLK_SEL_RTC_SLOW=y`; removed the invalid `CONFIG_BT_LE_SLEEP_ENABLE`).
Regenerated sdkconfig (delete-then-build) → `BT_CTRL_SLEEP_MODE_EFF=1`,
`SLEEP_CLOCK_EFF=3` (RTC slow). **User flashed both boards: current dropped from
~90 mA to ~30 mA with LEDs, buttons, pairing, and steady-state detection all
working.** This is the payoff of the whole migration — coordinated (BT-aware)
light sleep, which the plain-Arduino build could never compile in, and which the
earlier *manual* `esp_light_sleep_start()` attempt achieved on current but broke
detection. Here detection holds because the BT controller owns its own sleep.

The internal-RC low-power clock (`RTC_SLOW`, ~±5 %) did NOT break the
connectionless beacon in practice — adverts + passive scan tolerate loose timing.

**Flashing caveat (expected, not a bug):** once the sleeping firmware is running,
esptool auto-reset-into-bootloader fails (USB-Serial/JTAG drops off / doesn't
service the reset during light sleep — "port busy" / "No serial data received").
Reflash requires **manual bootloader entry** (hold BOOT, tap RESET). Optional dev
fix if it becomes annoying: hold a `NO_LIGHT_SLEEP` PM lock for the first ~4 s
after boot to give esptool an auto-reset window.

**Burst-scan estimate (question raised, not implemented):** the big win is already
captured by modem sleep; the radio now sleeps in the gaps of the continuous
30 %-duty scan. Burst (→~13 % duty, long contiguous off-periods) would save only
another ~5–10 mA on a minimal-LED build, and is negligible when a bright
multi-LED strip dominates current. It also re-introduces detection latency + the
pairing-window-vs-cycle bug. To bound it: measure radio-off current (short-press)
— that's the floor burst can't beat. Recommendation: skip unless hard-squeezing
battery on a minimal-LED build.

---

## 2026-07-21 — burst scanning implemented (on top of modem sleep)

User confirmed radio-off is <10 mA (so the radio adds ~20 mA at 30 mA total) and
the deployment is battery-focused + minimal-LED → worth chasing. Implemented burst
scanning in `src/main.cpp`, riding on esp_pm's *coordinated* light sleep (not the
manual sleep that broke detection before):

- Constants: `SCAN_BURST_MS=1300`, `SCAN_SLEEP_MS=8700` (~10 s cycle, ~13 % duty
  vs the old continuous 30 %). `TIMEOUT_MS` 8000 → 25000 (≈2.5 cycles, so a missed
  burst doesn't flap peer-present).
- Scan config: `setWindow(160)` == `setInterval(160)` → 100 % RX *during* a burst.
- State: `scanActive` (volatile, cleared by `onScanEnd`), `nextBurstAt`,
  `scanContinuous` (loop-only). `onScanEnd` no longer restarts — it just clears
  `scanActive`; **all scheduling lives in the loop() scan scheduler**.
- Scheduler (loop): normal = start a `SCAN_BURST_MS` burst when
  `now >= nextBurstAt`, which auto-stops → off-gap → repeat. **Pairing bypasses
  bursting and scans continuously** (`start(0)`), because a single 10 s pairing
  window can't afford to fall inside an off-gap (the documented bug). Leaving
  pairing stops the continuous scan and resumes bursts.
- Advertising stays continuous (only RX is duty-cycled). Status line now prints
  `scan=<0/1>` so bursts are visible on serial.

Builds clean. **Handed to user to flash both boards (manual bootloader) and
measure.** Expect current below the 30 mA baseline (rough target ~20–25 mA), at
the cost of up to ~10 s detection latency. Verify on serial: `scan=1` during
~1.3 s bursts and `scan=0` during ~8.7 s gaps, `RX id=…` lines land inside
bursts, `present=1/1` holds across gaps, and pairing still works (continuous scan
while pairing).

---

## 2026-07-21 — burst-scan measured: ~20 mA average (model confirmed)

User measured the burst build: ~90 mA during each ~1.3 s burst, mostly <10 mA
(occasionally 20 mA — the 1 Hz advert blips) in the gaps; pairing mode a steady
90 mA (expected — continuous scan). Average ≈ **20 mA**.

All three measured states fit one model: **avg ≈ duty × 90 mA + (1 − duty) ×
10 mA** (≈90 mA = radio actively receiving, ≈10 mA = radio off + light sleep +
minimal LEDs):
- continuous, no modem sleep: 100 % → 90 mA ✓
- continuous 30 % windowed + modem sleep: 30 % → ~34 mA (≈30 mA measured) ✓
- burst 1.3 s / 10 s (13 %): → ~20 mA measured ✓

Progression **90 → 30 → 20 mA**. Breakdown at 20 mA: ~11.7 mA is the burst,
~8.7 mA the idle floor. **The ~10 mA idle floor is the asymptote** (cost of being
powered + advertising) — can't beat it while still detecting.

Further headroom = longer off-gap only (burst must stay ≥ ADV_INTERVAL to
guarantee a catch): 20 s cycle → ~15 mA, 30 s → ~13.5 mA, 60 s → ~12 mA — strongly
diminishing, and latency grows 1:1 with the cycle. **Recommendation: stop at the
10 s / ~20 mA point** unless a battery target forces the 20 s / ~15 mA step.

Standing caveat: all radio savings are swamped once the LED strip lights up
(peer near) — real battery life is LED-dominated in any bright deployment.

---

## 2026-07-21 — review + cleanup pass

Went through `src/main.cpp` for bugs/readability/dead code:
- **Scheduler robustness (behavioral):** burst-start now gated purely on time
  instead of `!scanActive`, so a missed `onScanEnd` or a failed
  `NimBLEScan::start()` self-heals next cycle rather than wedging the scan off
  forever (which would have killed detection silently after TIMEOUT_MS).
- Added `#else #error` to the target-specific `esp_pm_config` struct.
- Fixed the now-wrong `esp_pm` comment (claimed modem sleep unset; it's enabled),
  trimmed it, and fixed stale "magenta" proximity comments + the `startBle`
  "continuous scan" header.
- Removed the per-packet `RX id=…` debug print (serial flood during bursts; the
  1 Hz status line suffices — in git history if needed).
- Left `millis()` rollover (49-day edge, self-corrects) and `smoothRssi` first
  sample (cosmetic) as-is on purpose.
Builds clean; no logic change beyond the scheduler self-heal.

---

## 2026-07-21 — false alarm: USB serial monitor inflates current (confirmed)

After the cleanup, current briefly read ~110 mA — traced to a **USB serial
monitor being attached**: an active USB-Serial/JTAG connection blocks light
sleep, so current jumps to ~90–110 mA regardless of burst mode (the USB-CDC
caveat, now confirmed empirically). Detach the monitor → back to ~20 mA. The
cleanup did NOT regress anything; the burst scheduler was confirmed intact from
the live log (`scan=` toggling ~1.3 s on / ~8.7 s off, peer caught per burst).

Diagnostic tell: the monitor repeatedly logged `Closed serial port … due to
disconnection` / reopened — that's the board dropping off USB when it deep-sleeps
in the gaps and re-enumerating on wake, i.e. **proof light sleep is engaging**.
Rule: measure current with USB serial detached.

---

## Current state of `beacon_espidf/` (as of 2026-07-21, after the above)

- **Builds clean** (`beacon_c3_espidf`). `src/main.cpp` is now feature-current
  with `ble-beacon@nonsleep`. `sdkconfig.defaults` gained
  `CONFIG_AUTOSTART_ARDUINO=y`. Added a project `.gitignore` (`.pio`,
  `build.log`, etc.).
- PlatformIO also auto-generated a root `CMakeLists.txt` for the espidf build
  (untracked) alongside the existing `src/CMakeLists.txt`.

### Original scaffolding (unchanged)

- **Scaffolding done:** `platformio.ini` `beacon_c3_espidf` env
  (`framework = espidf, arduino`, `-Wno-error=…` flags to survive IDF's
  project-wide `-Werror` firing inside vendored NimBLE/FastLED),
  `sdkconfig.defaults` (`CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`,
  `CONFIG_BT_LE_SLEEP_ENABLE`, `CONFIG_FREERTOS_HZ=1000`, `CONFIG_BT_ENABLED`,
  `CONFIG_BT_CONTROLLER_ONLY`), a generated `sdkconfig.beacon_c3_espidf`, and an
  auto-generated `src/CMakeLists.txt` (glob + `idf_component_register`).
- **Git:** single commit (`64dc559`). Essentially unversioned WIP.
- **⚠ `src/main.cpp` here is OUTDATED** — it is an *earlier* fork of the firmware
  (from around the `esp_pm`/duty-cycle era) and is **missing the colour picker
  and the per-peer colour display**: `MFR_LEN=8` (no hue byte), single
  `peerSeen`/`lastSeenMs`/`lastRSSI` scalars, no `runColorPicker`/`myHue`/
  `peerHue`/`loadColor`/`saveColor`. It does already contain the
  `esp_pm_configure()` block and the 30 % scan duty. It is **not** ~identical to
  the current `ble-beacon` file (an earlier note here wrongly said so).
- **⚠ `serial.log` is stale** — old status format, `rssi=-127 nearby=1`; predates
  even this main.cpp. Light sleep has **not** been verified/measured on the IDF
  build.

### Board notes

- Primary: **ESP32-C3** (`seeed_xiao_esp32c3`), LED GPIO 10, BOOT GPIO 9.
- **ESP32-S3** (`lolin_s3_mini`, LED 48, BOOT 0) also intended; the
  `esp_pm_config` struct already branches on `CONFIG_IDF_TARGET_ESP32S3` /
  `ESP32C3`. No `beacon_s3_espidf` env exists yet.

---

## Open questions / next steps

- [x] ~~Port the current firmware.~~ Done — `src/main.cpp` = `ble-beacon@nonsleep`.
- [x] ~~Resolve `CONFIG_BT_CONTROLLER_ONLY=y` vs NimBLE.~~ Non-issue — NimBLE
  brings its own host, links clean.
- [x] ~~Get a clean build.~~ Done via `CONFIG_AUTOSTART_ARDUINO=y` (+ delete the
  generated sdkconfig to force regeneration).

Remaining, in order:

1. **Flash + confirm PM engages (single board).** `pio run -e beacon_c3_espidf -t
   upload` (from the venv). Boot log must show `PM config: … result=OK` — the
   whole point; it was `ESP_ERR_NOT_SUPPORTED` under plain Arduino. Note serial
   is USB-CDC HWCDC on the C3 (`ARDUINO_USB_MODE=1`); reading it non-interactively
   in this env needed a pyserial DTR/RTS-reset script (see power doc), not
   `pio device monitor`.
2. **Enable BLE controller modem sleep** so light sleep can actually lower current
   without stalling the radio: set `CONFIG_BT_CTRL_MODEM_SLEEP=y` (and the
   low-power clock source it needs) in `sdkconfig.defaults`, regenerate, rebuild.
   Without this the automatic light sleep has little radio idle to exploit.
3. **Two-board end-to-end verify** — THE crux (manual sleep failed exactly here).
   Pairing succeeds, steady-state detection holds between two already-paired
   nodes, AND current drops toward the ~30 mA seen with manual sleep — *without*
   the detection breakage. A single-board "doesn't crash" test is not sufficient.
4. **USB-CDC caveat:** `ARDUINO_USB_CDC_ON_BOOT=1` may hold a PM lock / block
   light sleep while a monitor is attached — measure current with USB unplugged.
5. Refresh `serial.log` from the real IDF build; add a `beacon_s3_espidf` env if
   the S3 also needs managed sleep (the `esp_pm_config` struct already branches
   on target).
