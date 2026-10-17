cmake_minimum_required(VERSION 3.19)

# Board / firmware line (2026-10-16 branch fold): ONE source tree builds both
# lines; the line is chosen per build and is REQUIRED — there is no default:
#   idf.py -B build/array -DBOARD=array build    # XVF3800 mic-array board
#   idf.py -B build/flex  -DBOARD=flex  build    # reSpeaker Flex
# BOARD drives the image's whole identity: OTA track URL (/firmware/<board>/),
# project name (<board>_esp32 — a node refuses an OTA image with another name,
# fw M4) and the board-specific code paths (LENOXVI_BOARD_ARRAY/FLEX). Give each
# board its own build dir (-B build/<board>) so a cached value can't leak.
set(BOARD "" CACHE STRING "firmware line: array | flex")
if(NOT BOARD STREQUAL "array" AND NOT BOARD STREQUAL "flex")
  message(FATAL_ERROR "BOARD must be set: idf.py -B build/<board> -DBOARD=array|flex build (got '${BOARD}')")
endif()
add_compile_definitions(XVF_OTA_TRACK="${BOARD}")
if(BOARD STREQUAL "array")
  add_compile_definitions(LENOXVI_BOARD_ARRAY=1)
else()
  add_compile_definitions(LENOXVI_BOARD_FLEX=1)
endif()

if(NOT IDF_TARGET STREQUAL linux)
  if(NOT DEFINED ENV{WIFI_SSID} OR NOT DEFINED ENV{WIFI_PASSWORD})
    message(FATAL_ERROR "Env variables WIFI_SSID and WIFI_PASSWORD must be set")
  endif()

  add_compile_definitions(WIFI_SSID="$ENV{WIFI_SSID}")
  add_compile_definitions(WIFI_PASSWORD="$ENV{WIFI_PASSWORD}")
endif()

if(NOT DEFINED ENV{PIPECAT_SMALLWEBRTC_URL})
  message(FATAL_ERROR "Env variable PIPECAT_SMALLWEBRTC_URL must be set")
endif()

if(DEFINED ENV{PIPECAT_ROOM})
  add_compile_definitions(PIPECAT_ROOM="$ENV{PIPECAT_ROOM}")
else()
  add_compile_definitions(PIPECAT_ROOM="lenoxvi")
endif()

# Debug-only: logs EVERY data-channel message to serial — including set_wifi
# payloads (the WiFi password). Explicit opt-in by VALUE (=1): the old
# `if(DEFINED ENV{...})` was true for set-but-EMPTY, and deploy_firmware.sh
# exported it empty, so every script build shipped with it on (2026-10-13).
if("$ENV{LOG_DATACHANNEL_MESSAGES}" STREQUAL "1")
  add_compile_definitions(LOG_DATACHANNEL_MESSAGES="1")
endif()

add_compile_definitions(PIPECAT_SMALLWEBRTC_URL="$ENV{PIPECAT_SMALLWEBRTC_URL}")

# libpeer reserves a STATIC H.264 reassembly buffer (rtp.c nalu_buf,
# CONFIG_MAX_NALU_SIZE, default 100 KB) in internal .bss — on an audio-only
# node (video_codec = CODEC_NONE) it can never be used. On the array line
# it starved internal DMA RAM: hardware AES couldn't allocate its bounce
# buffer and every TLS-encrypted OTA download died at byte 0 (2026-10-15,
# array 1.2.6). One packet is plenty; rtp.c drops anything larger.
add_compile_definitions(CONFIG_MAX_NALU_SIZE=1500)

set(COMPONENTS src)
set(EXTRA_COMPONENT_DIRS "src" "components/srtp" "components/peer" "components/esp-libopus")

if(IDF_TARGET STREQUAL linux)
  add_compile_definitions(LINUX_BUILD=1)
  list(APPEND EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/protocols/linux_stubs/esp_stubs
    "components/esp-protocols/common_components/linux_compat/esp_timer"
    "components/esp-protocols/common_components/linux_compat/freertos"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${BOARD}_esp32 VERSION 1.2.23)
