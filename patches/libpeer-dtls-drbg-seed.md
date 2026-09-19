# libpeer patch: seed DRBG before cookie_setup (mbedtls 3.x path)

`deps/libpeer` is gitignored, so this fix lives here as documentation.
Upstream: https://github.com/sepfy/libpeer (main, as of 2026-09-15).

## Why

IDF 5.3 ships mbedtls 3.6. On that path, `dtls_srtp_init()` calls
`mbedtls_ssl_cookie_setup()` immediately after `mbedtls_ctr_drbg_init()`
(line ~560). Cookie setup generates its cookie key via the RNG right away,
but upstream main only seeds the DRBG later, inside cert creation
(`create_cert()`, line ~790). An unseeded DRBG makes cookie_setup fail with
`-0x0020` = `MBEDTLS_ERR_AES_INVALID_KEY_LENGTH` — NOT an OOM, which cost
an evening to diagnose (2026-09-15, reSpeaker node bring-up, design note).

Symptom if missing: `mbedtls_ssl_cookie_setup returned -0x0020` in the boot
log, WebRTC never connects.

## Patch (deps/libpeer/src/dtls_srtp.c, in dtls_srtp_init)

Insert right after `dtls_srtp->initialized = 1;` (after the init calls,
before the version-4 psa_crypto_init block):

```c
#if MBEDTLS_VERSION_NUMBER < 0x04000000
  /* mbedtls 3.x (IDF 5.3): seed the DRBG NOW — mbedtls_ssl_cookie_setup
   * generates its cookie key via the RNG immediately, and upstream main
   * only seeds inside cert creation (which runs LATER). Unseeded DRBG =
   * cookie_setup fails with -0x0020 (AES invalid key length) and the whole
   * DTLS init dies. Found on the reSpeaker node bring-up, 2026-09-15. */
  {
    const char *pers = "dtls_srtp";
    int seed_ret = mbedtls_ctr_drbg_seed(&dtls_srtp->ctr_drbg,
                                          mbedtls_entropy_func,
                                          &dtls_srtp->entropy,
                                          (const unsigned char *)pers,
                                          strlen(pers));
    if (seed_ret != 0) {
      LOGE("mbedtls_ctr_drbg_seed (init) failed -0x%.4x", (unsigned int)-seed_ret);
      return -1;
    }
  }
#endif
```

## Apply on fresh clone

The block above is already present in the working tree's
`deps/libpeer/src/dtls_srtp.c`. If you re-clone libpeer from upstream,
re-apply this hunk before building or WebRTC will die at DTLS init.