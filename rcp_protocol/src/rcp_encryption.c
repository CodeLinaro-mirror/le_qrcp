/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <rcp_crypto.h>
#include <rcp_encryption.h>
#include <rcp_log.h>

static struct {
	uint32_t priv_key_id;             /**< Our private AES key ID. */
	uint32_t shared_key_id;           /**< ID of the derived shared key. */
	uint8_t pub_key[RCP_PUBKEY_SIZE]; /**< Our public key for sharing with peer. */
	uint16_t pub_key_len;             /**< Actual length of the public key. */
	bool has_key;                     /**< Is shared key derived */
} ctx;

bool rcp_encryption_init(void)
{
	if (RCP_ENC_OK != rcp_crypto_init()) {
		RCP_LOG_ERR("Failed to initialize RCP crypto module");
		return false;
	}

	enum rcp_enc_status status =
		rcp_crypto_gen_keypair(&ctx.priv_key_id, ctx.pub_key, &ctx.pub_key_len);

	if (status != RCP_ENC_OK) {
		RCP_LOG_ERR("Failed to generate key pair for encryption");
		return false;
	}

	RCP_LOG_INF("Crypto module initialized - key pair generated");
	return true;
}

bool rcp_enc_set_peer_key(uint8_t *peer_key, uint16_t peer_key_len)
{
	if (!peer_key || peer_key_len == 0) {
		RCP_LOG_ERR("Peer public key invalid");
		return false;
	}

	enum rcp_enc_status status = rcp_crypto_get_shared_key(ctx.priv_key_id, &ctx.shared_key_id,
							       peer_key, peer_key_len);

	if (status != RCP_ENC_OK) {
		RCP_LOG_ERR("Failed to generate shared encryption key");
		return false;
	}

	ctx.has_key = true;
	RCP_LOG_INF("Peer key received - shared encryption key generated");
	return true;
}

uint16_t rcp_enc_get_pub_key(uint8_t **pub_key)
{
	if (ctx.pub_key_len) {
		*pub_key = ctx.pub_key;
		return ctx.pub_key_len;
	}
	return 0;
}

uint16_t rcp_encrypt_buf(uint8_t *buf, uint16_t buf_len, uint8_t *out, uint16_t out_size)
{
	uint16_t out_len = 0;

	if (!ctx.has_key) {
		RCP_LOG_ERR("Can't encrypt buffer, key not initialized");
		return 0;
	}

	enum rcp_enc_status status =
		rcp_crypto_encrypt_data(ctx.shared_key_id, buf, buf_len, out, out_size, &out_len);

	if (status != RCP_ENC_OK) {
		return 0;
	}
	RCP_LOG_DBG("Encrypted data in=%p(%d) out=%p(%d)", (void *)buf, buf_len, (void *)out,
		    out_len);

	return out_len;
}

uint16_t rcp_decrypt_buf(uint8_t *buf, uint16_t buf_len, uint8_t *out, uint16_t out_size)
{
	uint16_t out_len = 0;

	if (!ctx.has_key) {
		RCP_LOG_ERR("Can't decrypt buffer, key not initialized");
		return 0;
	}

	enum rcp_enc_status status =
		rcp_crypto_decrypt_data(ctx.shared_key_id, buf, buf_len, out, out_size, &out_len);

	if (status != RCP_ENC_OK) {
		return 0;
	}
	RCP_LOG_DBG("Decrypted data in=%p(%d) out=%p(%d)", (void *)buf, buf_len, (void *)out,
		    out_len);

	return out_len;
}
