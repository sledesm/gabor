#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <psa/crypto.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/platform_util.h>
#include "b64url.h"
#include "mem.h"
#include "pairing.h"

LOG_MODULE_REGISTER(pairing, LOG_LEVEL_INF);

#define PAIRING_VERSION 5
#define PAIRING_MODEL "hatch_link"
#define PAIRING_SUITE "p256-hkdf-sha256-aes-gcm-v1"
#define POLICY_APP "confirm_app"
#define AUTH_COMMUNITY "none"
#define RECORD_LABEL "hatch-link ble setup v1"
#define SESSION_ID_LABEL "hatch-link session id v1"

#define CLIENT_FINISHED_TIMEOUT_MS (60 * 1000)
#define CONFIRMED_TIMEOUT_MS (120 * 1000)
#define PROVISIONING_TIMEOUT_MS (120 * 1000)

#define P256_POINT 65
#define NONCE_BYTES 16
#define SESSION_ID_BYTES 16
#define TAG_BYTES 16
#define MAX_B64 4096
#define MAX_CIPHERTEXT_B64 16384
#define TO_DEVICE 0
#define FROM_DEVICE 1

enum state { IDLE, WAIT_CLIENT_FINISHED, READY, PROVISIONING };

static struct {
	char node_id[24];
	char device_id[40];
	char mac[20];
	char firmware_version[24];
	char sdk_token[128];
} s_cfg;

static K_MUTEX_DEFINE(s_lock);
static enum state s_state;
static int64_t s_deadline;
static uint32_t s_generation;
static uint8_t s_rx_key[32];
static uint8_t s_tx_key[32];
static char s_session_id_b64[32];
static uint64_t s_rx_counter;
static uint64_t s_tx_counter;

static void clear_locked(void)
{
	s_state = IDLE;
	s_deadline = 0;
	mbedtls_platform_zeroize(s_rx_key, sizeof(s_rx_key));
	mbedtls_platform_zeroize(s_tx_key, sizeof(s_tx_key));
	s_session_id_b64[0] = '\0';
	s_rx_counter = 0;
	s_tx_counter = 0;
}

static void advance_generation_locked(void)
{
	s_generation++;
	if (s_generation == 0) {
		s_generation = 1;
	}
}

static void reset_locked(void)
{
	advance_generation_locked();
	clear_locked();
}

/* Drops the keys of an expired session but keeps its generation. */
static bool expire_locked(void)
{
	if (s_state == IDLE || k_uptime_get() <= s_deadline) {
		return false;
	}
	clear_locked();
	return true;
}

void pairing_init(const char *node_id, const char *device_id, const char *mac,
		  const char *firmware_version, const char *sdk_token)
{
	snprintf(s_cfg.node_id, sizeof(s_cfg.node_id), "%s", node_id);
	snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "%s", device_id);
	snprintf(s_cfg.mac, sizeof(s_cfg.mac), "%s", mac);
	snprintf(s_cfg.firmware_version, sizeof(s_cfg.firmware_version), "%s",
		 firmware_version && *firmware_version ? firmware_version : "unknown");
	snprintf(s_cfg.sdk_token, sizeof(s_cfg.sdk_token), "%s", sdk_token ? sdk_token : "");
	psa_crypto_init();
	k_mutex_lock(&s_lock, K_FOREVER);
	reset_locked();
	k_mutex_unlock(&s_lock);
}

void pairing_reset(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	reset_locked();
	k_mutex_unlock(&s_lock);
}

void pairing_add_device_info(cJSON *obj)
{
	cJSON_AddStringToObject(obj, "device_id", s_cfg.device_id);
	cJSON_AddStringToObject(obj, "mac", s_cfg.mac);
	cJSON_AddStringToObject(obj, "model", PAIRING_MODEL);
	cJSON_AddNumberToObject(obj, "pairing_protocol", PAIRING_VERSION);
	cJSON_AddStringToObject(obj, "pairing_auth", AUTH_COMMUNITY);
	cJSON_AddNumberToObject(obj, "pairing_auth_epoch", 0);
	cJSON_AddStringToObject(obj, "pairing_policy", POLICY_APP);
}

