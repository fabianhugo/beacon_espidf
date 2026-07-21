# Web flasher (ESP32-C3)

A browser-based installer for the ESP32-C3 beacon firmware, like WLED's. It uses
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) (the Web Serial API), so
it runs esptool **in the browser** — no local toolchain needed.

- **Browser:** Chrome or Edge on desktop (Web Serial isn't in Firefox/Safari or on
  mobile).
- **Hosting:** must be served over **HTTPS** (or `localhost`) — Web Serial requires
  a secure context.

## Files

| File | Committed? | Purpose |
|---|---|---|
| `index.html` | yes | The flasher page (install button + instructions). |
| `manifest.json` | yes | Tells ESP Web Tools the chip + flash layout. |
| `prepare-bins.sh` | yes | Builds the firmware and copies the `.bin`s here for local testing. |
| `bootloader.bin` / `partitions.bin` / `firmware.bin` | **no** (git-ignored) | Build outputs; produced locally by the script and in CI by the workflow. |

The manifest flashes the three parts **separately** (`0x0` / `0x8000` / `0x10000`)
rather than one merged image, so the NVS region (`0x9000`) is left untouched and
**saved pairings + the device colour survive a re-flash** — matching a normal
`pio run -t upload`. Choosing *Erase device* in the install dialog wipes them.

## Test locally

```sh
webflash/prepare-bins.sh                 # build + copy the .bin files here
python3 -m http.server -d webflash 8000  # localhost is a secure context
# open http://localhost:8000, click Connect & Install
```

## Deploy (GitHub Pages, manual)

Deployment is a **manually-triggered** GitHub Action —
[`.github/workflows/deploy-webflasher.yml`](../.github/workflows/deploy-webflasher.yml).
It builds the C3 firmware fresh (regenerating `sdkconfig` from
`sdkconfig.defaults`) and publishes `index.html` + `manifest.json` + the three
`.bin`s to Pages. It never runs on push or PR.

**One-time setup:** in the GitHub repo, **Settings → Pages → Build and deployment →
Source: GitHub Actions**.

**To deploy/update:** repo **Actions** tab → *Deploy web flasher* → **Run
workflow**. The published URL is printed in the run summary (and under Settings →
Pages), typically `https://<user>.github.io/<repo>/`.

## Notes

- The running firmware light-sleeps, which can stop the C3 from auto-entering the
  download bootloader. If the browser can't connect, hold **BOOT**, tap **RESET**,
  release BOOT, then retry — this is covered in the page's instructions.
- `manifest.json` is C3-only by design. To add another chip later, add a build
  entry with its `chipFamily` and parts.
