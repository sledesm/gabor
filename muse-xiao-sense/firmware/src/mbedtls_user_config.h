/* mbedTLS settings on top of Zephyr's: the TLS record buffer for what we send
 * can be small (requests and WebSocket frames of a few KB); the receive
 * buffer stays 16 KB because servers may send full-size records. */
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

/* At most a few PSA keys exist at once (a pairing ECDH key, Noise X25519
 * and AES keys while they are in use); the default 32 slots cost ~33 KB. */
#undef MBEDTLS_PSA_KEY_SLOT_COUNT
#define MBEDTLS_PSA_KEY_SLOT_COUNT 6