static const char *str_item(const cJSON *obj, const char *key)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int sha256(const void *a, size_t alen, const void *b, size_t blen,
		  const void *c, size_t clen, uint8_t out[32])
{
	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	int rc = mbedtls_sha256_starts(&ctx, 0);
	rc = rc ? rc : mbedtls_sha256_update(&ctx, a, alen);
	rc = rc || !blen ? rc : mbedtls_sha256_update(&ctx, b, blen);
	rc = rc || !clen ? rc : mbedtls_sha256_update(&ctx, c, clen);
	rc = rc ? rc : mbedtls_sha256_finish(&ctx, out);
	mbedtls_sha256_free(&ctx);
	return rc;
}

/* The canonical v5 transcript for community confirm_app pairing. */
static char *build_transcript(const char *mobile_pub, const char *device_pub,
			      const char *mobile_nonce, const char *device_nonce)
{
	const char *fmt =
		"hatch-link-pairing-v5\n"
		"version=5\n"
		"initiator_role=mobile\n"
		"responder_role=link\n"
		"device_id=%s\n"
		"node_id=%s\n"
		"mac=%s\n"
		"model=" PAIRING_MODEL "\n"
		"firmware_version=%s\n"
		"selected_cipher_suite=" PAIRING_SUITE "\n"
		"pairing_auth=" AUTH_COMMUNITY "\n"
		"pairing_auth_epoch=0\n"
		"pairing_policy=" POLICY_APP "\n"
		"confirm_timeout_seconds=0\n"
		"mobile_pub=%s\n"
		"device_pub=%s\n"
		"mobile_nonce=%s\n"
		"device_nonce=%s";
	int n = snprintf(NULL, 0, fmt, s_cfg.device_id, s_cfg.node_id, s_cfg.mac,
			 s_cfg.firmware_version, mobile_pub, device_pub, mobile_nonce, device_nonce);
	char *out = mem_alloc(n + 1);
	if (out) {
		snprintf(out, n + 1, fmt, s_cfg.device_id, s_cfg.node_id, s_cfg.mac,
			 s_cfg.firmware_version, mobile_pub, device_pub, mobile_nonce, device_nonce);
	}
	return out;
}

/* Returns (mobile_tx, mobile_rx, session_id): the device decrypts with
 * mobile_tx and encrypts with mobile_rx. */
static int derive_session_keys(const uint8_t ecdh[32], const uint8_t mobile_nonce[16],
			       const uint8_t device_nonce[16], const uint8_t transcript_hash[32],
			       uint8_t mobile_tx[32], uint8_t mobile_rx[32],
			       uint8_t session_id[SESSION_ID_BYTES])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	uint8_t salt[32], secret[32], sid[32];
	int rc = sha256(mobile_nonce, 16, device_nonce, 16, transcript_hash, 32, salt);
	rc = rc ? rc : mbedtls_hkdf(md, salt, 32, ecdh, 32,
				    (const uint8_t *)RECORD_LABEL, strlen(RECORD_LABEL), secret, 32);
	rc = rc ? rc : mbedtls_hkdf_expand(md, secret, 32, (const uint8_t *)"mobile->device", 14,
					   mobile_tx, 32);
	rc = rc ? rc : mbedtls_hkdf_expand(md, secret, 32, (const uint8_t *)"device->mobile", 14,
					   mobile_rx, 32);
	rc = rc ? rc : sha256(SESSION_ID_LABEL, strlen(SESSION_ID_LABEL), transcript_hash, 32,
			      ecdh, 32, sid);
	memcpy(session_id, sid, SESSION_ID_BYTES);
	mbedtls_platform_zeroize(secret, sizeof(secret));
	return rc;
}

static void record_nonce(int direction, uint64_t counter, uint8_t nonce[12])
{
	memset(nonce, 0, 12);
	nonce[0] = (uint8_t)direction;
	for (int i = 0; i < 8; i++) {
		nonce[4 + i] = (uint8_t)(counter >> (56 - 8 * i));
	}
}

static int record_aad(int direction, uint64_t counter, char *out, size_t size)
{
	return snprintf(out, size, RECORD_LABEL "|%s|%s|%llu", s_session_id_b64,
			direction == TO_DEVICE ? "m2d" : "d2m", (unsigned long long)counter);
}

static bool hello_number_is_version(const cJSON *v)
{
	return cJSON_IsNumber(v) && v->valuedouble == PAIRING_VERSION;
}

