set(PEER_PROJECT_PATH "../../deps/libpeer")
file(GLOB CODES "${PEER_PROJECT_PATH}/src/*.c")

idf_component_register(
  SRCS ${CODES}
  INCLUDE_DIRS "${PEER_PROJECT_PATH}/src"
  REQUIRES mbedtls srtp json esp_netif
)

if(NOT IDF_TARGET STREQUAL linux)
  add_definitions("-DESP32 -DCONFIG_USE_LWIP=1 -DCONFIG_AUDIO_BUFFER_SIZE=8096 -DCONFIG_DATA_BUFFER_SIZE=102400 -D__BYTE_ORDER=__LITTLE_ENDIAN")
endif()

# Force ECDSA, because aiortc doesn't want to use RSA.
# STUN keepalive: a request every 5 s, resent every 1 s while unanswered
# (libpeer patch 0004), session closed after 15 s with no reply — a dead bot,
# power loss or Wi-Fi loss is noticed in ~15-20 s, and one lost packet no
# longer drops the session. Never shorten these without the resend (0004).
add_definitions("-DHTTP_DO_NOT_USE_CUSTOM_CONFIG -DMQTT_DO_NOT_USE_CUSTOM_CONFIG -DCONFIG_DTLS_USE_ECDSA=1 -DCONFIG_USE_USRSCTP=0 -DDISABLE_PEER_SIGNALING=0 -DCONFIG_STUN_KEEPALIVE_INTERVAL=5000 -DCONFIG_STUN_KEEPALIVE_TIMEOUT=15000 -DCONFIG_STUN_KEEPALIVE_RETX=1000")
