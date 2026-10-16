cmake_minimum_required(VERSION 3.19)

if(NOT IDF_TARGET STREQUAL linux)
  if(NOT DEFINED ENV{WIFI_SSID} OR NOT DEFINED ENV{WIFI_PASSWORD})
    message(FATAL_ERROR "Env variables WIFI_SSID and WIFI_PASSWORD must be set")
  endif()

  # Firmware line identity (2026-10-16): this branch IS the flex line.
  # XVF_OTA_TRACK routes OTA to /firmware/flex/version; project() below names
  # the image flex_esp32 (the node refuses an OTA image with another name — fw
  # M4). Hard-coded per branch, never env-driven: a stale CMake cache can't
  # build one line's identity into the other.
  add_compile_definitions(XVF_OTA_TRACK="flex")

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
project(flex_esp32 VERSION 1.2.9)