cJSON *pairing_handle_hello(const cJSON *hello, const char **err)
{
	*err = PAIRING_ERROR_INVALID_HELLO;
	const char *auth = str_item(hello, "pairing_auth");
	const char *policy = str_item(hello, "pairing_policy");
	if (!hello_number_is_version(cJSON_GetObjectItemCaseSensitive(hello, "version")) ||
	    !auth || strcmp(auth, AUTH_COMMUNITY) != 0 || !policy || strcmp(policy, POLICY_APP) != 0) {
		return NULL;
	}

	uint8_t mobile_pub[P256_POINT + 1], mobile_nonce[NONCE_BYTES + 1];
	int pub_len = b64url_decode(str_item(hello, "mobile_pub"), MAX_B64, mobile_pub,
				    sizeof(mobile_pub));
	int nonce_len = b64url_decode(str_item(hello, "mobile_nonce"), MAX_B64, mobile_nonce,
				      sizeof(mobile_nonce));

	k_mutex_lock(&s_lock, K_FOREVER);
	reset_locked();
	cJSON *ready = NULL;
	char *transcript = NULL;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	uint8_t device_pub[P256_POINT], device_nonce[NONCE_BYTES], ecdh[32];
	size_t device_pub_len = 0, ecdh_len = 0;

	if (pub_len != P256_POINT || mobile_pub[0] != 0x04 || nonce_len != NONCE_BYTES) {
		goto out;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	if (psa_generate_key(&attr, &key) != PSA_SUCCESS ||
	    psa_export_public_key(key, device_pub, sizeof(device_pub), &device_pub_len) != PSA_SUCCESS ||
	    device_pub_len != P256_POINT ||
	    psa_generate_random(device_nonce, sizeof(device_nonce)) != PSA_SUCCESS) {
		LOG_ERR("device key generation failed");
		goto out;
	}
	/* Rejects points not on the curve, as the reference does. */
	if (psa_raw_key_agreement(PSA_ALG_ECDH, key, mobile_pub, P256_POINT, ecdh, sizeof(ecdh),
				  &ecdh_len) != PSA_SUCCESS || ecdh_len != 32) {
		goto out;
	}

	char mpub_b64[96], dpub_b64[96], mnonce_b64[32], dnonce_b64[32], hash_b64[48];
	b64url_encode(mobile_pub, P256_POINT, mpub_b64, sizeof(mpub_b64));
	b64url_encode(device_pub, P256_POINT, dpub_b64, sizeof(dpub_b64));
	b64url_encode(mobile_nonce, NONCE_BYTES, mnonce_b64, sizeof(mnonce_b64));
	b64url_encode(device_nonce, NONCE_BYTES, dnonce_b64, sizeof(dnonce_b64));
	transcript = build_transcript(mpub_b64, dpub_b64, mnonce_b64, dnonce_b64);
	if (!transcript) {
		goto out;
	}
	uint8_t transcript_hash[32], session_id[SESSION_ID_BYTES];
	if (sha256(transcript, strlen(transcript), NULL, 0, NULL, 0, transcript_hash) ||
	    derive_session_keys(ecdh, mobile_nonce, device_nonce, transcript_hash, s_rx_key,
				s_tx_key, session_id)) {
		goto out;
	}
	b64url_encode(transcript_hash, 32, hash_b64, sizeof(hash_b64));
	b64url_encode(session_id, SESSION_ID_BYTES, s_session_id_b64, sizeof(s_session_id_b64));
	s_rx_counter = 0;
	s_tx_counter = 0;
	s_state = WAIT_CLIENT_FINISHED;
	s_deadline = k_uptime_get() + CLIENT_FINISHED_TIMEOUT_MS;

	ready = cJSON_CreateObject();
	cJSON_AddStringToObject(ready, "type", "pairing_ready");
	cJSON_AddNumberToObject(ready, "version", PAIRING_VERSION);
	cJSON_AddStringToObject(ready, "device_id", s_cfg.device_id);
	cJSON_AddStringToObject(ready, "node_id", s_cfg.node_id);
	cJSON_AddStringToObject(ready, "mac", s_cfg.mac);
	cJSON_AddStringToObject(ready, "model", PAIRING_MODEL);
	cJSON_AddStringToObject(ready, "firmware_version", s_cfg.firmware_version);
	cJSON_AddStringToObject(ready, "pairing_auth", AUTH_COMMUNITY);
	cJSON_AddNumberToObject(ready, "pairing_auth_epoch", 0);
	cJSON_AddStringToObject(ready, "pairing_policy", POLICY_APP);
	cJSON_AddStringToObject(ready, "device_pub", dpub_b64);
	cJSON_AddStringToObject(ready, "device_nonce", dnonce_b64);
	cJSON_AddStringToObject(ready, "transcript_hash", hash_b64);
	cJSON_AddStringToObject(ready, "session_id", s_session_id_b64);
	*err = NULL;
out:
	if (!ready) {
		reset_locked();
	}
	if (key != PSA_KEY_ID_NULL) {
		psa_destroy_key(key);
	}
	mbedtls_platform_zeroize(ecdh, sizeof(ecdh));
	mem_free(transcript);
	k_mutex_unlock(&s_lock);
	return ready;
}

static bool parse_counter(const char *text, uint64_t *out)
{
	if (!text || !*text || strlen(text) > 20) {
		return false;
	}
	uint64_t v = 0;
	for (const char *p = text; *p; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
		uint64_t next = v * 10 + (uint64_t)(*p - '0');
		if (next / 10 != v) {
			return false; /* overflow */
		}
		v = next;
	}
	*out = v;
	return true;
}

char *pairing_decrypt(const cJSON *envelope)
{
	char *plaintext = NULL;
	uint8_t *ct = NULL;
	k_mutex_lock(&s_lock, K_FOREVER);
	if (expire_locked() || s_state == IDLE) {
		goto fail;
	}
	const char *sid = str_item(envelope, "session_id");
	uint64_t counter;
	if (!sid || strcmp(sid, s_session_id_b64) != 0 ||
	    !parse_counter(str_item(envelope, "counter"), &counter) || counter != s_rx_counter) {
		goto fail;
	}
	const char *ct_b64 = str_item(envelope, "ciphertext");
	size_t ct_cap = ct_b64 ? strlen(ct_b64) * 3 / 4 + 3 : 0;
	ct = mem_alloc(ct_cap + 1);
	plaintext = mem_alloc(ct_cap + 1);
	uint8_t tag[TAG_BYTES + 1];
	int ct_len = ct && plaintext ? b64url_decode(ct_b64, MAX_CIPHERTEXT_B64, ct, ct_cap) : -1;
	int tag_len = b64url_decode(str_item(envelope, "tag"), MAX_B64, tag, sizeof(tag));
	if (ct_len < 0 || tag_len != TAG_BYTES) {
		goto fail;
	}
	uint8_t nonce[12];
	char aad[96];
	record_nonce(TO_DEVICE, counter, nonce);
	int aad_len = record_aad(TO_DEVICE, counter, aad, sizeof(aad));
	mbedtls_gcm_context gcm;
	mbedtls_gcm_init(&gcm);
	int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_rx_key, 256);
	rc = rc ? rc : mbedtls_gcm_auth_decrypt(&gcm, ct_len, nonce, sizeof(nonce),
						(const uint8_t *)aad, aad_len, tag, TAG_BYTES,
						ct, (uint8_t *)plaintext);
	mbedtls_gcm_free(&gcm);
	if (rc) {
		goto fail;
	}
	plaintext[ct_len] = '\0';
	if (strlen(plaintext) != (size_t)ct_len) {
		goto fail; /* embedded NUL: not valid text */
	}
	s_rx_counter++;
	mem_free(ct);
	k_mutex_unlock(&s_lock);
	return plaintext;
fail:
	/* Any failure clears the session, as the reference does. */
	reset_locked();
	mem_free(ct);
	mem_free(plaintext);
	k_mutex_unlock(&s_lock);
	return NULL;
}

