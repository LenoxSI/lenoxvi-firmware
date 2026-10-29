<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/lenoxvi-horizontal-lime-on-dark.svg">
  <img alt="LenoxVI" src="docs/lenoxvi-horizontal-lime-on-light.svg" width="420">
</picture>

# Mic-node firmware

Firmware for the ESP32-S3 microphone nodes of [LenoxVI](https://github.com/LenoxSI/lenoxvi).
A node streams its microphone to the LenoxVI bot over WebRTC (data channel for
commands, LEDs and status) and updates itself over the air from the bot.

## Firmware lines

Each supported board is its own **line**, with one name used everywhere. All
lines build from the `main` branch; the line is chosen per build with
`-DBOARD`:

| line      | board                                    | build flag          | image                 | tags               |
|-----------|------------------------------------------|---------------------|-----------------------|--------------------|
| array     | reSpeaker XVF3800 mic array + ESP32-S3   | `-DBOARD=array`     | `array_esp32.bin`     | `array-vX.Y.Z`     |
| flex      | reSpeaker Flex (XVF3800) + XIAO ESP32-S3 | `-DBOARD=flex`      | `flex_esp32.bin`      | `flex-vX.Y.Z`      |
| waveshare | Waveshare ESP32-S3-AUDIO                 | `-DBOARD=waveshare` | `waveshare_esp32.bin` | `waveshare-vX.Y.Z` |

Never put one line's image on another line's board. Since 1.2.9 a node
refuses an over-the-air image whose project name isn't its own, and the bot
refuses uploads whose image doesn't match the chosen line.

Board notes:

- **array / flex** — the XVF3800 does the audio processing (beamforming,
  AGC, echo cancellation) and drives the array's 12-LED ring and mute button.
  Flex has no LED ring or button.
- **waveshare** (since 1.2.30) — mic only: the ES7210 ADC delivers raw,
  unprocessed audio (no AGC), so it runs quieter than the XVF lines. Its 7
  WS2812 LEDs (RGB order) show the same states as the array's ring; while
  muted, one pixel is red (the array's separate mute LED). The on-board
  speaker (ES8311) is not used yet; the amplifier is held off. This line's
  board code lives in `components/board_waveshare/` and is only built for
  `-DBOARD=waveshare`.

Other branches: `feature/nvs-config` (parked work).

## Build

Requirements: [ESP-IDF v5.3.1](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/get-started/)
and network access on the first build (libpeer and the managed components are
downloaded; the waveshare line's components are pinned in
`dependencies.lock.waveshare`). The target (`esp32s3`) and every build option come from
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
idf.py -B build/waveshare -DBOARD=waveshare build   # -> build/waveshare/waveshare_esp32.bin
```

`BOARD` is required (the build stops without it). Use one build directory per
board so a cached setting never carries over from one line to another.

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
- `MIC_LEVEL_LOG=1` (bench only, waveshare) — logs mic rms/peak to serial
  once a second for 30 s after boot, to check a board's mic. Never in a
  deployed image.

Both debug options are opt-in by value (`=1`), and the LenoxVI deploy script
refuses to publish an image built with either.

## Flashing

- **First flash of a blank board:** build, publish the image to the bot
  (`scripts/deploy_firmware.sh` or the dashboard's Firmware panel), then flash it
  over USB from the dashboard's **Node setup** page — that flashes the
  bot-signed image (see *Signed updates*). A plain `idf.py -p <port> flash`
  still boots, but the node will refuse every over-the-air update until it's
  re-flashed from Node setup.
- **Updates:** over the air through the LenoxVI bot — the dashboard's Firmware
  panel, or `scripts/deploy_firmware.sh --track array|flex|waveshare` in the
  LenoxVI repo.
  A new image boots pending verification and rolls back automatically unless
  it reaches the bot within 5 minutes.
- On array/flex, the XVF3800 audio chip runs its own XMOS firmware, flashed
  separately (see the LenoxVI repo's bring-up docs).

## Signed updates

From 1.3.0 a node installs an over-the-air update only if it's signed with the
same key as the firmware it's running (ESP-IDF "verify app signature on
update", RSA-3072, no hardware secure boot or eFuses). **The LenoxVI bot
holds the key and signs every image it serves** — you build images exactly as
before, unsigned, and never handle a key. A refused image never becomes
bootable; the node keeps running and reports `image-refused` in its OTA status.

- Nodes on 1.2.x don't check signatures, so they accept the first signed image.
- Keep a backup of the bot's key (LenoxVI README, *Security*). A bot with a
  different key can't update your nodes until each one is re-flashed over USB
  from the dashboard's Node setup page.
- Hardware secure boot and flash encryption are not used: a node someone can
  plug into is not protected by this.

## Versions

The version is `project(${BOARD}_esp32 VERSION X.Y.Z)` in `CMakeLists.txt` —
the node compares it with the bot's manifest and updates when the manifest is
newer. All lines build from the same source, so they share version numbers;
a node only updates when its own line's track serves a newer image.

## Third-party code

- `components/srtp` — [libsrtp](https://github.com/cisco/libsrtp), BSD-3-Clause
  (`components/srtp/libsrtp/LICENSE`), with an ESP-IDF port, MIT
  (`components/srtp/LICENSE`). Vendored.
- `components/esp-libopus` — the [Opus](https://opus-codec.org/) codec ported
  to ESP-IDF, BSD (`components/esp-libopus/COPYING`). Vendored.
- `deps/libpeer` — [libpeer](https://github.com/sepfy/libpeer) (MIT), fetched
  at a pinned commit and patched by `scripts/fetch_libpeer.sh`;
  `components/peer` is its ESP-IDF build glue.
- waveshare line only, downloaded by the ESP-IDF component manager (all
  Apache-2.0): `espressif/esp_codec_dev` (ES7210 driver),
  `espressif/esp_io_expander` + `esp_io_expander_tca95xx_16bit` (TCA9555),
  `espressif/led_strip` (WS2812 ring).
