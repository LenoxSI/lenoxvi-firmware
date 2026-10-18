set(COMMON_SRC "webrtc.cpp" "main.cpp" "http.cpp")
set(SRC_REQUIRES driver esp_wifi nvs_flash peer esp_psram esp-libopus esp_http_client json app_update esp_timer)
# Waveshare line: audio comes from the board component (set by the root
# CMakeLists as a build property — BOARD itself isn't visible here).
idf_build_get_property(lenoxvi_board LENOXVI_BOARD)
if(lenoxvi_board STREQUAL "waveshare")
  list(APPEND SRC_REQUIRES board_waveshare)
endif()

if(IDF_TARGET STREQUAL linux)
	idf_component_register(
		SRCS ${COMMON_SRC}
		REQUIRES peer esp-libopus esp_http_client json)
else()
	idf_component_register(
		SRCS ${COMMON_SRC} "wifi.cpp" "media.cpp" "rtvi.cpp" "rtvi_callbacks.cpp" "rgb.c" "ota.cpp" "settings.cpp" "cmd.cpp" "cmd_json.c" "wifi_trial.c" "boot_diag.c"
		REQUIRES ${SRC_REQUIRES})
endif()

idf_component_get_property(lib peer COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=restrict)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-truncation)
# Vendored libpeer (upstream main): switch-enum + unused-var warnings under
# -Werror=all break the build; suppress at the component level (v1 parity)
target_compile_options(${lib} PRIVATE -Wno-error=switch)
target_compile_options(${lib} PRIVATE -Wno-error=unused-but-set-variable)
target_compile_options(${lib} PRIVATE -Wno-error=incompatible-pointer-types)

idf_component_get_property(lib srtp COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=incompatible-pointer-types)

idf_component_get_property(lib esp-libopus COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=maybe-uninitialized)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-overread)
