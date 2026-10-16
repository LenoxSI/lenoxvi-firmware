# lenoxvi-firmware — LenoxVI mic-node firmware

Firmware for the ESP32-S3 microphone nodes of [LenoxVI](https://github.com/LenoxSI/lenoxvi).
A node streams its microphone to the LenoxVI bot over WebRTC (data channel for
commands, LEDs and status) and updates itself over the air from the bot.

## Firmware lines

Each supported board is its own **line**, with one name used everywhere. All
lines build from the `main` branch; the line is chosen per build with
`-DBOARD`:

| line  | board                                    | build flag     | image             | tags           |
|-------|------------------------------------------|----------------|-------------------|----------------|
| array | reSpeaker XVF3800 mic array + ESP32-S3   | `-DBOARD=array` | `array_esp32.bin` | `array-vX.Y.Z` |
| flex  | reSpeaker Flex (XVF3800) + XIAO ESP32-S3 | `-DBOARD=flex`  | `flex_esp32.bin`  | `flex-vX.Y.Z`  |

Never put one line's image on the other line's board. Since 1.2.9 a node
refuses an over-the-air image whose project name isn't its own, and the bot
refuses uploads whose image doesn't match the chosen line.

Other branches: `waveshare-v1` (frozen archive of the Waveshare board port),
`feature/nvs-config` (parked work).

## Build

Requirements: [ESP-IDF v5.3.1](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/get-started/)
and network access on the first build (libpeer and the managed components are
downloaded). The target (`esp32s3`) and every build option come from
`sdkconfig.defaults` — a fresh clone builds the same image as the released one.

```sh
git clone https://github.com/LenoxSI/lenoxvi-firmware.git
cd lenoxvi-firmware
./scripts/fetch_libpeer.sh          # libpeer, pinned + patched (patches/libpeer/)
. "$IDF_PATH/export.sh"

export PIPECAT_SMALLWEBRTC_URL="https://<bot-host>:7860/api/offer"
export WIFI_SSID="" WIFI_PASSWORD=""   # empty: the node reads Wi-Fi from its NVS (normal)
idf.py -B build/array -DBOARD=array build   # -> build/array/array_esp32.bin
idf.py -B build/flex  -DBOARD=flex  build   # -> build/flex/flex_esp32.bin
```

`BOARD` is required (the build stops without it). Use one build directory per
board so a cached setting never carries over from one line to the other.

Build environment variables:

- `PIPECAT_SMALLWEBRTC_URL` (required) — the bot's WebRTC offer URL. The node
  also derives its update URL from it (`/firmware/<line>/version`).
- `WIFI_SSID`, `WIFI_PASSWORD` (must be defined, normally empty) — only a
  fallback for a board whose NVS holds no Wi-Fi settings. Never set them for
  an image that will be served over the air: OTA images are readable by
  anyone on the network.
- `PIPECAT_ROOM` (optional, default `lenoxvi`) — the name a factory-fresh node
  reports; rename nodes from the LenoxVI dashboard afterwards.
- `LOG_DATACHANNEL_MESSAGES=1` (debug only) — logs every data-channel message
  to serial, including Wi-Fi settings. Never in a deployed image.

## Flashing

- **First flash of a blank board:** USB, with `WIFI_SSID`/`WIFI_PASSWORD` set
  for that one build (the firmware has no setup hotspot):
  `idf.py -p <port> flash`. Then set Wi-Fi and the node name from the LenoxVI
  dashboard; every later image can be credential-free.
- **Updates:** over the air through the LenoxVI bot — the dashboard's Firmware
  panel, or `scripts/deploy_firmware.sh --track array|flex` in the lenoxvi repo.
  A new image boots pending verification and rolls back automatically unless
  it reaches the bot within 5 minutes.
- The XVF3800 audio chip runs its own XMOS firmware, flashed separately (see
  the lenoxvi repo's bring-up docs).

## Versions

The version is `project(${BOARD}_esp32 VERSION X.Y.Z)` in `CMakeLists.txt` —
the node compares it with the bot's manifest and updates when the manifest is
newer. Both lines build from the same source, so they share version numbers.

## Third-party code

- `components/srtp` — [libsrtp](https://github.com/cisco/libsrtp), BSD-3-Clause
  (`components/srtp/libsrtp/LICENSE`), with an ESP-IDF port, MIT
  (`components/srtp/LICENSE`). Vendored.
- `components/esp-libopus` — the [Opus](https://opus-codec.org/) codec ported
  to ESP-IDF, BSD (`components/esp-libopus/COPYING`). Vendored.
- `deps/libpeer` — [libpeer](https://github.com/sepfy/libpeer) (MIT), fetched
  at a pinned commit and patched by `scripts/fetch_libpeer.sh`;
  `components/peer` is its ESP-IDF build glue.