uint32_t pairing_handle_client_finished(const cJSON *command)
{
	const char *action = str_item(command, "action");
	bool exact = action && strcmp(action, "pairing_client_finished") == 0 &&
		     cJSON_GetArraySize(command) == 1;
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = exact && !expire_locked() && s_state == WAIT_CLIENT_FINISHED && s_rx_counter == 1;
	uint32_t gen = 0;
	if (ok) {
		advance_generation_locked();
		s_state = READY;
		s_deadline = k_uptime_get() + CONFIRMED_TIMEOUT_MS;
		gen = s_generation;
	} else {
		reset_locked();
	}
	k_mutex_unlock(&s_lock);
	return gen;
}

bool pairing_confirmed(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	expire_locked();
	bool ok = s_state == READY || s_state == PROVISIONING;
	k_mutex_unlock(&s_lock);
	return ok;
}

uint32_t pairing_mark_provisioning(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	if (!expire_locked() && s_state == READY) {
		advance_generation_locked();
		s_state = PROVISIONING;
		s_deadline = k_uptime_get() + PROVISIONING_TIMEOUT_MS;
	}
	uint32_t gen = s_state == PROVISIONING ? s_generation : 0;
	k_mutex_unlock(&s_lock);
	return gen;
}

static bool provisioning_locked(uint32_t generation)
{
	return generation != 0 && generation == s_generation && !expire_locked() &&
	       s_state == PROVISIONING;
}

