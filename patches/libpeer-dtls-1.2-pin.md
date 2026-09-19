# libpeer patch: pin DTLS to 1.2 (both roles) — REQUIRED with TLS 1.3 enabled

`deps/libpeer` is gitignored, so this fix lives here. Upstream:
https://github.com/sepfy/libpeer (main, as of 2026-09-19).

## Why

`sdkconfig.defaults` enables `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3=y` so the
HTTPS OTA client can talk to uvicorn (which negotiates TLSv1.3; mbedtls
without 1.3 only speaks 1.2 and the OTA version GET dies
`mbedtls_ssl_handshake -0x4290` on every boot).

But that enablement also makes the **DTLS** (WebRTC) path advertise TLS 1.3
in its ClientHello. WebRTC peers (aiortc/pipecat) only speak **DTLS 1.2**;
with 1.3 advertised the handshake degraded and the SCTP association/DCEP
never completed — the data channel died on every session ("Data channel
not established within 10s", LED messages binned, 20s watchdog reboot
loops). Symptom cluster observed 2026-09-19.

Fix: keep TLS 1.3 enabled for HTTPS, but pin the DTLS endpoint to 1.2
explicitly in `deps/libpeer/src/dtls_srtp.c`, in `dtls_srtp_init()` right
after each `mbedtls_ssl_config_defaults(...)` call (BOTH the server-role
and client-role branches):

```c
#if MBEDTLS_VERSION_NUMBER >= 0x03060000
    mbedtls_ssl_conf_min_tls_version(&dtls_srtp->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&dtls_srtp->conf, MBEDTLS_SSL_VERSION_TLS1_2);
#endif
```

## Apply on fresh clone

The hunk above is in the working tree's `deps/libpeer/src/dtls_srtp.c`
(server branch after config_defaults, client branch likewise). Re-apply
whenever libpeer is re-cloned, together with the DRBG seed patch
(`patches/libpeer-dtls-drbg-seed.md`).