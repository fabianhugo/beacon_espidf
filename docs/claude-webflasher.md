# Web flasher — notes & decisions

Browser-based firmware installer for the ESP32-C3 beacon, like WLED's, plus a
manually-triggered GitHub Action that builds + deploys it to GitHub Pages.

## 2026-07-21 — built

**Approach.** [ESP Web Tools](https://esphome.github.io/esp-web-tools/) — the same
component WLED uses. It runs esptool in the browser over the **Web Serial API**
(commonly called "WebUSB", but technically Web Serial). Chrome/Edge desktop only;
requires an HTTPS (or localhost) secure context.

**Files** (`webflash/`): `index.html` (install button + instructions),
`manifest.json`, `prepare-bins.sh` (local build helper), `README.md`. The firmware
`.bin`s are git-ignored build outputs (`webflash/*.bin` in `.gitignore`),
regenerated locally by the script and in CI by the workflow.

### Decisions

- **Separate parts, not a merged image.** `manifest.json` flashes
  `bootloader.bin` @ `0x0`, `partitions.bin` @ `0x8000`, `firmware.bin` @
  `0x10000` as three parts. A single merged image at `0x0` would span and thus
  erase the NVS partition (`0x9000`), wiping pairings + device colour on every
  web-flash. Separate parts leave NVS untouched → **pairings survive a re-flash**,
  matching `pio run -t upload`. `new_install_prompt_erase: false`. (Offsets
  confirmed via `esptool merge-bin` and the boot-log partition table; C3
  bootloader is at `0x0`, unlike classic ESP32's `0x1000`.)
- **C3-only** per the user's scope. Adding another chip later = another `builds[]`
  entry with its `chipFamily` + parts.
- **Deploy = manual GitHub Action** (`.github/workflows/deploy-webflasher.yml`),
  `on: workflow_dispatch` only — never push/PR, as requested. It installs
  PlatformIO, **regenerates `sdkconfig` from `sdkconfig.defaults`** (`rm -f
  sdkconfig.beacon_c3_espidf` first — PlatformIO won't re-apply defaults while a
  generated sdkconfig exists; this is the same gotcha from the migration doc), so
  CI builds are reproducible and authoritative. Assembles `index.html` +
  `manifest.json` + the three `.bin`s into a site dir and deploys via
  `upload-pages-artifact@v3` + `deploy-pages@v4`. PlatformIO/`~/.platformio` is
  cached keyed on `platformio.ini` + `sdkconfig.defaults`.
- **ESP Web Tools loaded from unpkg CDN** (`esp-web-tools@10`) — simplest; fine on
  the deployed HTTPS page. Could be self-hosted later if a hard offline/no-CDN
  requirement appears.

### One-time repo setup (user)

GitHub repo → **Settings → Pages → Source: GitHub Actions**. Then **Actions tab →
Deploy web flasher → Run workflow**. Published at `https://<user>.github.io/<repo>/`.

### Known caveat surfaced on the page

The running firmware light-sleeps, which can stop the C3 from auto-entering the
download bootloader (same reason CLI re-flashing needs manual bootloader entry —
see claude-espidf-migration.md). The page instructs: if connect fails, hold BOOT,
tap RESET, release BOOT, retry.

### Not tested

Browser Web Serial flashing can't be exercised headlessly here. Verified
statically: manifest is valid JSON, offsets match a successful `esptool merge-bin`,
bootloader starts with the `0xE9` ESP image magic. Needs a real Chrome + board to
confirm end-to-end.