bool pairing_is_provisioning(uint32_t generation)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = provisioning_locked(generation);
	k_mutex_unlock(&s_lock);
	return ok;
}

bool pairing_extend_provisioning(uint32_t generation)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = provisioning_locked(generation);
	if (ok) {
		s_deadline = k_uptime_get() + PROVISIONING_TIMEOUT_MS;
	}
	k_mutex_unlock(&s_lock);
	return ok;
}

bool pairing_commit_provisioning(uint32_t generation, bool (*commit)(void *), void *arg)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	bool ok = provisioning_locked(generation) && commit(arg);
	k_mutex_unlock(&s_lock);
	return ok;
}

char *pairing_encrypt_json(const char *plaintext, uint32_t generation)
{
	char *out = NULL;
	uint8_t *sealed = NULL;
	char *ct_b64 = NULL;
	k_mutex_lock(&s_lock, K_FOREVER);
	if ((generation && generation != s_generation) || expire_locked() || s_state == IDLE) {
		goto done;
	}
	size_t len = strlen(plaintext);
	uint64_t counter = s_tx_counter;
	uint8_t nonce[12], tag[TAG_BYTES];
	char aad[96];
	record_nonce(FROM_DEVICE, counter, nonce);
	int aad_len = record_aad(FROM_DEVICE, counter, aad, sizeof(aad));
	sealed = mem_alloc(len + 1);
	size_t ct_b64_size = len * 4 / 3 + 4;
	ct_b64 = mem_alloc(ct_b64_size);
	if (!sealed || !ct_b64) {
		goto done;
	}
	mbedtls_gcm_context gcm;
	mbedtls_gcm_init(&gcm);
	int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, s_tx_key, 256);
	rc = rc ? rc : mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce,
						 sizeof(nonce), (const uint8_t *)aad, aad_len,
						 (const uint8_t *)plaintext, sealed, TAG_BYTES, tag);
	mbedtls_gcm_free(&gcm);
	if (rc) {
		goto done;
	}
	s_tx_counter++;
	char tag_b64[32], counter_text[24];
	b64url_encode(sealed, len, ct_b64, ct_b64_size);
	b64url_encode(tag, TAG_BYTES, tag_b64, sizeof(tag_b64));
	snprintf(counter_text, sizeof(counter_text), "%llu", (unsigned long long)counter);
	cJSON *env = cJSON_CreateObject();
	cJSON_AddStringToObject(env, "type", "pairing_encrypted");
	cJSON_AddStringToObject(env, "session_id", s_session_id_b64);
	cJSON_AddStringToObject(env, "counter", counter_text);
	cJSON_AddStringToObject(env, "ciphertext", ct_b64);
	cJSON_AddStringToObject(env, "tag", tag_b64);
	out = cJSON_PrintUnformatted(env);
	cJSON_Delete(env);
done:
	k_mutex_unlock(&s_lock);
	mem_free(sealed);
	mem_free(ct_b64);
	return out;
}

char *pairing_encrypt_status(const char *status, uint32_t generation)
{
	cJSON *msg = cJSON_CreateObject();
	cJSON_AddStringToObject(msg, "type", "status");
	cJSON_AddStringToObject(msg, "status", status);
	/* Apps read only type and status, so older ones ignore the token. */
	if (s_cfg.sdk_token[0] && strcmp(status, "pairing_confirmed") == 0) {
		cJSON_AddStringToObject(msg, "sdk_token", s_cfg.sdk_token);
	}
	char *plaintext = cJSON_PrintUnformatted(msg);
	cJSON_Delete(msg);
	char *out = plaintext ? pairing_encrypt_json(plaintext, generation) : NULL;
	mem_free(plaintext);
	return out;
}
