/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "rcp_crypto.h"
#include <rcp_log.h>
#include <psa/crypto.h>
#include <string.h>

/** Convert PSA Crypto error code to application */
static enum rcp_enc_status psa_to_rcp(psa_status_t status)
{
	if (status != PSA_SUCCESS) {
		RCP_LOG_ERR("PSA crypto function failed: %d", (int)status);
	}
	switch (status) {
	case PSA_SUCCESS:
		return RCP_ENC_OK;
	case PSA_ERROR_INVALID_ARGUMENT:
		return RCP_ENC_ERR_INVALID;
	case PSA_ERROR_INSUFFICIENT_MEMORY:
		return RCP_ENC_ERR_MEMORY;
	default:
		return RCP_ENC_ERR;
	}
}

enum rcp_enc_status rcp_crypto_init(void)
{
	return psa_to_rcp(psa_crypto_init());
}

enum rcp_enc_status rcp_crypto_gen_keypair(uint32_t *out_id, uint8_t *pubkey, uint16_t *pubkey_len)
{
	if (!out_id || !pubkey || !pubkey_len) {
		return RCP_ENC_ERR_INVALID;
	}

	psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attrs, 256);
	psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attrs, PSA_ALG_ECDH);

	psa_key_id_t key_id;
	psa_status_t status = psa_generate_key(&attrs, &key_id);
	if (status != PSA_SUCCESS) {
		return psa_to_rcp(status);
	}

	size_t exported_len = 0;
	status = psa_export_public_key(key_id, pubkey, RCP_PUBKEY_SIZE, &exported_len);
	if (status != PSA_SUCCESS) {
		psa_destroy_key(key_id);
		return psa_to_rcp(status);
	}

	*out_id = (uint32_t)key_id;
	*pubkey_len = (uint16_t)exported_len;

	return RCP_ENC_OK;
}

enum rcp_enc_status rcp_crypto_get_shared_key(uint32_t key_id, uint32_t *out_key_id,
					      const uint8_t *peer_pubkey, uint16_t peer_pubkey_len)
{
	if (!out_key_id || !peer_pubkey || peer_pubkey_len == 0) {
		return RCP_ENC_ERR_INVALID;
	}

	psa_algorithm_t alg = PSA_ALG_KEY_AGREEMENT(PSA_ALG_ECDH, PSA_ALG_HKDF(PSA_ALG_SHA_256));

	psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;

	psa_status_t status = psa_key_derivation_setup(&op, alg);
	if (status != PSA_SUCCESS) {
		return psa_to_rcp(status);
	}

	status = psa_key_derivation_key_agreement(&op, PSA_KEY_DERIVATION_INPUT_SECRET,
						  (psa_key_id_t)key_id, peer_pubkey,
						  peer_pubkey_len);
	if (status != PSA_SUCCESS) {
		goto cleanup;
	}

	status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
						(const uint8_t *)HKDF_INFO, HKDF_INFO_LEN);
	if (status != PSA_SUCCESS) {
		goto cleanup;
	}

	/* Derive AES-GCM key */
	psa_key_attributes_t aes_attrs = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_type(&aes_attrs, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&aes_attrs, AES_KEY_BITS);
	psa_set_key_usage_flags(&aes_attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&aes_attrs, PSA_ALG_GCM);

	psa_key_id_t aes_key_id;
	status = psa_key_derivation_output_key(&aes_attrs, &op, &aes_key_id);
	if (status != PSA_SUCCESS) {
		goto cleanup;
	}

	*out_key_id = (uint32_t)aes_key_id;

	/* Destroy the private key — no longer needed */
	psa_destroy_key((psa_key_id_t)key_id);

cleanup:
	psa_key_derivation_abort(&op);
	return psa_to_rcp(status);
}

enum rcp_enc_status rcp_crypto_encrypt_data(uint32_t key_id, const uint8_t *data, uint16_t data_len,
					    uint8_t *out, uint16_t out_size, uint16_t *out_len)
{
	if (!data || !out || !out_len || data_len == 0) {
		RCP_LOG_ERR("RCP encryption - invalid usage");
		return RCP_ENC_ERR_INVALID;
	}

	if (out_size < (data_len + RCP_ENC_ENCRYPTION_OVERHEAD)) {
		RCP_LOG_ERR("Buffer for encryption to small");
		return RCP_ENC_ERR_MEMORY;
	}

	psa_status_t status = psa_generate_random(out, NONCE_SIZE);
	if (status != PSA_SUCCESS) {
		return psa_to_rcp(status);
	}

	size_t ct_len = 0;
	status = psa_aead_encrypt((psa_key_id_t)key_id, PSA_ALG_GCM, out, NONCE_SIZE, NULL, 0, data,
				  data_len, out + NONCE_SIZE, data_len + TAG_SIZE, &ct_len);

	if (status == PSA_SUCCESS) {
		*out_len = (uint16_t)(NONCE_SIZE + ct_len);
	}

	return psa_to_rcp(status);
}

enum rcp_enc_status rcp_crypto_decrypt_data(uint32_t key_id, const uint8_t *data, uint16_t data_len,
					    uint8_t *out, uint16_t out_size, uint16_t *out_len)
{
	/* Minimum length = nonce + tag + at least one byte */
	if (!data || !out || !out_len || data_len <= RCP_ENC_ENCRYPTION_OVERHEAD) {
		RCP_LOG_ERR("RCP decryption - invalid usage");
		return RCP_ENC_ERR_INVALID;
	}

	if (out_size < data_len - RCP_ENC_ENCRYPTION_OVERHEAD) {
		RCP_LOG_ERR("Buffer for decryption to small");
		return RCP_ENC_ERR_MEMORY;
	}

	const uint8_t *nonce = data;
	const uint8_t *ciphertext = data + NONCE_SIZE;
	uint16_t ct_len = data_len - NONCE_SIZE;

	size_t pt_len = 0;
	psa_status_t status = psa_aead_decrypt((psa_key_id_t)key_id, PSA_ALG_GCM, nonce, NONCE_SIZE,
					       NULL, 0, ciphertext, ct_len, out, ct_len, &pt_len);

	if (status == PSA_SUCCESS) {
		*out_len = (uint16_t)pt_len;
	}

	return psa_to_rcp(status);
}
