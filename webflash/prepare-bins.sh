#!/usr/bin/env bash
# Build the ESP32-C3 firmware and copy the flashable binaries next to the flasher
# page so index.html + manifest.json work when served locally. These .bin files
# are git-ignored — the CI workflow (.github/workflows/deploy-webflasher.yml)
# regenerates them for the deployed site.
#
# Usage (from anywhere):  webflash/prepare-bins.sh
# Then serve:             python -m http.server -d webflash 8000
#                         open http://localhost:8000
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
env="beacon_c3_espidf"

cd "$root"
pio run -e "$env"

for bin in bootloader partitions firmware; do
  cp ".pio/build/$env/$bin.bin" "$here/$bin.bin"
done

echo "Copied bootloader/partitions/firmware.bin to $here"
echo "Serve with:  python3 -m http.server -d '$here' 8000   (then open http://localhost:8000)"
