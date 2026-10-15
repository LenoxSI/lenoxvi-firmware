cmake_minimum_required(VERSION 3.19)

if(NOT IDF_TARGET STREQUAL linux)
  if(NOT DEFINED ENV{WIFI_SSID} OR NOT DEFINED ENV{WIFI_PASSWORD})
    message(FATAL_ERROR "Env variables WIFI_SSID and WIFI_PASSWORD must be set")
  endif()

  # flex-xvf3800 branch = the flex firmware track, by definition. This
  # compile definition routes OTA to /firmware/flex/version (per-model
  # manifests, design note) — the array node's respeaker-xvf3800 branch
  # deliberately leaves it unset (byte-identical legacy /firmware/version
  # for the deployed 1.1.3 fleet). Hard-coded, not env-driven: a stale
  # CMake cache can never silently build flex firmware against the
  # array's manifest URL.
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

if(DEFINED ENV{LOG_DATACHANNEL_MESSAGES})
  add_compile_definitions(LOG_DATACHANNEL_MESSAGES="1")
endif()

add_compile_definitions(PIPECAT_SMALLWEBRTC_URL="$ENV{PIPECAT_SMALLWEBRTC_URL}")

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
project(node_esp32 VERSION 1.2.4)
